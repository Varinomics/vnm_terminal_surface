#include <vnm_process_custody/owner.h>

#include "../src/owner_protocol.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

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

bool wait_child(const custody::Spawn_result& child, int* status)
{
    pollfd observer{child.process_handle, POLLIN, 0};
    if (poll(&observer, 1, 3000) != 1) {
        // The test remains the sole waiter, so even failure cleanup retains
        // direct-child identity and cannot signal a recycled numeric PID.
        (void)kill((pid_t)child.process_id, SIGKILL);
    }
    pid_t waited;
    do {
        waited = waitpid((pid_t)child.process_id, status, 0);
    }
    while (waited < 0 && errno == EINTR);
    return waited == child.process_id;
}

custody::Spawn_request command(const std::string& executable, const std::string& mode)
{
    custody::Spawn_request request;
    request.executable = executable;
    request.argv = {executable, "--fixture", mode};
    request.environment = {"PATH=/usr/bin:/bin", "CUSTODY_FIXTURE=visible"};
    return request;
}

bool spawn_contract(const std::string& self)
{
    std::string error;
    custody::Readable_pipe read_end;
    custody::Writable_pipe write_end;
    if (!custody::create_pipe(&read_end, &write_end, &error)) {
        return check(false, error.c_str());
    }
    auto request = command(self, "descriptor");
    request.inherited = {{write_end.native(), 9}};
    auto child = custody::spawn(request);
    if (!check(child.ok, child.error.c_str())) {
        return false;
    }
    write_end.close();
    int status = 0;
    bool ok = check(wait_child(child, &status) && WIFEXITED(status) && WEXITSTATUS(status) == 23,
        "spawn preserves environment, descriptor map and command status");
    custody::close_native_handle(child.process_handle);
    std::uint8_t byte = 0;
    ok &= check(read_end.read(&byte, 1).status == custody::Io_status::TRANSFERRED && byte == 'D',
        "only the requested child descriptor reports data");
    request.executable = "/vnm-custody-no-such-executable";
    request.inherited.clear();
    child = custody::spawn(request);
    ok &= check(!child.ok && child.process_id == 0 && child.process_handle == custody::k_invalid_handle &&
            child.error.find("execve") != std::string::npos,
        "exec failure returns no residual wait obligation");
    request = command(self, "pause");
    child = custody::spawn(request);
    if (!check(child.ok, child.error.c_str())) {
        return false;
    }
    const auto observer = custody::open_process_observer(child.process_id, child.process_birth_token, &error);
    ok &= check(observer >= 0, "receiver creates its own exact process observer");
    const auto mismatched = custody::open_process_observer(child.process_id, child.process_birth_token + 1, &error);
    ok &= check(mismatched == custody::k_invalid_handle, "wrong birth identity cannot acquire an observer");
    custody::close_native_handle(mismatched);
    pollfd observed{observer, POLLIN, 0};
    ok &= check(poll(&observed, 1, 0) == 0, "local process observer starts live");
    (void)kill((pid_t)child.process_id, SIGKILL);
    ok &= check(poll(&observed, 1, 3000) == 1 && (observed.revents & POLLIN) != 0,
        "local observer sees original occurrence exit");
    ok &= check(poll(&observed, 1, 0) == 1, "death stays latched without consuming the kevent");
    ok &= check(wait_child(child, &status), "observer never steals the sole parent's wait");
    custody::close_native_handle(observer);
    custody::close_native_handle(child.process_handle);
    request.admit_execution = [] { return false; };
    child = custody::spawn(request);
    ok &= check(!child.ok && child.process_id == 0 && child.process_handle == custody::k_invalid_handle,
        "pre-execution cancellation reaps the withheld child");
    return ok;
}

