#include <vnm_process_custody/owner.h>

#include "../src/owner_protocol.h"

#include <fcntl.h>
#include <libproc.h>
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
    ok &= check(!child.ok && child.process_id == 0 && child.process_handle == custody::k_invalid_handle &&
            child.no_workload_admitted && !child.failed_birth,
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

bool finite_send_requires_nonblocking()
{
    using namespace std::chrono_literals;
    custody::Duplex_channel sender, receiver;
    std::string error;
    if (!custody::Duplex_channel::create_pair(64, &sender, &receiver, &error)) {
        return check(false, error.c_str());
    }
    const int original_flags = fcntl(sender.native(), F_GETFL);
    if (!check(original_flags >= 0 && (original_flags & O_NONBLOCK) == 0,
        "finite-send refusal fixture starts with a blocking channel"))
    {
        return false;
    }
    const std::array<std::uint8_t, 3> payload{1, 2, 3};
    int send_error = 0;
    const auto started = Clock::now();
    const auto status = sender.send_descriptors_until(payload.data(), payload.size(), {}, started + 80ms, &send_error);
    pollfd peer{receiver.native(), POLLIN, 0};
    bool ok = check(status == custody::Io_status::FAILED && send_error == EINVAL &&
            sender.valid() && fcntl(sender.native(), F_GETFL) == original_flags && poll(&peer, 1, 0) == 0 &&
            Clock::now() - started < 1s,
        "finite send refuses a blocking descriptor without bytes, mode changes or channel closure");
    ok &= check(sender.send(payload.data(), payload.size()) == custody::Io_status::TRANSFERRED,
        "unbounded send remains available on the unchanged blocking channel");
    std::array<std::uint8_t, 64> output{};
    const auto received = receiver.receive(output.data(), output.size(), 1000);
    ok &= check(received.status == custody::Receive_status::MESSAGE && received.size == payload.size() &&
            std::memcmp(output.data(), payload.data(), payload.size()) == 0 && received.descriptors.empty(),
        "unbounded retry delivers exactly its original payload");
    for (const int descriptor : received.descriptors) {
        close(descriptor);
    }
    return ok;
}

bool partial_frame_deadline_closes_stream()
{
    using namespace std::chrono_literals;
    std::fprintf(stderr, "framing: bounded partial send\n");
    constexpr std::size_t payload_bytes = 1024 * 1024;
    custody::Duplex_channel sender, receiver;
    std::string error;
    if (!custody::Duplex_channel::create_pair(payload_bytes, &sender, &receiver, &error)) {
        return check(false, error.c_str());
    }
    // Match send_owner_start_until's nonblocking-channel precondition.
    const int send_flags = fcntl(sender.native(), F_GETFL);
    const int receive_flags = fcntl(receiver.native(), F_GETFL);
    if (!check(send_flags >= 0 && fcntl(sender.native(), F_SETFL, send_flags | O_NONBLOCK) == 0 &&
            receive_flags >= 0 && fcntl(receiver.native(), F_SETFL, receive_flags | O_NONBLOCK) == 0,
        "bounded writer and raw wire-progress observation are nonblocking"))
    {
        return false;
    }
    int send_capacity = 4096;
    socklen_t capacity_size = sizeof(send_capacity);
    if (!check(setsockopt(sender.native(), SOL_SOCKET, SO_SNDBUF, &send_capacity, sizeof(send_capacity)) == 0 &&
            getsockopt(sender.native(), SOL_SOCKET, SO_SNDBUF, &send_capacity, &capacity_size) == 0 &&
            send_capacity > 0 && (std::size_t)send_capacity < payload_bytes,
        "single-record fixture has a send buffer smaller than its payload"))
    {
        return false;
    }
    const std::vector<std::uint8_t> payload(payload_bytes, 0x5a);
    int send_error = 0;
    const auto started = Clock::now();
    const auto status = sender.send_descriptors_until(payload.data(), payload.size(), {}, started + 80ms, &send_error);
    const auto elapsed = Clock::now() - started;

    // Inspect actual wire progress, not an assumed relationship between the
    // socket capacity and its first write. Zero-byte timeouts remain retryable.
    std::array<std::uint8_t, 8192> wire{};
    std::size_t transferred = 0;
    ssize_t count;
    do {
        count = recv(receiver.native(), wire.data(), wire.size(), MSG_DONTWAIT);
        if (count > 0) {
            transferred += (std::size_t)count;
        }
    }
    while (count > 0 && transferred <= payload_bytes + 4);
    std::printf("partial frame: wire_bytes=%zu full_record=%zu elapsed_ms=%lld\n", transferred,
        payload_bytes + 4, (long long)std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    bool ok = check(transferred > 0 && transferred < payload_bytes + 4,
        "deadline interrupted an actually partially transmitted single record");
    ok &= check(status == custody::Io_status::FAILED && send_error == ETIMEDOUT &&
            elapsed >= 80ms && elapsed < 2s,
        "partial record consumes one bounded send deadline");
    ok &= check(!sender.valid() && count == 0,
        "failed partial record closes the sender and delivers EOF after its wire bytes");
    return ok;
}

bool concurrent_channel_retirement()
{
    using namespace std::chrono_literals;
    constexpr std::size_t payload_bytes = 1024 * 1024;
    custody::Duplex_channel channel, peer;
    std::string error;
    if (!custody::Duplex_channel::create_pair(payload_bytes, &channel, &peer, &error)) {
        return check(false, error.c_str());
    }
    const int flags = fcntl(channel.native(), F_GETFL);
    const int send_capacity = 4096;
    if (!check(flags >= 0 && fcntl(channel.native(), F_SETFL, flags | O_NONBLOCK) == 0 &&
            setsockopt(channel.native(), SOL_SOCKET, SO_SNDBUF, &send_capacity, sizeof(send_capacity)) == 0,
        "concurrent retirement starts with a bounded nonblocking sender"))
    {
        return false;
    }

    // Retain an incomplete incoming payload before the opposite direction fails.
    const std::array<std::uint8_t, 5> partial{0, 0, 0, 2, 7};
    std::vector<std::uint8_t> output(payload_bytes);
    if (!check(write(peer.native(), partial.data(), partial.size()) == (ssize_t)partial.size() &&
            channel.receive(output.data(), output.size(), 10).status == custody::Receive_status::TIMEOUT,
        "receiver retains its original partial record"))
    {
        return false;
    }
    custody::Receive_result received;
    std::thread receiver([&] {
        received = channel.receive(output.data(), output.size(), 500);
    });
    const std::vector<std::uint8_t> payload(payload_bytes, 0x5a);
    int send_error = 0;
    const auto status = channel.send_descriptors_until(
        payload.data(), payload.size(), {}, Clock::now() + 80ms, &send_error);
    receiver.join();

    std::array<std::uint8_t, 8192> wire{};
    std::size_t transferred = 0;
    ssize_t count;
    do {
        count = recv(peer.native(), wire.data(), wire.size(), MSG_DONTWAIT);
        if (count > 0) {
            transferred += (std::size_t)count;
        }
    }
    while (count > 0 && transferred <= payload_bytes + 4);
    bool ok = check(transferred > 0 && transferred < payload_bytes + 4 && count == 0,
        "failed concurrent send retires an actually partial wire record");
    ok &= check(status == custody::Io_status::FAILED && send_error == ETIMEDOUT && !channel.valid(),
        "failed partial send closes the channel");
    ok &= check(received.status == custody::Receive_status::CLOSED ||
            received.status == custody::Receive_status::FAILED,
        "send retirement interrupts the concurrent incomplete receive without a timeout or message");
    for (const int descriptor : received.descriptors) {
        close(descriptor);
    }
    return ok;
}

bool directional_channel_eof()
{
    custody::Duplex_channel channel, peer;
    std::string error;
    if (!custody::Duplex_channel::create_pair(64, &channel, &peer, &error)) {
        return check(false, error.c_str());
    }
    const int duplicate = dup(channel.native());
    if (!check(duplicate >= 0, "retained child descriptor fixture")) {
        return false;
    }
    custody::Duplex_channel inherited(duplicate, channel.max_message_bytes());
    channel.close();
    channel = std::move(inherited);
    int native_error = 0;
    std::array<std::uint8_t, 64> output{};
    const std::uint8_t reply = 7;
    bool ok = check(peer.send(&reply, 1) == custody::Io_status::TRANSFERRED &&
            channel.receive(output.data(), output.size(), 1000).status == custody::Receive_status::MESSAGE,
        "idle close preserves the endpoint retained by a child descriptor");
    ok &= check(channel.shutdown_send(&native_error) &&
            recv(peer.native(), output.data(), output.size(), MSG_DONTWAIT) == 0,
        "directional shutdown sends EOF to the peer");
    ok &= check(peer.send(&reply, 1) == custody::Io_status::TRANSFERRED,
        "peer can still send its terminal receipt after receiving EOF");
    const auto received = channel.receive(output.data(), output.size(), 1000);
    ok &= check(received.status == custody::Receive_status::MESSAGE &&
            received.size == 1 && output[0] == reply,
        "directional shutdown preserves the receiving direction");
    peer.close();
    ok &= check(channel.receive(output.data(), output.size(), 1000).status == custody::Receive_status::CLOSED &&
            channel.shutdown_send(&native_error),
        "directional shutdown is complete after channel retirement");
    return ok;
}

enum class Rejected_record { OVERSIZE, LATE_RIGHTS, TRUNCATED_RIGHTS };

bool rejected_record_releases_descriptors(Rejected_record kind)
{
    const char* label = kind == Rejected_record::OVERSIZE ? "oversized frame" :
        kind == Rejected_record::LATE_RIGHTS ? "late rights" : "truncated rights";
    std::fprintf(stderr, "framing: %s\n", label);
    custody::Duplex_channel sender, receiver;
    custody::Readable_pipe read_end;
    custody::Writable_pipe write_end;
    std::string error;
    if (!custody::Duplex_channel::create_pair(64, &sender, &receiver, &error) ||
        !custody::create_pipe(&read_end, &write_end, &error) || !read_end.prepare_for_owner(&error))
    {
        return check(false, error.c_str());
    }
    std::array<std::uint8_t, 64> output{};
    std::array<std::uint8_t, 5> wire{0, 0, 0, 1, 0x5a};
    std::size_t offset = 0;
    if (kind == Rejected_record::OVERSIZE) {
        wire[3] = 65;
    }
    if (kind == Rejected_record::LATE_RIGHTS) {
        if (!check(write(sender.native(), wire.data(), 1) == 1,
            "late-rights fixture sends its first prefix byte without descriptors"))
        {
            return false;
        }
        const auto partial = receiver.receive(output.data(), output.size(), 10);
        if (!check(partial.status == custody::Receive_status::TIMEOUT,
            "receiver retains the prefix before late rights arrive"))
        {
            return false;
        }
        offset = 1;
    }
    // Repeating one pipe writer makes EOF an ownership oracle for every
    // installed descriptor, including a truncated SCM_RIGHTS control message.
    const std::size_t descriptor_count = kind == Rejected_record::TRUNCATED_RIGHTS ? 65 : 1;
    const std::vector<int> descriptors(descriptor_count, write_end.native());
    alignas(cmsghdr) std::uint8_t control[CMSG_SPACE(sizeof(int) * 65)]{};
    iovec part{wire.data() + offset, wire.size() - offset};
    msghdr message{};
    message.msg_iov = &part;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = CMSG_SPACE(sizeof(int) * descriptor_count);
    auto* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int) * descriptor_count);
    std::memcpy(CMSG_DATA(header), descriptors.data(), sizeof(int) * descriptor_count);
    if (!check(sendmsg(sender.native(), &message, 0) == (ssize_t)part.iov_len,
        "malformed-frame fixture transfers its real pipe descriptors"))
    {
        return false;
    }
    write_end.close();
    const auto received = receiver.receive(output.data(), output.size(), 1000);
    const auto expected = kind == Rejected_record::OVERSIZE ?
        custody::Receive_status::OVERSIZE : custody::Receive_status::FAILED;
    std::uint8_t byte = 0;
    pollfd pipe_state{read_end.native(), POLLIN, 0};
    const bool writers_released = poll(&pipe_state, 1, 250) == 1 &&
        read_end.read(&byte, 1).status == custody::Io_status::CLOSED;
    std::printf("rejected %s: status=%d channel_closed=%d descriptors_returned=%zu pipe_eof=%d\n",
        label, (int)received.status, !receiver.valid(), received.descriptors.size(), writers_released);
    const bool ok = check(received.status == expected && !receiver.valid() && received.descriptors.empty() &&
            writers_released,
        "rejected record closes its stream and releases every received descriptor");
    for (const int descriptor : received.descriptors) {
        close(descriptor);
    }
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
    ok &= check(outcome.complete && !outcome.terminal_unconfirmed &&
            outcome.cleanup_scope == custody::Cleanup_scope::OWNED_PROCESS_GROUPS,
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
    bool launch(const std::string& executable, bool pty_contract = false)
    {
        std::string error;
        custody::Duplex_channel child;
        if (!custody::Duplex_channel::create_pair(custody::k_owner_message_bytes, &channel, &child, &error)) {
            return check(false, error.c_str());
        }
        custody::Spawn_request request;
        request.executable = executable;
        request.argv = {executable, "--control", custody::inherited_argument(0)};
        if (pty_contract) {
            request.argv.emplace_back(custody::k_darwin_pty_owner_argument);
        }
        request.inherited = {{child.native(), 3}};
        request.environment = {"PATH=/usr/bin:/bin"};
        helper = custody::spawn(request);
        return check(helper.ok, helper.error.c_str());
    }
};

bool unsupported_pty_mode(const std::string& helper)
{
    Run run;
    if (!run.launch(helper, true)) {
        return false;
    }
    std::string error;
    custody::Native_process_status_observer observer;
    if (!check(observer.open(run.helper.process_id, run.helper.process_birth_token, &error),
        "unsupported-mode fixture retains the original helper status before START"))
    {
        return false;
    }
    custody::Owner_start_request request;
    request.mode = custody::Owner_mode::COMMAND;
    request.process.executable = "/bin/sh";
    request.process.argv = {"sh", "-c", "exit 23"};
    if (!check(custody::send_owner_start(run.channel, request, &error), error.c_str())) {
        return false;
    }
    bool rejected = false;
    bool positive_settlement = false;
    bool root_born = false;
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < deadline) {
        custody::Owner_event event;
        const auto status = custody::receive_owner_event(run.channel, &event, 100, &error);
        if (status == custody::Receive_status::TIMEOUT) {
            continue;
        }
        if (status != custody::Receive_status::MESSAGE) {
            break;
        }
        custody::close_native_handle(event.root_reference);
        custody::close_native_handle(event.pty_master);
        rejected |= event.kind == custody::Owner_event_kind::START_REJECTED;
        root_born |= event.kind == custody::Owner_event_kind::STARTED;
        positive_settlement |= event.kind == custody::Owner_event_kind::TREE_SETTLEMENT &&
            event.settlement == custody::Tree_settlement::CONFIRMED;
    }
    int wait_status = 0;
    const bool waited = wait_child(run.helper, &wait_status);
    if (waited) {
        run.helper.process_id = 0;
    }
    const auto observed = observer.observe();
    const bool positive_outcome = observed.status_valid && WIFEXITED(observed.wait_status) &&
        (WEXITSTATUS(observed.wait_status) == static_cast<int>(custody::Darwin_pty_owner_exit::NO_WORKLOAD) ||
         WEXITSTATUS(observed.wait_status) == static_cast<int>(custody::Darwin_pty_owner_exit::CONFIRMED));
    std::printf("UNSUPPORTED_PTY_MODE rejected=%d root_born=%d confirmed=%d wait_status=%d observed_status=%d\n",
        rejected, root_born, positive_settlement, wait_status, observed.wait_status);
    bool ok = check(waited && observed.status_valid && observed.wait_status == wait_status,
        "unsupported mode preserves the exact original helper status");
    ok &= check(rejected && !root_born, "unsupported mode rejects before root admission");
    ok &= check(!positive_settlement && !positive_outcome,
        "unsupported mode has no positive PTY cleanup meaning on either channel");
    return ok;
}

