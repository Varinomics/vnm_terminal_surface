#include <vnm_process_custody/owner.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace custody = vnm::process_custody;
using namespace std::chrono_literals;

namespace {

bool check(bool condition, const char* message)
{
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

struct Run
{
    custody::Duplex_channel control;
    custody::Spawn_result helper;
    bool reaped = false;
    ~Run()
    {
        control.close();
        if (helper.ok && !reaped) {
            int status;
            while (waitpid(static_cast<pid_t>(helper.process_id), &status, 0) < 0 && errno == EINTR) {}
        }
        if (helper.process_handle >= 0) close(helper.process_handle);
    }
    bool launch(const std::string& executable)
    {
        std::string error;
        custody::Duplex_channel child;
        if (!custody::Duplex_channel::create_pair(custody::k_owner_message_bytes, &control, &child, &error)) {
            std::fprintf(stderr, "owner channel: %s\n", error.c_str());
            return false;
        }
        custody::Spawn_request request;
        request.executable = executable;
        request.argv = {executable, "--control", custody::inherited_argument(0)};
        request.inherited = {{child.native(), 3}};
        request.environment = {"PATH=/usr/bin:/bin"};
        helper = custody::spawn(request);
        if (!helper.ok) std::fprintf(stderr, "helper spawn: %s\n", helper.error.c_str());
        child.close();
        return helper.ok;
    }
    bool wait()
    {
        pollfd descriptor{helper.process_handle, POLLIN, 0};
        if (poll(&descriptor, 1, 5000) != 1) return false;
        int status = 0;
        reaped = waitpid(static_cast<pid_t>(helper.process_id), &status, 0) == helper.process_id;
        return reaped;
    }
};

bool receipt(Run& run, bool* started, bool* root_exited, int* exit_code, int* master = nullptr)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        custody::Owner_event event;
        std::string error;
        const auto status = custody::receive_owner_event(run.control, &event, 100, &error);
        if (status == custody::Receive_status::TIMEOUT) continue;
        if (status != custody::Receive_status::MESSAGE) return false;
        if (event.root_reference >= 0) close(event.root_reference);
        if (event.pty_master >= 0) {
            if (master) *master = event.pty_master;
            else close(event.pty_master);
        }
        if (event.kind == custody::Owner_event_kind::STARTED) *started = true;
        if (event.kind == custody::Owner_event_kind::ROOT_EXITED) {
            *root_exited = true;
            *exit_code = event.root_exit.exit_code;
        }
        if (event.kind == custody::Owner_event_kind::TREE_SETTLEMENT &&
            event.settlement == custody::Tree_settlement::CONFIRMED) return true;
    }
    return false;
}

bool exercise(const std::string& helper, const std::string& self, const std::string& mode)
{
    Run run;
    if (!check(run.launch(helper), "helper spawn")) return false;
    custody::Owner_start_request request;
    request.process.executable = self;
    request.process.argv = {self, "--fixture", mode};
    request.process.environment = {"PATH=/usr/bin:/bin"};
    if (mode == "pty") request.mode = custody::Owner_mode::PTY;
    int report_pipe[2];
    if (pipe2(report_pipe, O_CLOEXEC) != 0) return false;
    int release_pipe[2];
    if (pipe2(release_pipe, O_CLOEXEC) != 0) return false;
    request.process.inherited = {{report_pipe[1], 4}, {release_pipe[0], 5}};
    std::string error;
    if (!custody::send_owner_start(run.control, request, &error)) return false;
    close(report_pipe[1]);
    close(release_pipe[0]);
    pid_t descendant = 0;
    pollfd report{report_pipe[0], POLLIN, 0};
    const bool reported = poll(&report, 1, 3000) == 1 &&
        read(report_pipe[0], &descendant, sizeof(descendant)) == sizeof(descendant);
    close(report_pipe[0]);
    if (!check(reported && descendant > 0, "detached fixture descendant reported")) return false;
    const int reference = custody::posix_pidfd_open(descendant);
    if (!check(reference >= 0, "fixture descendant exact reference")) return false;
    bool started = false, root_exited = false;
    int exit_code = -1, master = -1;
    bool ok = true;
    if (mode != "owner-loss") {
        ok &= write(release_pipe[1], "R", 1) == 1;
        close(release_pipe[1]);
    }
    if (mode == "cancel" || mode == "timeout") {
        ok &= custody::send_owner_stop(run.control, mode == "cancel" ? 50 : 0, &error);
    }
    if (mode == "desktop-loss") run.control.close();
    if (mode == "owner-loss") {
        custody::posix_pidfd_send_signal(run.helper.process_handle, SIGKILL);
        ok &= check(run.wait(), "lost helper reaped by designated client");
        ok &= write(release_pipe[1], "R", 1) == 1;
        close(release_pipe[1]);
        const bool confirmed = receipt(run, &started, &root_exited, &exit_code);
        ok &= check(!confirmed, "owner loss cannot fabricate settlement");
        pollfd alive{reference, POLLIN, 0};
        ok &= check(poll(&alive, 1, 0) == 0, "owner-loss fixture demonstrates surviving descendant");
        // Fixture-only cleanup uses its retained exact reference, never PGID.
        custody::posix_pidfd_send_signal(reference, SIGKILL);
    }
    else {
        if (mode != "desktop-loss") {
            ok &= check(receipt(run, &started, &root_exited, &exit_code, &master), "tree settlement receipt");
            ok &= check(started && root_exited, "workload birth and root exit are distinct events");
            if (mode == "normal" || mode == "pty") ok &= check(exit_code == 23, "original workload exit status");
        }
        ok &= check(run.wait(), "helper exits after cleanup");
    }
    pollfd ended{reference, POLLIN, 0};
    ok &= check(poll(&ended, 1, 3000) == 1, "detached descendant physically ended");
    if (master >= 0) close(master);
    close(reference);
    return ok;
}

