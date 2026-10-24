#include <vnm_process_custody/owner.h>

#include "../src/owner_protocol.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace custody = vnm::process_custody;
using namespace std::chrono_literals;

namespace {

bool check(bool condition, const char* message)
{
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

bool started_descriptor_contract()
{
    custody::Duplex_channel sender, receiver;
    std::string error;
    if (!check(custody::Duplex_channel::create_pair(
            custody::k_owner_message_bytes, &sender, &receiver, &error),
        "STARTED descriptor fixture opens a native control channel"))
    {
        return false;
    }
    custody::Owner_event started;
    started.kind = custody::Owner_event_kind::STARTED;
    started.root_exit.process_id = getpid();
    if (!check(custody::detail::send_event(sender, started, &error),
        "STARTED without a descriptor reaches the receiver"))
    {
        return false;
    }
    custody::Owner_event received;
    bool ok = check(custody::receive_owner_event(receiver, &received, 1000, &error) ==
            custody::Receive_status::FAILED,
        "Linux STARTED without its process reference is a protocol failure");
    custody::close_native_handle(received.root_reference);
    custody::close_native_handle(received.pty_master);

    started.root_reference = custody::posix_pidfd_open(getpid());
    if (!check(started.root_reference >= 0, "valid STARTED fixture retains its own process reference")) {
        return false;
    }
    const bool sent = custody::detail::send_event(sender, started, &error);
    custody::close_native_handle(started.root_reference);
    if (!check(sent, "valid STARTED transfers its process reference")) {
        return false;
    }
    received = {};
    ok &= check(custody::receive_owner_event(receiver, &received, 1000, &error) ==
            custody::Receive_status::MESSAGE && received.kind == custody::Owner_event_kind::STARTED &&
            received.root_exit.process_id == getpid() && received.root_reference >= 0,
        "Linux STARTED with its process reference remains valid");
    ok &= check(received.root_reference >= 0 &&
            custody::posix_pidfd_send_signal(received.root_reference, 0) == 0,
        "received process reference names the live fixture occurrence");
    custody::close_native_handle(received.root_reference);
    custody::close_native_handle(received.pty_master);
    return ok;
}

bool native_receive_error_contract(bool fragmented)
{
    custody::Duplex_channel sender, receiver;
    std::string error;
    if (!check(custody::Duplex_channel::create_pair(
            custody::k_owner_message_bytes, &sender, &receiver, &error),
        "receive-error fixture opens a native control channel"))
    {
        return false;
    }
    if (fragmented) {
        const std::uint8_t first_fragment[] = {
            0, 0, 0, 1, // wire revision
            0, 0, 0, 5, // fragment kind
            0, 0, 0, 2, // total assembled bytes
            0, 0, 0, 0, // initial offset
            0,         // first of the two assembled bytes
        };
        if (!check(sender.send(first_fragment, sizeof(first_fragment)) == custody::Io_status::TRANSFERRED,
            "partial owner fragment reaches the receiver"))
        {
            return false;
        }
    }
    // The native frame announces one payload byte but carries none. Exercise
    // both the initial read and continuation, before the owner decoder.
    const std::uint8_t truncated_frame[] = {0, 0, 0, 1};
    if (!check(::send(sender.native(), truncated_frame, sizeof(truncated_frame), MSG_NOSIGNAL) ==
            sizeof(truncated_frame), "malformed native frame reaches the receiver"))
    {
        return false;
    }
    custody::Owner_event event;
    const auto status = custody::receive_owner_event(receiver, &event, 1000, &error);
    return check(status == custody::Receive_status::FAILED &&
            error.find(std::to_string(EPROTO)) != std::string::npos,
        "owner protocol retains the native receive error");
}

bool terminal_channel_retirement()
{
    custody::Duplex_channel owner, client;
    std::string error;
    if (!check(custody::Duplex_channel::create_pair(64, &owner, &client, &error),
        "terminal channel fixture opens its endpoints"))
    {
        return false;
    }
    int pipe[2];
    if (!check(pipe2(pipe, O_CLOEXEC | O_NONBLOCK) == 0, "terminal channel descriptor canary")) {
        return false;
    }
    const std::uint8_t reply[] = {7, 11};
    const std::uint8_t pending[] = {13};
    bool ok = check(owner.send(reply, sizeof(reply)) == custody::Io_status::TRANSFERRED,
        "final reply is queued before input retirement");
    ok &= check(client.send_descriptors(pending, sizeof(pending), {pipe[1]}) == custody::Io_status::TRANSFERRED,
        "late input carries a descriptor into the retiring endpoint");
    close(pipe[1]);
    ok &= check(owner.finish_receiving(), "terminal input is sealed and drained");
    ok &= check(client.send(pending, sizeof(pending)) == custody::Io_status::CLOSED,
        "no command can arrive after input retirement");
    owner.close();

    std::uint8_t received[64];
    const auto result = client.receive(received, sizeof(received), 1000);
    ok &= check(result.status == custody::Receive_status::MESSAGE && result.size == sizeof(reply) &&
            received[0] == reply[0] && received[1] == reply[1],
        "failed late send and owner close preserve the real queued reply");
    char byte;
    ok &= check(read(pipe[0], &byte, 1) == 0, "discarded input retains no transferred descriptor");
    close(pipe[0]);
    return ok;
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

bool receipt(Run& run, bool* started, bool* root_exited, int* exit_code,
    int* master = nullptr, bool* cpu_admitted = nullptr)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        custody::Owner_event event;
        std::string error;
        const auto status = custody::receive_owner_event(run.control, &event, 100, &error);
        if (status == custody::Receive_status::TIMEOUT) continue;
        if (status != custody::Receive_status::MESSAGE) {
            std::fprintf(stderr, "owner receipt failed: status=%d error=%s started=%d root_exited=%d\n",
                static_cast<int>(status), error.c_str(), *started, *root_exited);
            return false;
        }
        if (event.root_reference >= 0) close(event.root_reference);
        if (event.pty_master >= 0) {
            if (master) *master = event.pty_master;
            else close(event.pty_master);
        }
        if (event.kind == custody::Owner_event_kind::STARTED) {
            *started = true;
            if (cpu_admitted) {
                *cpu_admitted = event.cpu_accounting_admitted;
            }
        }
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
    if (mode == "late-stop") {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!root_exited && std::chrono::steady_clock::now() < deadline) {
            custody::Owner_event event;
            const auto status = custody::receive_owner_event(run.control, &event, 100, &error);
            if (status == custody::Receive_status::TIMEOUT) continue;
            if (status != custody::Receive_status::MESSAGE) break;
            custody::close_native_handle(event.root_reference);
            custody::close_native_handle(event.pty_master);
            if (event.kind == custody::Owner_event_kind::STARTED) started = true;
            if (event.kind == custody::Owner_event_kind::ROOT_EXITED) {
                root_exited = true;
                exit_code = event.root_exit.exit_code;
            }
            if (event.kind == custody::Owner_event_kind::TREE_SETTLEMENT) break;
        }
        pollfd alive{reference, POLLIN, 0};
        const bool retiring = started && root_exited && exit_code == 23 && poll(&alive, 1, 0) == 0;
        ok &= check(retiring, "late STOP fixture prerequisite: normal root exit with a living descendant");
        if (retiring) {
            ok &= check(custody::send_owner_stop(run.control, 0, &error),
                "STOP is accepted while normal-root cleanup owns the descendant");
        }
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
            if (mode == "normal" || mode == "pty" || mode == "late-stop")
                ok &= check(exit_code == 23, "original workload exit status");
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

bool large_bounded_start(const std::string& helper)
{
    Run run;
    if (!check(run.launch(helper), "large-start helper spawn")) {
        return false;
    }
    custody::Owner_start_request request;
    request.mode = custody::Owner_mode::PTY;
    request.process.executable = "/bin/sh";
    request.process.argv = {"/bin/sh", "-c", "test \"${#ENTRY_0}\" -eq 32768 && exit 23"};
    for (int index = 0; index != 16; ++index) {
        request.process.environment.push_back(
            "ENTRY_" + std::to_string(index) + "=" + std::string(32768, 'x'));
    }
    const int original_flags = ::fcntl(run.control.native(), F_GETFL, 0);
    if (!check(original_flags >= 0 &&
            ::fcntl(run.control.native(), F_SETFL, original_flags | O_NONBLOCK) == 0,
            "large-start control channel becomes nonblocking"))
    {
        return false;
    }
    std::string error;
    const bool sent = custody::send_owner_start_until(
        run.control, request, std::chrono::steady_clock::now() + 2s, &error);
    const int restore_result = ::fcntl(run.control.native(), F_SETFL, original_flags);
    if (!check(sent && restore_result == 0, "bounded large-start request is accepted")) {
        return false;
    }
    bool started = false;
    bool exited = false;
    int exit_code = -1;
    int master = -1;
    bool ok = check(receipt(run, &started, &exited, &exit_code, &master),
        "large-start receives exact native settlement");
    ok &= check(started && exited && exit_code == 23 && master >= 0,
        "the real PTY workload receives the complete large environment");
    if (master >= 0) {
        (void)::close(master);
    }
    run.control.close();
    ok &= check(run.wait(), "large-start helper is reaped by its designated client");
    return ok;
}

bool unavailable_cpu_accounting(const std::string& helper)
{
    custody::Process_cpu_scope unavailable;
    if (!check(!unavailable.cpu_time_usec(), "an unestablished CPU scope is unavailable")) {
        return false;
    }
    Run run;
    if (!check(run.launch(helper), "unavailable-accounting helper starts")) {
        return false;
    }
    custody::Owner_start_request request;
    request.mode = custody::Owner_mode::PTY;
    request.process.executable = "/bin/sh";
    request.process.argv = {"/bin/sh", "-c", "exit 23"};
    // A real transferred descriptor with no cgroup admission authority makes
    // the helper exercise the optional failure path on any Linux host.
    const int unavailable_directory = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (!check(unavailable_directory >= 0, "unavailable accounting fixture descriptor opens")) {
        return false;
    }
    request.process.cpu_accounting_cgroup = unavailable_directory;
    std::string error;
    const bool sent = custody::send_owner_start(run.control, request, &error);
    close(unavailable_directory);
    if (!check(sent, "optional accounting descriptor reaches the helper")) {
        return false;
    }
    bool started = false;
    bool exited = false;
    bool admitted = true;
    int exit_code = -1;
    bool ok = check(receipt(run, &started, &exited, &exit_code, nullptr, &admitted),
        "unavailable accounting retains native settlement");
    ok &= check(started && exited && exit_code == 23 && !admitted,
        "unavailable accounting preserves ordinary PTY execution without claiming a counter");
    ok &= check(run.wait(), "unavailable-accounting helper is collected");
    return ok;
}

struct Cpu_fixture_pipes
{
    int report[2] = {-1, -1};
    int root_release[2] = {-1, -1};
    int worker_release[2] = {-1, -1};

    ~Cpu_fixture_pipes()
    {
        for (const auto pipe : {report, root_release, worker_release}) {
            for (int index = 0; index < 2; ++index) {
                if (pipe[index] >= 0) {
                    close(pipe[index]);
                }
            }
        }
    }

    bool open()
    {
        return pipe2(report, O_CLOEXEC) == 0 &&
            pipe2(root_release, O_CLOEXEC) == 0 &&
            pipe2(worker_release, O_CLOEXEC) == 0;
    }

    void close_transferred()
    {
        close(report[1]);
        report[1] = -1;
        close(root_release[0]);
        root_release[0] = -1;
        close(worker_release[0]);
        worker_release[0] = -1;
    }
};

bool cpu_scope_membership(int directory, pid_t root, pid_t worker, pid_t helper)
{
    const int descriptor = openat(directory, "cgroup.procs", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return false;
    }
    char buffer[4096];
    const auto count = read(descriptor, buffer, sizeof(buffer));
    close(descriptor);
    if (count <= 0 || count == static_cast<ssize_t>(sizeof(buffer))) {
        return false;
    }
    std::istringstream processes(std::string(buffer, static_cast<std::size_t>(count)));
    bool root_present = false;
    bool worker_present = false;
    pid_t process = 0;
    while (processes >> process) {
        if (process == helper) {
            return false;
        }
        root_present |= process == root;
        worker_present |= process == worker;
    }
    return root_present && worker_present;
}

enum class Cpu_cleanup_case { NORMAL, OWNER_LOSS, CLIENT_LOSS };

bool cpu_loss_retirement(Cpu_cleanup_case cleanup_case, Run& run,
    custody::Process_cpu_scope& scope, const custody::Spawn_request& request, int worker_reference)
{
    const int parent = openat(request.cpu_accounting_cgroup, "..",
        O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (!check(parent >= 0, "CPU retirement fixture retains the directory's parent")) {
        return false;
    }
    bool ok = true;
    if (cleanup_case == Cpu_cleanup_case::OWNER_LOSS) {
        ok &= check(custody::posix_pidfd_send_signal(run.helper.process_handle, SIGKILL) == 0 && run.wait(),
            "the exact CPU-accounting helper is killed and collected");
        pollfd live{worker_reference, POLLIN, 0};
        ok &= check(poll(&live, 1, 0) == 0, "detached CPU descendant survives helper loss");
        ok &= check(!scope.release() && scope.cpu_time_usec().has_value(),
            "failed populated-scope removal retains its identity and accounting authority");
        std::string error;
        ok &= check(scope.terminate_and_release(5s, &error),
            "existing family cleanup retires the admitted group after helper loss");
    }
    else {
        // This is the sole control endpoint. Its EOF is the same cleanup
        // trigger as loss of the client process. Keep the parent scope wholly
        // untouched until helper exit and the independent directory check.
        run.control.close();
        ok &= check(run.wait(), "client control EOF lets the helper complete native cleanup");
    }
    pollfd ended{worker_reference, POLLIN, 0};
    ok &= check(poll(&ended, 1, 5000) == 1, "CPU retirement physically ends the detached descendant");
    struct stat directory{};
    ok &= check(fstatat(parent, request.cpu_accounting_name.c_str(), &directory, AT_SYMLINK_NOFOLLOW) < 0 &&
            errno == ENOENT, "the surviving cleanup owner removes the CPU scope directory");
    close(parent);
    ok &= check(scope.release(), "the other owner's scope retirement is idempotent");
    return ok;
}

int cpu_accounting(const std::string& helper, const std::string& self, Cpu_cleanup_case cleanup_case)
{
    custody::Process_cpu_scope scope;
    std::string error;
    if (!scope.establish("test-" + std::to_string(getpid()), &error)) {
        std::printf("SKIP: native CPU accounting requires a delegated cgroup v2: %s\n", error.c_str());
        return 77;
    }
    const auto baseline = scope.cpu_time_usec();
    if (!check(baseline.has_value(), "fresh native CPU scope exposes its cumulative counter")) {
        return 1;
    }
    Run run;
    Cpu_fixture_pipes pipes;
    if (!check(pipes.open() && run.launch(helper), "CPU fixture pipes and owner start")) {
        return 1;
    }
    custody::Owner_start_request request;
    request.mode = custody::Owner_mode::PTY;
    request.process.executable = self;
    request.process.argv = {self, "--cpu-fixture"};
    request.process.inherited = {
        {pipes.report[1], 4}, {pipes.root_release[0], 5}, {pipes.worker_release[0], 6}};
    scope.configure_spawn(request.process);
    if (!check(custody::send_owner_start(run.control, request, &error), "CPU fixture request transfers scope")) {
        return 1;
    }
    pipes.close_transferred();
    custody::Owner_event started;
    if (!check(custody::receive_owner_event(run.control, &started, 5000, &error) ==
            custody::Receive_status::MESSAGE && started.kind == custody::Owner_event_kind::STARTED,
        "CPU fixture gets its native admission receipt"))
    {
        custody::close_native_handle(started.root_reference);
        custody::close_native_handle(started.pty_master);
        return 1;
    }
    custody::close_native_handle(started.root_reference);
    custody::close_native_handle(started.pty_master);
    pid_t worker = 0;
    bool intermediate_collected = false;
    pollfd report{pipes.report[0], POLLIN, 0};
    for (int record = 0; record < 2; ++record) {
        pid_t value = -1;
        if (!check(poll(&report, 1, 5000) == 1 &&
                read(pipes.report[0], &value, sizeof(value)) == sizeof(value),
            "CPU fixture reports its worker and the intermediate parent's collection"))
        {
            return 1;
        }
        if (value == 0) {
            intermediate_collected = true;
        }
        else {
            worker = value;
        }
    }
    if (!check(worker > 0 && intermediate_collected, "CPU worker is detached and reparented before release")) {
        return 1;
    }
    const int worker_reference = custody::posix_pidfd_open(worker);
    if (!check(worker_reference >= 0, "detached CPU worker is retained before release")) {
        return 1;
    }
    bool ok = true;
    if (started.cpu_accounting_admitted) {
        ok &= check(cpu_scope_membership(request.process.cpu_accounting_cgroup,
                static_cast<pid_t>(started.root_exit.process_id), worker,
                static_cast<pid_t>(run.helper.process_id)),
            "CPU scope contains root and detached descendant while excluding the custody helper");
    }
    if (started.cpu_accounting_admitted && cleanup_case != Cpu_cleanup_case::NORMAL) {
        ok &= check(mkdirat(request.process.cpu_accounting_cgroup, "empty-child", 0700) == 0,
            "retirement also owns an empty child cgroup left by its workload");
        ok &= cpu_loss_retirement(cleanup_case, run, scope, request.process, worker_reference);
        close(worker_reference);
        return ok ? 0 : 1;
    }
    ok &= check(write(pipes.worker_release[1], "R", 1) == 1, "detached CPU worker is released");
    pollfd ended{worker_reference, POLLIN, 0};
    ok &= check(poll(&ended, 1, 5000) == 1, "detached CPU worker exits before accounting is sampled");
    close(worker_reference);
    const auto after = scope.cpu_time_usec();
    if (started.cpu_accounting_admitted) {
        ok &= check(after && *after >= *baseline && *after - *baseline >= 20000,
            "cgroup cumulative CPU retains the exited detached worker's CPU time");
    }
    ok &= check(write(pipes.root_release[1], "R", 1) == 1, "CPU fixture root is released");
    bool was_started = true;
    bool exited = false;
    int exit_code = -1;
    ok &= check(receipt(run, &was_started, &exited, &exit_code), "CPU fixture native tree settles");
    ok &= check(exited && exit_code == 23 && run.wait(), "CPU fixture and helper finish normally");
    if (!ok) {
        return 1;
    }
    if (!started.cpu_accounting_admitted) {
        std::printf("SKIP: cgroup exists but workload migration is unavailable; PTY launch still succeeded\n");
        return 77;
    }
    return 0;
}

int cpu_fixture()
{
    const pid_t intermediate = fork();
    if (intermediate < 0) {
        return 3;
    }
    if (intermediate == 0) {
        close(5);
        if (setsid() < 0) {
            _exit(4);
        }
        const pid_t worker = fork();
        if (worker < 0) {
            _exit(5);
        }
        if (worker > 0) {
            _exit(0);
        }
        const pid_t self = getpid();
        char release = 0;
        if (write(4, &self, sizeof(self)) != sizeof(self) || read(6, &release, 1) != 1) {
            _exit(6);
        }
        close(4);
        close(6);
        timespec start{}, now{};
        if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start) != 0) {
            _exit(7);
        }
        do {
            if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now) != 0) {
                _exit(7);
            }
        }
        while ((now.tv_sec - start.tv_sec) * 1000000000LL + now.tv_nsec - start.tv_nsec < 20000000LL);
        _exit(0);
    }
    close(6);
    int status = 0;
    pid_t collected;
    do {
        collected = waitpid(intermediate, &status, 0);
    }
    while (collected < 0 && errno == EINTR);
    const pid_t intermediate_collected = 0;
    if (collected != intermediate || write(4, &intermediate_collected, sizeof(intermediate_collected)) !=
            sizeof(intermediate_collected))
    {
        return 9;
    }
    close(4);
    char release = 0;
    return read(5, &release, 1) == 1 ? 23 : 8;
}