struct held_group_ready_t
{
    pid_t parent = 0;
    pid_t child = 0;
    pid_t original_group = 0;
    int child_status = 0;
};

bool read_fixture_bytes(int descriptor, void* destination, std::size_t size)
{
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    auto* bytes = static_cast<std::uint8_t*>(destination);
    while (size != 0 && Clock::now() < deadline) {
        pollfd ready{descriptor, POLLIN, 0};
        const int remaining = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
        if (poll(&ready, 1, remaining > 0 ? remaining : 0) != 1) {
            return false;
        }
        const ssize_t count = read(descriptor, bytes, size);
        if (count <= 0) {
            return false;
        }
        bytes += count;
        size -= static_cast<std::size_t>(count);
    }
    return size == 0;
}

class Held_group_fixture
{
public:
    ~Held_group_fixture()
    {
        m_release_write.close();
        custody::close_native_handle(m_parent_observer);
    }

    bool prepare(custody::Spawn_request& request)
    {
        std::string error;
        if (!custody::create_pipe(&m_receipt_read, &m_receipt_write, &error) ||
            !custody::create_pipe(&m_release_read, &m_release_write, &error))
        {
            return check(false, error.c_str());
        }
        request.inherited = {{m_receipt_write.native(), 9}, {m_release_read.native(), 10}};
        return true;
    }