bool framed_transport()
{
    std::string error;
    custody::Duplex_channel sender, receiver;
    if (!custody::Duplex_channel::create_pair(64, &sender, &receiver, &error)) {
        return check(false, error.c_str());
    }
    custody::Readable_pipe read_end;
    custody::Writable_pipe write_end;
    if (!custody::create_pipe(&read_end, &write_end, &error)) {
        return false;
    }
    const std::array<std::uint8_t, 3> payload{1, 2, 3};
    bool ok = check(sender.send_descriptors(payload.data(), payload.size(), {write_end.native()}) ==
            custody::Io_status::TRANSFERRED,
        "framed send transfers the descriptor with its record");
    ok &= check(sender.send(payload.data(), payload.size()) == custody::Io_status::TRANSFERRED,
        "adjacent stream record is sent");
    std::array<std::uint8_t, 64> buffer{};
    auto received = receiver.receive(buffer.data(), buffer.size(), 1000);
    ok &= check(received.status == custody::Receive_status::MESSAGE && received.size == payload.size() &&
            std::memcmp(buffer.data(), payload.data(), payload.size()) == 0 && received.descriptors.size() == 1,
        "first record owns exactly its descriptor");
    if (received.descriptors.size() == 1) {
        const int descriptor = received.descriptors.front();
        ok &= check((fcntl(descriptor, F_GETFD) & FD_CLOEXEC) != 0 && write(descriptor, "F", 1) == 1,
            "received descriptor works and is close-on-exec");
    }
    for (const int descriptor : received.descriptors) {
        close(descriptor);
    }
    received = receiver.receive(buffer.data(), buffer.size(), 1000);
    ok &= check(received.status == custody::Receive_status::MESSAGE && received.descriptors.empty(),
        "adjacent record does not acquire preceding SCM_RIGHTS");

    const std::array<std::uint8_t, 6> wire{0, 0, 0, 2, 7, 8};
    ok &= check(write(sender.native(), wire.data(), 2) == 2, "partial prefix fixture write");
    received = receiver.receive(buffer.data(), buffer.size(), 10);
    ok &= check(received.status == custody::Receive_status::TIMEOUT, "partial prefix preserves timeout");
    ok &= check(write(sender.native(), wire.data() + 2, 3) == 3, "partial payload fixture write");
    received = receiver.receive(buffer.data(), buffer.size(), 10);
    ok &= check(received.status == custody::Receive_status::TIMEOUT, "partial payload preserves timeout");
    ok &= check(write(sender.native(), wire.data() + 5, 1) == 1, "payload completion fixture write");
    received = receiver.receive(buffer.data(), buffer.size(), 1000);
    ok &= check(received.status == custody::Receive_status::MESSAGE && received.size == 2 &&
            buffer[0] == 7 && buffer[1] == 8,
        "timeout resumes the same bounded frame");
    ok &= check(write(sender.native(), wire.data(), 5) == 5, "truncated record fixture write");
    sender.close();
    received = receiver.receive(buffer.data(), buffer.size(), 1000);
    ok &= check(received.status == custody::Receive_status::FAILED,
        "peer loss inside a record cannot produce a message or clean EOF");
    return ok;
}

bool retained_group(const std::string& self)
{
    using namespace std::chrono_literals;
    std::string error;
    custody::Process_tree_owner tree;
    if (!tree.establish(&error)) {
        return check(false, error.c_str());
    }
    custody::Readable_pipe read_end;
    custody::Writable_pipe write_end;
    if (!custody::create_pipe(&read_end, &write_end, &error)) {
        return false;
    }
    auto request = command(self, "group");
    request.inherited = {{write_end.native(), 9}};
    tree.configure_root_spawn(request);
    const auto root = custody::spawn(request);
    if (!check(root.ok, root.error.c_str())) {
        return false;
    }
    tree.record_root(root);
    write_end.close();
    pollfd reported{read_end.native(), POLLIN, 0};
    std::uint8_t byte = 0;
    bool ok = check(poll(&reported, 1, 3000) == 1 &&
            read_end.read(&byte, 1).status == custody::Io_status::TRANSFERRED && byte == 'G',
        "root spawned an ordinary group child");
    const auto deadline = Clock::now() + 3s;
    std::vector<custody::Exit_status> exits;
    while (exits.empty() && Clock::now() < deadline) {
        exits = tree.reap_available();
        std::this_thread::sleep_for(5ms);
    }
    siginfo_t info{};
    ok &= check(exits.size() == 1 && exits.front().exit_code == 23 &&
            waitid(P_PID, (id_t)root.process_id, &info, WEXITED | WNOHANG | WNOWAIT) == 0 &&
            info.si_pid == root.process_id,
        "root exit is reported while its original zombie remains owned");
    ok &= check(tree.children_remain(), "root exit is not group settlement");
    const auto outcome = tree.terminate_and_reap(10ms, 3000ms);
    ok &= check(outcome.complete && outcome.cleanup_scope == custody::Cleanup_scope::OWNED_PROCESS_GROUPS,
        outcome.error.empty() ? "original group cleanup settles within explicit scope" : outcome.error.c_str());
    ok &= check(waitpid((pid_t)root.process_id, nullptr, WNOHANG) == -1 && errno == ECHILD,
        "the owner reaps its root exactly once after group settlement");
    ok &= check(tree.terminate_and_reap(0ms, 10ms).complete,
        "repeated settlement does not signal a stale process group");
    return ok;
}

