#include <vnm_process_custody/owner.h>

#include <fcntl.h>
#include <libproc.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <string>

namespace custody = vnm::process_custody;
using Clock = std::chrono::steady_clock;

namespace {

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

bool read_exact(int descriptor, void* destination, std::size_t size)
{
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    auto* bytes = static_cast<std::uint8_t*>(destination);
    while (size != 0 && Clock::now() < deadline) {
        pollfd ready{descriptor, POLLIN, 0};
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        const int polled = poll(&ready, 1, remaining.count() > 0 ? static_cast<int>(remaining.count()) : 0);
        if (polled < 0 && errno == EINTR) {
            continue;
        }
        if (polled != 1) {
            return false;
        }
        const ssize_t count = read(descriptor, bytes, size);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        bytes += count;
        size -= static_cast<std::size_t>(count);
    }
    return size == 0;
}

custody::Native_process_status await_status(custody::Native_process_status_observer& observer)
{
    pollfd event{observer.native_handle(), POLLIN, 0};
    int ready;
    do {
        ready = poll(&event, 1, 5000);
    }
    while (ready < 0 && errno == EINTR);
    return observer.observe();
}

class Process_observer
{
public:
    ~Process_observer() { custody::close_native_handle(m_handle); }

    bool open(std::int64_t pid, std::uint64_t birth_token, std::string* error)
    {
        m_handle = custody::open_process_observer(pid, birth_token, error);
        return m_handle != custody::k_invalid_handle;
    }

    bool live() const
    {
        pollfd event{m_handle, POLLIN, 0};
        return m_handle != custody::k_invalid_handle && poll(&event, 1, 0) == 0;
    }

    bool ended() const
    {
        pollfd event{m_handle, POLLIN, 0};
        return m_handle != custody::k_invalid_handle && poll(&event, 1, 5000) == 1 &&
            (event.revents & POLLIN) != 0;
    }

private:
    custody::Native_handle m_handle = custody::k_invalid_handle;
};

class Owner_run
{
public:
    ~Owner_run()
    {
        channel.close();
        if (m_helper.process_id > 0) {
            pollfd ended{m_helper.process_handle, POLLIN, 0};
            if (poll(&ended, 1, 3000) != 1) {
                // This fixture is the sole waiter of its unreaped direct child.
                (void)kill(static_cast<pid_t>(m_helper.process_id), SIGKILL);
            }
            int status = 0;
            (void)collect(&status);
        }
        custody::close_native_handle(m_helper.process_handle);
    }

    bool launch(const std::string& executable)
    {
        custody::Duplex_channel child;
        std::string error;
        if (!custody::Duplex_channel::create_pair(custody::k_owner_message_bytes, &channel, &child, &error)) {
            return check(false, error.c_str());
        }
        const int null_stream = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (!check(null_stream >= 0, "helper standard streams open")) {
            return false;
        }
        custody::Spawn_request request;
        request.executable  = executable;
        request.argv        = {executable, "--control", custody::inherited_argument(0),
            custody::k_darwin_pty_owner_argument};
        request.inherited   = {{child.native(), 3}, {null_stream, 0}, {null_stream, 1}, {null_stream, 2}};
        request.environment = {"PATH=/usr/bin:/bin"};
        request.new_session = true;
        m_helper = custody::spawn(request);
        close(null_stream);
        child.close();
        if (!check(m_helper.ok, m_helper.error.c_str())) {
            return false;
        }
        return check(m_status.open(m_helper.process_id, m_helper.process_birth_token, &error) &&
            m_status.observe().state == custody::Native_process_observation::LIVE,
            "exact admitted helper status observer is retained before START");
    }

    bool kill_original_helper()
    {
        return check(kill(static_cast<pid_t>(m_helper.process_id), SIGKILL) == 0,
            "sole parent kills its original unreaped helper");
    }

    bool expect_exit(custody::Darwin_pty_owner_exit expected)
    {
        const auto observed = await_status(m_status);
        int status = 0;
        const bool collected = observed.state == custody::Native_process_observation::ENDED && collect(&status);
        std::fprintf(stderr, "PTY_OWNER_EXIT expected=%d state=%d status_valid=%d observed_status=%d collected=%d wait_status=%d\n",
            static_cast<int>(expected), static_cast<int>(observed.state), observed.status_valid,
            observed.wait_status, collected, status);
        return check(collected && observed.state == custody::Native_process_observation::ENDED &&
            observed.status_valid && WIFEXITED(observed.wait_status) &&
            WEXITSTATUS(observed.wait_status) == static_cast<int>(expected) &&
            status == observed.wait_status,
            "preregistered helper status and sole parent wait agree on the exact typed outcome");
    }