    bool ready(std::int64_t root)
    {
        m_receipt_write.close();
        m_release_read.close();
        held_group_ready_t ready;
        if (!check(read_fixture_bytes(m_receipt_read.native(), &ready, sizeof(ready)) &&
                ready.parent > 1 && ready.child > 1 && ready.original_group == root &&
                ready.child_status == CLD_EXITED,
            "fixture parent retains a dead child in the original root group"))
        {
            return false;
        }
        proc_bsdinfo info{};
        if (!check(proc_pidinfo(ready.parent, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info) &&
                info.pbi_pid == ready.parent && info.pbi_pgid == ready.parent,
            "fixture holder moved to its own session after its child exited"))
        {
            return false;
        }
        std::string error;
        m_parent_observer = custody::open_process_observer(ready.parent,
            info.pbi_start_tvsec * 1'000'000 + info.pbi_start_tvusec, &error);
        std::printf("held group root=%lld parent=%d child=%d observer=%d\n",
            static_cast<long long>(root), ready.parent, ready.child, m_parent_observer);
        return check(m_parent_observer >= 0 && holding(), "fixture holder is live before cleanup");
    }

    bool holding() const
    {
        pollfd parent{m_parent_observer, POLLIN, 0};
        pollfd receipt{m_receipt_read.native(), POLLIN, 0};
        return m_parent_observer >= 0 && poll(&parent, 1, 0) == 0 && poll(&receipt, 1, 0) == 0;
    }