bool unrelated_group_survives(const std::string& self)
{
    auto request = command(self, "pause");
    request.new_session = true;
    const auto unrelated = custody::spawn(request);
    if (!check(unrelated.ok, unrelated.error.c_str())) {
        return false;
    }
    bool ok = retained_group(self);
    pollfd observer{unrelated.process_handle, POLLIN, 0};
    ok &= check(poll(&observer, 1, 0) == 0, "cleanup preserves an unrelated profile's original group");
    (void)kill((pid_t)unrelated.process_id, SIGKILL);
    int status;
    ok &= check(wait_child(unrelated, &status), "test retains sole wait for its unrelated fixture");
    custody::close_native_handle(unrelated.process_handle);
    return ok;
}

struct Run
{
    custody::Duplex_channel channel;
    custody::Spawn_result helper;
    ~Run()
    {
        channel.close();
        if (helper.process_id > 0) {
            int status;
            wait_child(helper, &status);
        }
        custody::close_native_handle(helper.process_handle);
    }
    bool launch(const std::string& executable)
    {
        std::string error;
        custody::Duplex_channel child;
        if (!custody::Duplex_channel::create_pair(custody::k_owner_message_bytes, &channel, &child, &error)) {
            return check(false, error.c_str());
        }
        custody::Spawn_request request;
        request.executable = executable;
        request.argv = {executable, "--control", custody::inherited_argument(0)};
        request.inherited = {{child.native(), 3}};
        request.environment = {"PATH=/usr/bin:/bin"};
        helper = custody::spawn(request);
        return check(helper.ok, helper.error.c_str());
    }
};

bool command_owner(const std::string& helper, bool fail_start,
    custody::Owner_mode mode = custody::Owner_mode::COMMAND)
{
    Run run;
    if (!run.launch(helper)) {
        return false;
    }
    custody::Owner_start_request request;
    request.mode = mode;
    request.process.executable = fail_start ? "/vnm-no-such-command" : "/bin/sh";
    request.process.argv = {"sh", "-c", "exit 23"};
    std::string error;
    if (!custody::send_owner_start(run.channel, request, &error)) {
        return check(false, error.c_str());
    }
    bool started = false;
    bool rejected = false;
    bool exited = false;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline) {
        custody::Owner_event event;
        const auto status = custody::receive_owner_event(run.channel, &event, 100, &error);
        if (status == custody::Receive_status::TIMEOUT) {
            continue;
        }
        if (!check(status == custody::Receive_status::MESSAGE, error.c_str())) {
            return false;
        }
        custody::close_native_handle(event.root_reference);
        custody::close_native_handle(event.pty_master);
        if (!check(event.cleanup_scope == custody::Cleanup_scope::OWNED_PROCESS_GROUPS,
                "every Darwin owner receipt identifies its cleanup scope"))
        {
            return false;
        }
        started |= event.kind == custody::Owner_event_kind::STARTED;
        rejected |= event.kind == custody::Owner_event_kind::START_REJECTED;
        exited |= event.kind == custody::Owner_event_kind::ROOT_EXITED && event.root_exit.exit_code == 23;
        if (event.kind == custody::Owner_event_kind::TREE_SETTLEMENT &&
            event.settlement == custody::Tree_settlement::CONFIRMED)
        {
            const bool expect_rejection = fail_start || mode == custody::Owner_mode::PTY;
            return check(expect_rejection ? rejected && !started : started && exited,
                "command birth/rejection, root status and scoped settlement are distinct");
        }
    }
    return check(false, "owner settlement deadline");
}