    bool live() { return m_status.observe().state == custody::Native_process_observation::LIVE; }

    bool expect_signal_death()
    {
        const auto observed = await_status(m_status);
        int status = 0;
        const bool collected = observed.state == custody::Native_process_observation::ENDED && collect(&status);
        std::fprintf(stderr, "PTY_OWNER_SIGNAL expected=%d state=%d status_valid=%d observed_status=%d collected=%d wait_status=%d\n",
            SIGKILL, static_cast<int>(observed.state), observed.status_valid, observed.wait_status, collected, status);
        return check(collected && observed.state == custody::Native_process_observation::ENDED &&
            observed.status_valid && WIFSIGNALED(observed.wait_status) &&
            WTERMSIG(observed.wait_status) == SIGKILL && status == observed.wait_status,
            "helper signal death retains exact negative evidence and has no typed positive outcome");
    }

    custody::Duplex_channel channel;

private:
    bool collect(int* status)
    {
        pid_t result;
        do {
            result = waitpid(static_cast<pid_t>(m_helper.process_id), status, 0);
        }
        while (result < 0 && errno == EINTR);
        if (result != m_helper.process_id) {
            return false;
        }
        m_helper.process_id = 0;
        return true;
    }

    custody::Spawn_result m_helper;
    custody::Native_process_status_observer m_status;
};

struct fixture_ready_t
{
    pid_t root = 0;
    pid_t holder = 0;
    pid_t zombie = 0;
    pid_t original_group = 0;
    int child_status = 0;
};

class Fixture_control
{
public:
    bool prepare(custody::Spawn_request& request)
    {
        std::string error;
        if (!custody::create_pipe(&m_release_read, &m_release_write, &error) ||
            !custody::create_pipe(&m_receipt_read, &m_receipt_write, &error))
        {
            return check(false, error.c_str());
        }
        request.inherited = {{m_release_read.native(), 9}, {m_receipt_write.native(), 10}};
        return true;
    }

    bool ready(std::int64_t root, bool held_zombie)
    {
        m_release_read.close();
        m_receipt_write.close();
        fixture_ready_t ready;
        if (!check(read_exact(m_receipt_read.native(), &ready, sizeof(ready)) &&
            ready.root == root && ready.original_group == root,
            "original PTY root fixture reports its retained process group"))
        {
            return false;
        }
        if (!held_zombie) {
            return true;
        }
        proc_bsdinfo info{};
        std::string error;
        return check(ready.holder > 1 && ready.zombie > 1 && ready.child_status == CLD_EXITED &&
            proc_pidinfo(ready.holder, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info) &&
            info.pbi_pid == ready.holder && info.pbi_pgid == ready.holder &&
            m_holder.open(ready.holder, info.pbi_start_tvsec * 1'000'000 + info.pbi_start_tvusec, &error) &&
            holding(), "detached fixture holder retains an unreaped zombie in the original terminal group");
    }

    bool holding()
    {
        return m_holder.live();
    }

    void release() { m_release_write.close(); }