bool abort_before_execution()
{
    int output[2];
    if (pipe2(output, O_CLOEXEC) != 0) return false;
    custody::Spawn_request request;
    request.executable = "/bin/sh";
    request.argv = {"/bin/sh", "-c", "printf reached >&4"};
    request.inherited = {{output[1], 4}};
    request.admit_execution = []() { return false; };
    const auto result = custody::spawn(request);
    close(output[1]);
    char byte;
    const auto count = read(output[0], &byte, 1);
    close(output[0]);
    return check(!result.ok && result.process_handle == custody::k_invalid_handle && count == 0,
        "rejected admission never reaches workload code and retains no child");
}

bool vacant_standard_slots(const std::string& helper, custody::Owner_mode mode)
{
    Run run;
    if (!check(run.launch(helper), "control-only helper launch")) return false;
    custody::Owner_start_request request;
    request.mode = mode;
    request.process.executable = "/bin/sh";
    request.process.argv = {"/bin/sh", "-c", "exit 23"};
    request.process.environment = {"PATH=/usr/bin:/bin"};
    std::string error;
    if (!custody::send_owner_start(run.control, request, &error)) return false;
    bool started = false;
    bool exited = false;
    int exit_code = -1;
    int master = -1;
    bool ok = check(receipt(run, &started, &exited, &exit_code, &master),
        "control-only helper confirms settlement");
    ok &= check(started && exited && exit_code == 23, "control-only workload status");
    if (mode == custody::Owner_mode::PTY) ok &= check(master >= 0, "control-only PTY transfer");
    if (master >= 0) close(master);
    run.control.close();
    if (!run.wait()) {
        custody::posix_pidfd_send_signal(run.helper.process_handle, SIGKILL);
        ok = false;
        (void)run.wait();
    }
    return ok;
}

int fixture(const std::string& mode)
{
    const pid_t child = fork();
    if (child < 0) return 3;
    if (child == 0) {
        if (setsid() < 0) _exit(4);
        const pid_t self = getpid();
        if (write(4, &self, sizeof(self)) != sizeof(self)) _exit(5);
        close(4);
        close(5);
        while (true) pause();
    }
    close(4);
    char release;
    if (read(5, &release, 1) != 1) return 6;
    close(5);
    if (mode == "normal" || mode == "pty" || mode == "owner-loss") {
        return 23;
    }
    while (true) pause();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--fixture") return fixture(argv[2]);
    if (argc != 2) return 2;
    std::string error;
    if (!custody::prepare_parent_process(&error)) return 2;
    bool ok = abort_before_execution();
    for (auto mode : {custody::Owner_mode::COMMAND, custody::Owner_mode::PTY}) {
        const bool passed = vacant_standard_slots(argv[1], mode);
        std::printf("vacant-stdio-%s: %s\n", mode == custody::Owner_mode::PTY ? "pty" : "command",
            passed ? "PASS" : "FAIL");
        ok &= passed;
    }
    for (const std::string mode : {"normal", "cancel", "timeout", "desktop-loss", "owner-loss", "pty"}) {
        const bool passed = exercise(argv[1], argv[0], mode);
        std::printf("%s: %s\n", mode.c_str(), passed ? "PASS" : "FAIL");
        ok &= passed;
    }
    return ok ? 0 : 1;
}