bool stop_or_lose_owner(const std::string& helper, const std::string& self, const std::string& mode)
{
    using namespace std::chrono_literals;
    Run run;
    if (!run.launch(helper)) {
        return false;
    }
    std::string error;
    custody::Readable_pipe release_read;
    custody::Writable_pipe release_write;
    if (!custody::create_pipe(&release_read, &release_write, &error)) {
        return check(false, error.c_str());
    }
    custody::Owner_start_request request;
    request.process = command(self, "release");
    request.process.inherited = {{release_read.native(), 9}};
    if (!custody::send_owner_start(run.channel, request, &error)) {
        return check(false, error.c_str());
    }
    release_read.close();
    custody::Owner_event event;
    const auto receipt = custody::receive_owner_event(run.channel, &event, 3000, &error);
    if (!check(receipt == custody::Receive_status::MESSAGE && event.kind == custody::Owner_event_kind::STARTED &&
            event.root_reference >= 0 && event.root_birth_token != 0,
            "living owner workload supplies a receiver-local exact observer"))
    {
        custody::close_native_handle(event.root_reference);
        return false;
    }
    const int observer = event.root_reference;
    const std::int64_t root_pid = event.root_exit.process_id;
    bool ok = true;
    if (mode == "loss") {
        // The helper is our unreaped direct child. Its workload remains gated
        // by the private release pipe, so this cannot accidentally test a PID
        // that disappeared before final-owner loss.
        (void)kill((pid_t)run.helper.process_id, SIGKILL);
        int status;
        ok &= check(wait_child(run.helper, &status), "final helper loss is observed by its sole parent");
        run.helper.process_id = 0;
        custody::Owner_event lost;
        ok &= check(custody::receive_owner_event(run.channel, &lost, 1000, &error) != custody::Receive_status::MESSAGE,
            "final owner loss produces no settlement receipt");
        custody::Kill_coordinates coordinates;
        coordinates.cleanup_scope = custody::Cleanup_scope::OWNED_PROCESS_GROUPS;
        coordinates.process_group = root_pid;
        coordinates.process_ids = {root_pid};
        coordinates.process_handles = {observer};
        ok &= check(!custody::terminate_coordinates(coordinates, 0ms, 0ms),
            "even a living observer grants no fallback numeric signal authority");
        pollfd alive{observer, POLLIN, 0};
        ok &= check(poll(&alive, 1, 0) == 0, "owner-loss cleanup remains truthfully unconfirmed while root survives");
        ok &= check(write(release_write.native(), "R", 1) == 1,
            "private fixture release ends the otherwise unowned workload");
    }
    else {
        if (mode == "close") {
            run.channel.close();
        }
        else {
            ok &= check(custody::send_owner_stop(run.channel, 10, &error), "owner stop command is admitted");
            bool settled = false;
            const auto deadline = Clock::now() + 3s;
            while (Clock::now() < deadline && !settled) {
                custody::Owner_event next;
                const auto received = custody::receive_owner_event(run.channel, &next, 100, &error);
                if (received == custody::Receive_status::TIMEOUT) {
                    continue;
                }
                if (received != custody::Receive_status::MESSAGE) {
                    break;
                }
                settled = next.kind == custody::Owner_event_kind::TREE_SETTLEMENT &&
                    next.settlement == custody::Tree_settlement::CONFIRMED &&
                    next.cleanup_scope == custody::Cleanup_scope::OWNED_PROCESS_GROUPS;
                custody::close_native_handle(next.root_reference);
                custody::close_native_handle(next.pty_master);
            }
            ok &= check(settled, "cancel returns explicit group settlement");
        }
    }
    pollfd ended{observer, POLLIN, 0};
    ok &= check(poll(&ended, 1, 3000) == 1 && (ended.revents & POLLIN) != 0,
        "original root occurrence physically ends after cancel, close or fixture release");
    release_write.close();
    custody::close_native_handle(observer);
    return ok;
}