    bool release()
    {
        // This private EOF releases only the fixture's own parent. The test
        // never signals or waits either of these nonchild process numbers.
        m_release_write.close();
        char receipt = 0;
        const bool reaped = read_fixture_bytes(m_receipt_read.native(), &receipt, 1) && receipt == 'R';
        pollfd parent{m_parent_observer, POLLIN, 0};
        return check(reaped && m_parent_observer >= 0 && poll(&parent, 1, 3000) == 1 &&
                (parent.revents & POLLIN) != 0,
            "fixture parent alone collects its dead child and ends after release");
    }

private:
    custody::Readable_pipe m_receipt_read;
    custody::Writable_pipe m_receipt_write;
    custody::Readable_pipe m_release_read;
    custody::Writable_pipe m_release_write;
    int m_parent_observer = -1;
};

enum class Held_cleanup { FORCE_STOP, GRACEFUL_STOP, NORMAL_EXIT };

bool exhausted_group_is_terminal(const std::string& self, Held_cleanup cleanup)
{
    using namespace std::chrono_literals;
    const bool normal_exit = cleanup == Held_cleanup::NORMAL_EXIT;
    Held_group_fixture fixture;
    auto request = command(self, normal_exit ? "held_group_exit" : "held_group");
    if (!fixture.prepare(request)) {
        return false;
    }
    custody::Process_tree_owner tree;
    std::string error;
    if (!tree.establish(&error)) {
        return check(false, error.c_str());
    }
    tree.configure_root_spawn(request);
    const auto root = custody::spawn(request);
    if (!check(root.ok, root.error.c_str())) {
        return false;
    }
    tree.record_root(root);
    bool ok = fixture.ready(root.process_id);
    std::vector<custody::Exit_status> root_exits;
    if (normal_exit) {
        const auto deadline = Clock::now() + 3s;
        while (root_exits.empty() && Clock::now() < deadline) {
            root_exits = tree.reap_available();
            if (root_exits.empty()) {
                std::this_thread::sleep_for(1ms);
            }
        }
        ok &= check(root_exits.size() == 1 && root_exits.front().exit_code == 23,
            "normal root exit is reported before cleanup begins");
    }
    const auto started = Clock::now();
    const auto grace = cleanup == Held_cleanup::FORCE_STOP ? 0ms : 250ms;
    const auto first = tree.terminate_and_reap(grace, 150ms);
    root_exits.insert(root_exits.end(), first.exits.begin(), first.exits.end());
    ok &= check(!first.complete && first.terminal_unconfirmed && first.escalated_to_kill &&
            first.cleanup_scope == custody::Cleanup_scope::OWNED_PROCESS_GROUPS &&
            Clock::now() - started < 2s && fixture.holding(),
        "bounded cleanup ends unconfirmed while another parent still holds the dead member");
    const int expected_signal = normal_exit ? 0 : grace.count() == 0 ? SIGKILL : SIGTERM;
    ok &= check(root_exits.size() == 1 && root_exits.front().process_id == root.process_id &&
            root_exits.front().signal == expected_signal &&
            (!normal_exit || root_exits.front().exit_code == 23),
        "terminal exhaustion preserves the exact original root exit receipt");
    siginfo_t root_status{};
    ok &= check(waitid(P_PID, static_cast<id_t>(root.process_id), &root_status, WEXITED | WNOHANG | WNOWAIT) == -1 &&
            errno == ECHILD &&
            !tree.children_remain() && tree.exit_wait_handles().empty() &&
            tree.coordinates().process_group == 0 && tree.coordinates().process_ids.empty(),
        "terminal exhaustion collects the root once and retires its action authority");
    const auto repeated = tree.terminate_and_reap(0ms, 150ms);
    ok &= check(!repeated.complete && repeated.terminal_unconfirmed && !repeated.escalated_to_kill &&
            repeated.exits.empty() && fixture.holding(),
        "repeated cleanup remains terminal without another signal or root receipt");
    ok &= fixture.release();
    const auto released = tree.terminate_and_reap(0ms, 150ms);
    ok &= check(!released.complete && released.terminal_unconfirmed && !released.escalated_to_kill,
        "later fixture collection cannot upgrade the retired outcome to confirmed");
    std::printf("held group direct cleanup=%d terminal=%d repeat_terminal=%d result=%d detail=%s\n",
        static_cast<int>(cleanup), first.terminal_unconfirmed, repeated.terminal_unconfirmed, ok,
        first.error.c_str());
    return ok;
}