int fixture(const std::string& mode)
{
    const pid_t child = fork();
    if (child < 0) return 3;
    if (child == 0) {
        if (setsid() < 0) _exit(4);
        // Keep the owner's existing graceful-retirement interval observable
        // after root exit. The owner still ends this real child with SIGKILL.
        if (mode == "late-stop" && signal(SIGTERM, SIG_IGN) == SIG_ERR) _exit(7);
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
    if (mode == "normal" || mode == "pty" || mode == "owner-loss" || mode == "late-stop") {
        return 23;
    }
    while (true) pause();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--cpu-fixture") {
        return cpu_fixture();
    }
    if (argc == 3 && (std::string(argv[1]) == "--cpu-accounting" ||
            std::string(argv[1]) == "--cpu-unavailable" ||
            std::string(argv[1]) == "--cpu-owner-loss" ||
            std::string(argv[1]) == "--cpu-client-loss"))
    {
        std::string error;
        if (!custody::prepare_parent_process(&error)) {
            return 2;
        }
        const std::string mode = argv[1];
        if (mode == "--cpu-unavailable") {
            return unavailable_cpu_accounting(argv[2]) ? 0 : 1;
        }
        const auto cleanup_case = mode == "--cpu-owner-loss" ? Cpu_cleanup_case::OWNER_LOSS :
            mode == "--cpu-client-loss" ? Cpu_cleanup_case::CLIENT_LOSS : Cpu_cleanup_case::NORMAL;
        return cpu_accounting(argv[2], argv[0], cleanup_case);
    }
    if (argc == 2 && std::string(argv[1]) == "--started-descriptor") {
        return started_descriptor_contract() ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--receive-error") {
        bool ok = native_receive_error_contract(false);
        ok &= native_receive_error_contract(true);
        return ok ? 0 : 1;
    }
    if (argc == 3 && std::string(argv[1]) == "--late-stop") {
        std::string error;
        if (!custody::prepare_parent_process(&error)) return 2;
        bool ok = terminal_channel_retirement();
        ok &= exercise(argv[2], argv[0], "late-stop");
        return ok ? 0 : 1;
    }
    if (argc == 3 && std::string(argv[1]) == "--fixture") return fixture(argv[2]);
    if (argc != 2) return 2;
    std::string error;
    if (!custody::prepare_parent_process(&error)) return 2;
    bool ok = started_descriptor_contract();
    ok &= abort_before_execution();
    ok &= large_bounded_start(argv[1]);
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