bool unknown_capabilities_remain_local()
{
    custody::Duplex_channel sender, receiver;
    std::string error;
    if (!custody::Duplex_channel::create_pair(custody::k_owner_message_bytes, &sender, &receiver, &error)) {
        return false;
    }
    std::vector<std::uint8_t> message;
    const auto append_number = [](std::vector<std::uint8_t>& bytes, std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            bytes.push_back((std::uint8_t)(value >> shift));
        }
    };
    // Existing EVENT schema followed by unknown versions of two independent
    // optional records. Their absence must not invalidate a command birth.
    for (const std::uint32_t value : {1u, 4u, 0u, (std::uint32_t)getpid(), 0u, 0u, 0u, 0u,
             1u, 99u, 4u, 2u, 2u, 99u, 8u, 0u, 1u})
    {
        append_number(message, value);
    }
    std::vector<std::uint8_t> fragment;
    for (const std::uint32_t value : {1u, 5u, (std::uint32_t)message.size(), 0u}) {
        append_number(fragment, value);
    }
    fragment.insert(fragment.end(), message.begin(), message.end());
    if (sender.send(fragment.data(), fragment.size()) != custody::Io_status::TRANSFERRED) {
        return false;
    }
    custody::Owner_event event;
    const auto status = custody::receive_owner_event(receiver, &event, 1000, &error);
    return check(status == custody::Receive_status::MESSAGE && event.kind == custody::Owner_event_kind::STARTED &&
            event.cleanup_scope == custody::Cleanup_scope::UNSPECIFIED && event.root_birth_token == 0 &&
            event.root_reference == custody::k_invalid_handle,
        "unknown optional scope/observation records remain unavailable capabilities");
}

int fixture(const std::string& mode)
{
    if (mode == "release") {
        char release;
        return read(9, &release, 1) == 1 && release == 'R' ? 0 : 36;
    }
    if (mode == "descriptor") {
        const char* environment = std::getenv("CUSTODY_FIXTURE");
        if (!environment || std::string(environment) != "visible") {
            return 31;
        }
        for (int fd = 3; fd < 128; ++fd) {
            if (fd != 9 && fcntl(fd, F_GETFD) >= 0) {
                return 32;
            }
        }
        return write(9, "D", 1) == 1 ? 23 : 33;
    }
    if (mode == "group") {
        const pid_t child = fork();
        if (child < 0) {
            return 34;
        }
        if (child > 0) {
            return 23;
        }
        (void)signal(SIGTERM, SIG_IGN);
        if (write(9, "G", 1) != 1) {
            return 35;
        }
    }
    while (true) {
        pause();
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--fixture") {
        return fixture(argv[2]);
    }
    if (argc != 2) {
        return 2;
    }
    std::string error;
    if (!custody::prepare_parent_process(&error)) {
        return 2;
    }
    bool ok = framed_transport();
    ok &= spawn_contract(argv[0]);
    ok &= unrelated_group_survives(argv[0]);
    ok &= command_owner(argv[1], false);
    ok &= command_owner(argv[1], true);
    ok &= command_owner(argv[1], false, custody::Owner_mode::PTY);
    ok &= stop_or_lose_owner(argv[1], argv[0], "stop");
    ok &= stop_or_lose_owner(argv[1], argv[0], "close");
    ok &= stop_or_lose_owner(argv[1], argv[0], "loss");
    ok &= unknown_capabilities_remain_local();
    custody::Process_tree_containment containment;
    ok &= check(!containment.establish("fixture", &error) && !containment.available() && !error.empty(),
        "Darwin reports unavailable independent containment");
    ok &= check(!custody::terminate_coordinates({}, std::chrono::milliseconds(0), std::chrono::milliseconds(0)),
        "final owner loss cannot turn numeric coordinates into successful cleanup");
    std::puts(ok ? "PASS: Darwin command custody and framed transport" : "FAIL: Darwin custody");
    return ok ? 0 : 1;
}