bool exhausted_owner_receipts(const std::string& helper, const std::string& self, bool normal_exit)
{
    using namespace std::chrono_literals;
    Run run;
    Held_group_fixture fixture;
    custody::Owner_start_request request;
    request.process = command(self, normal_exit ? "held_group_exit" : "held_group");
    if (!fixture.prepare(request.process) || !run.launch(helper)) {
        return false;
    }
    std::string error;
    if (!check(custody::send_owner_start(run.channel, request, &error), error.c_str())) {
        return false;
    }
    custody::Owner_event started;
    const auto start_status = custody::receive_owner_event(run.channel, &started, 3000, &error);
    custody::close_native_handle(started.root_reference);
    custody::close_native_handle(started.pty_master);
    if (!check(start_status == custody::Receive_status::MESSAGE &&
            started.kind == custody::Owner_event_kind::STARTED,
        "held-group helper reports workload birth before cleanup"))
    {
        return false;
    }
    bool ok = fixture.ready(started.root_exit.process_id);
    if (!normal_exit) {
        ok &= check(custody::send_owner_stop(run.channel, 0, &error), "held-group stop request is delivered");
    }
    bool root_exited = false;
    bool terminal = false;
    const auto deadline = Clock::now() + 3s;
    while (!terminal && Clock::now() < deadline) {
        custody::Owner_event event;
        const auto status = custody::receive_owner_event(run.channel, &event, 100, &error);
        if (status == custody::Receive_status::TIMEOUT) {
            continue;
        }
        custody::close_native_handle(event.root_reference);
        custody::close_native_handle(event.pty_master);
        if (!check(status == custody::Receive_status::MESSAGE, "held-group cleanup receipt is available")) {
            ok = false;
            break;
        }
        ok &= check(event.cleanup_scope == custody::Cleanup_scope::OWNED_PROCESS_GROUPS,
            "held-group receipts retain their explicit scope");
        if (event.kind == custody::Owner_event_kind::ROOT_EXITED) {
            ok &= check(!root_exited && event.root_exit.process_id == started.root_exit.process_id &&
                    event.root_exit.signal == (normal_exit ? 0 : SIGKILL) &&
                    (!normal_exit || event.root_exit.exit_code == 23),
                "helper reports its exact root exit once before terminal settlement");
            root_exited = true;
        }
        if (event.kind == custody::Owner_event_kind::TREE_SETTLEMENT &&
            event.settlement != custody::Tree_settlement::PENDING)
        {
            terminal = true;
            ok &= check(root_exited && event.settlement == custody::Tree_settlement::UNCONFIRMED &&
                    fixture.holding(),
                "root receipt precedes terminal unconfirmed while the inert member remains held");
            std::printf("held group helper normal_exit=%d settlement=%d detail=%s\n",
                normal_exit, static_cast<int>(event.settlement), event.detail.c_str());
        }
    }
    ok &= check(terminal, "held-group helper returns a bounded terminal receipt");
    if (terminal) {
        int status = 0;
        const bool waited = wait_child(run.helper, &status);
        if (waited) {
            run.helper.process_id = 0;
        }
        ok &= check(waited && WIFEXITED(status) && WEXITSTATUS(status) == 0 && fixture.holding(),
            "helper exits after retiring its root wait while fixture parent remains live");
    }
    ok &= fixture.release();
    std::printf("held group helper normal_exit=%d root_receipt=%d terminal=%d result=%d\n",
        normal_exit, root_exited, terminal, ok);
    return ok;
}

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
    if (mode == "held_group" || mode == "held_group_exit") {
        const bool normal_exit = mode == "held_group_exit";
        int detached[2] = {-1, -1};
        if (normal_exit && pipe(detached) != 0) {
            return 37;
        }
        const pid_t parent = fork();
        if (parent < 0) {
            return 38;
        }
        if (parent == 0) {
            if (normal_exit) {
                close(detached[0]);
            }
            const pid_t child = fork();
            if (child < 0) {
                _exit(39);
            }
            if (child == 0) {
                _exit(0);
            }
            siginfo_t info{};
            if (waitid(P_PID, child, &info, WEXITED | WNOWAIT) != 0) {
                _exit(40);
            }
            const pid_t original_group = getpgrp();
            if (setsid() < 0) {
                _exit(41);
            }
            const held_group_ready_t ready{getpid(), child, original_group, info.si_code};
            if (write(9, &ready, sizeof(ready)) != sizeof(ready)) {
                _exit(43);
            }
            if (normal_exit) {
                const char receipt = 'D';
                (void)write(detached[1], &receipt, 1);
                close(detached[1]);
            }
            // The holder self-releases even if the test cannot deliver EOF.
            pollfd release{10, POLLIN, 0};
            (void)poll(&release, 1, 8000);
            int status = 0;
            const bool reaped = waitpid(child, &status, 0) == child;
            const char receipt = reaped ? 'R' : 'E';
            (void)write(9, &receipt, 1);
            _exit(reaped ? 0 : 44);
        }
        close(9);
        close(10);
        if (normal_exit) {
            close(detached[1]);
            char receipt = 0;
            const bool ready = read_fixture_bytes(detached[0], &receipt, 1) && receipt == 'D';
            close(detached[0]);
            return ready ? 23 : 46;
        }
        (void)poll(nullptr, 0, 12000);
        return 45;
    }
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
    if (argc == 3 && std::string(argv[1]) == "--unsupported-pty-mode") {
        std::string error;
        return custody::prepare_parent_process(&error) && unsupported_pty_mode(argv[2]) ? 0 : 1;
    }
    if (argc != 2) {
        return 2;
    }
    std::string error;
    if (!custody::prepare_parent_process(&error)) {
        return 2;
    }
    if (std::string(argv[1]) == "--finite-send") {
        return finite_send_requires_nonblocking() ? 0 : 1;
    }
    if (std::string(argv[1]) == "--concurrent-retirement") {
        bool ok = concurrent_channel_retirement();
        ok &= directional_channel_eof();
        return ok ? 0 : 1;
    }
    bool ok = finite_send_requires_nonblocking();
    ok &= framed_transport();
    ok &= partial_frame_deadline_closes_stream();
    ok &= concurrent_channel_retirement();
    ok &= directional_channel_eof();
    ok &= rejected_record_releases_descriptors(Rejected_record::OVERSIZE);
    ok &= rejected_record_releases_descriptors(Rejected_record::LATE_RIGHTS);
    ok &= rejected_record_releases_descriptors(Rejected_record::TRUNCATED_RIGHTS);
    if (std::string(argv[1]) == "--framing") {
        return ok ? 0 : 1;
    }
    ok &= spawn_contract(argv[0]);
    ok &= unrelated_group_survives(argv[0]);
    ok &= exhausted_group_is_terminal(argv[0], Held_cleanup::FORCE_STOP);
    ok &= exhausted_group_is_terminal(argv[0], Held_cleanup::GRACEFUL_STOP);
    ok &= exhausted_group_is_terminal(argv[0], Held_cleanup::NORMAL_EXIT);
    ok &= exhausted_owner_receipts(argv[1], argv[0], false);
    ok &= exhausted_owner_receipts(argv[1], argv[0], true);
    ok &= command_owner(argv[1], false);
    ok &= command_owner(argv[1], true);
    ok &= command_owner(argv[1], false, custody::Owner_mode::PTY);
    ok &= stop_or_lose_owner(argv[1], argv[0], "stop");
    ok &= stop_or_lose_owner(argv[1], argv[0], "close");
    ok &= stop_or_lose_owner(argv[1], argv[0], "loss");
    ok &= unknown_capabilities_remain_local();
    ok &= unsupported_pty_mode(argv[1]);
    custody::Process_tree_containment containment;
    ok &= check(!containment.establish("fixture", &error) && !containment.available() && !error.empty(),
        "Darwin reports unavailable independent containment");
    ok &= check(!custody::terminate_coordinates({}, std::chrono::milliseconds(0), std::chrono::milliseconds(0)),
        "final owner loss cannot turn numeric coordinates into successful cleanup");
    std::puts(ok ? "PASS: Darwin command custody and framed transport" : "FAIL: Darwin custody");
    return ok ? 0 : 1;
}