    bool release_held_zombie()
    {
        // Only the holder waits its child. EOF exercises the fixture's private
        // release path without granting numeric signal authority to this test.
        release();
        char receipt = 0;
        const bool collected = read_exact(m_receipt_read.native(), &receipt, 1) && receipt == 'R';
        return check(collected && m_holder.ended(),
            "holder alone collects its exact zombie and then ends after private release");
    }

private:
    custody::Readable_pipe m_release_read;
    custody::Writable_pipe m_release_write;
    custody::Readable_pipe m_receipt_read;
    custody::Writable_pipe m_receipt_write;
    Process_observer m_holder;
};

custody::Owner_start_request fixture_request(const std::string& self, const char* mode)
{
    custody::Owner_start_request request;
    request.mode = custody::Owner_mode::PTY;
    request.process.executable  = self;
    request.process.argv        = {self, "--fixture", mode};
    request.process.environment = {"PATH=/usr/bin:/bin"};
    return request;
}

bool start_root(Owner_run& run, const custody::Owner_start_request& request,
    Process_observer& root, std::int64_t& root_pid, int* retained_master = nullptr)
{
    std::string error;
    if (!check(custody::send_owner_start(run.channel, request, &error), error.c_str())) {
        return false;
    }
    custody::Owner_event event;
    const auto received = custody::receive_owner_event(run.channel, &event, 3000, &error);
    const bool started = received == custody::Receive_status::MESSAGE &&
        event.kind == custody::Owner_event_kind::STARTED && event.root_exit.process_id > 1 &&
        event.root_birth_token != 0 && event.pty_master >= 0 &&
        event.cleanup_scope == custody::Cleanup_scope::OWNED_PROCESS_GROUPS;
    const bool observed = started && root.open(event.root_exit.process_id, event.root_birth_token, &error);
    root_pid = event.root_exit.process_id;
    custody::close_native_handle(event.root_reference);
    if (retained_master) *retained_master = event.pty_master;
    else custody::close_native_handle(event.pty_master);
    return check(observed, "STARTED supplies exact original root identity and a live local observer");
}

bool pre_start_eof(const std::string& helper)
{
    Owner_run run;
    if (!run.launch(helper)) {
        return false;
    }
    run.channel.close();
    return run.expect_exit(custody::Darwin_pty_owner_exit::NO_WORKLOAD);
}

bool eof_or_helper_death(const std::string& helper, const std::string& self, bool helper_death)
{
    Owner_run run;
    Fixture_control fixture;
    auto request = fixture_request(self, "release");
    if (!fixture.prepare(request.process) || !run.launch(helper)) {
        return false;
    }
    Process_observer root;
    std::int64_t root_pid = 0;
    if (!start_root(run, request, root, root_pid) || !fixture.ready(root_pid, false)) {
        return false;
    }
    bool ok = true;
    if (helper_death) {
        ok &= run.kill_original_helper();
        ok &= run.expect_signal_death();
        custody::Owner_event event;
        std::string error;
        const auto received = custody::receive_owner_event(run.channel, &event, 1000, &error);
        custody::close_native_handle(event.root_reference);
        custody::close_native_handle(event.pty_master);
        ok &= check(received != custody::Receive_status::MESSAGE &&
            root.live(),
            "helper death supplies no settlement receipt while the original HUP-ignoring root survives");
        fixture.release();
    }
    else {
        run.channel.close();
        ok &= run.expect_exit(custody::Darwin_pty_owner_exit::CONFIRMED);
    }
    ok &= check(root.ended(),
        "the original root ends after owner EOF cleanup or the fixture's private release");
    return ok;
}

bool held_zombie_unconfirmed(const std::string& helper, const std::string& self)
{
    Owner_run run;
    Fixture_control fixture;
    auto request = fixture_request(self, "held-zombie");
    if (!fixture.prepare(request.process) || !run.launch(helper)) {
        return false;
    }
    Process_observer root;
    std::int64_t root_pid = 0;
    if (!start_root(run, request, root, root_pid) || !fixture.ready(root_pid, true)) {
        return false;
    }
    std::string error;
    bool ok = check(custody::send_owner_stop(run.channel, 0, &error), "held-zombie stop is delivered");
    bool root_ended = false;
    bool terminal = false;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!terminal && Clock::now() < deadline) {
        custody::Owner_event event;
        const auto received = custody::receive_owner_event(run.channel, &event, 100, &error);
        if (received == custody::Receive_status::TIMEOUT) {
            continue;
        }
        custody::close_native_handle(event.root_reference);
        custody::close_native_handle(event.pty_master);
        if (!check(received == custody::Receive_status::MESSAGE, "held-zombie owner receipt is available")) {
            ok = false;
            break;
        }
        ok &= check(event.cleanup_scope == custody::Cleanup_scope::OWNED_PROCESS_GROUPS,
            "every cleanup receipt preserves the adopted group scope");
        if (event.kind == custody::Owner_event_kind::ROOT_EXITED) {
            ok &= check(!root_ended && event.root_exit.process_id == root_pid && event.root_exit.signal == SIGKILL,
                "exact original root exit is reported once before terminal settlement");
            root_ended = true;
        }
        if (event.kind == custody::Owner_event_kind::TREE_SETTLEMENT &&
            event.settlement != custody::Tree_settlement::PENDING)
        {
            terminal = true;
            ok &= check(root_ended && event.settlement == custody::Tree_settlement::UNCONFIRMED && fixture.holding(),
                "held zombie prevents positive settlement after exact root exit");
        }
    }
    ok &= check(terminal, "held-zombie cleanup reaches a bounded terminal outcome");
    ok &= check(run.channel.shutdown_send(), "client retires PTY output after terminal settlement");
    ok &= run.expect_exit(custody::Darwin_pty_owner_exit::UNCONFIRMED);
    ok &= check(fixture.holding(), "typed unconfirmed outcome leaves fixture holder under its own authority");
    ok &= fixture.release_held_zombie();
    return ok;
}

bool retained_output(const std::string& helper, const std::string& self, bool abandon)
{
    Owner_run run;
    Fixture_control fixture;
    auto request = fixture_request(self, "retained-tail");
    if (!fixture.prepare(request.process) || !run.launch(helper)) return false;
    Process_observer root;
    std::int64_t root_pid = 0;
    int master = -1;
    if (!start_root(run, request, root, root_pid, &master) || !fixture.ready(root_pid, false)) return false;
    fixture.release();
    bool settled = false;
    bool root_ended = false;
    std::string error;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!settled && Clock::now() < deadline) {
        custody::Owner_event event;
        const auto received = custody::receive_owner_event(run.channel, &event, 100, &error);
        custody::close_native_handle(event.root_reference);
        custody::close_native_handle(event.pty_master);
        if (received == custody::Receive_status::TIMEOUT) continue;
        if (received != custody::Receive_status::MESSAGE) break;
        if (event.kind == custody::Owner_event_kind::ROOT_EXITED) {
            root_ended = event.root_exit.exit_code == 23 && event.root_exit.signal == 0;
        }
        settled = event.kind == custody::Owner_event_kind::TREE_SETTLEMENT &&
            event.settlement == custody::Tree_settlement::CONFIRMED;
    }
    int queued = -1;
    bool ok = check(settled && root_ended && run.live() &&
        ioctl(master, TIOCOUTQ, &queued) == 0 && queued == 512,
        "settled helper stays alive with accepted stopped output until client retirement");
    if (abandon) {
        run.channel.close();
    }
    else {
        char output[512];
        ok &= check(ioctl(master, TIOCSTART) == 0 && read_exact(master, output, sizeof(output)) &&
            std::string(output, sizeof(output)) == std::string(sizeof(output), 'R'),
            "master resumes and delivers the retained accepted queue after settlement");
        const int duplicate = dup(run.channel.native());
        ok &= check(duplicate >= 0 && run.channel.shutdown_send(), "half-close retires all client endpoint copies");
        ok &= run.expect_exit(custody::Darwin_pty_owner_exit::CONFIRMED);
        if (duplicate >= 0) close(duplicate);
    }
    if (abandon) ok &= run.expect_exit(custody::Darwin_pty_owner_exit::CONFIRMED);
    close(master);
    return ok;
}

int run_fixture(const std::string& mode)
{
    (void)signal(SIGHUP, SIG_IGN);
    (void)alarm(12);
    const pid_t root = getpid();
    if (mode == "release" || mode == "retained-tail") {
        const fixture_ready_t ready{root, 0, 0, getpgrp(), 0};
        if (write(10, &ready, sizeof(ready)) != sizeof(ready)) {
            return 31;
        }
        char byte = 0;
        if (read(9, &byte, 1) != 0) return 32;
        if (mode == "retained-tail") {
            const std::string tail(512, 'R');
            if (tcflow(STDOUT_FILENO, TCOOFF) != 0 ||
                write(STDOUT_FILENO, tail.data(), tail.size()) != static_cast<ssize_t>(tail.size())) return 40;
        }
        return 23;
    }
    if (mode != "held-zombie") {
        return 33;
    }
    const pid_t holder = fork();
    if (holder < 0) {
        return 34;
    }
    if (holder == 0) {
        const pid_t child = fork();
        if (child < 0) {
            _exit(35);
        }
        if (child == 0) {
            _exit(0);
        }
        siginfo_t info{};
        if (waitid(P_PID, static_cast<id_t>(child), &info, WEXITED | WNOWAIT) != 0) {
            _exit(36);
        }
        const pid_t group = getpgrp();
        if (setsid() < 0) {
            _exit(37);
        }
        const fixture_ready_t ready{root, getpid(), child, group, info.si_code};
        if (write(10, &ready, sizeof(ready)) != sizeof(ready)) {
            _exit(38);
        }
        pollfd release{9, POLLIN, 0};
        (void)poll(&release, 1, 8000);
        int status = 0;
        const bool collected = waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        const char receipt = collected ? 'R' : 'E';
        (void)write(10, &receipt, 1);
        _exit(collected ? 0 : 39);
    }
    close(9);
    close(10);
    for (;;) {
        pause();
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--fixture") {
        return run_fixture(argv[2]);
    }
    if (argc != 2) {
        return 2;
    }
    std::string error;
    if (!custody::prepare_parent_process(&error)) {
        return 2;
    }
    bool ok = pre_start_eof(argv[1]);
    ok &= eof_or_helper_death(argv[1], argv[0], false);
    ok &= eof_or_helper_death(argv[1], argv[0], true);
    ok &= held_zombie_unconfirmed(argv[1], argv[0]);
    ok &= retained_output(argv[1], argv[0], false);
    ok &= retained_output(argv[1], argv[0], true);
    return ok ? 0 : 1;
}
