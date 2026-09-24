#include <vnm_process_custody/owner.h>

#include "../src/owner_protocol.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace custody = vnm::process_custody;
using namespace std::chrono_literals;

namespace {

volatile sig_atomic_t receive_signal_observed = 0;

void interrupt_receive(int)
{
    receive_signal_observed = 1;
}

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

custody::Owner_start_request large_request()
{
    custody::Owner_start_request request;
    request.mode = custody::Owner_mode::PTY;
    request.process.executable = "/bin/true";
    request.process.argv = {"/bin/true"};
    for (int index = 0; index != 16; ++index) {
        request.process.environment.push_back(
            "ENTRY_" + std::to_string(index) + "=" + std::string(32768, 'x'));
    }
    return request;
}

bool set_nonblocking(custody::Duplex_channel& channel, int* original_flags)
{
    *original_flags = ::fcntl(channel.native(), F_GETFL, 0);
    return *original_flags >= 0 &&
        ::fcntl(channel.native(), F_SETFL, *original_flags | O_NONBLOCK) == 0;
}

bool delayed_reader(int delay_ms)
{
    custody::Duplex_channel sender;
    custody::Duplex_channel receiver;
    std::string error;
    if (!custody::Duplex_channel::create_pair(
            custody::k_owner_message_bytes, &sender, &receiver, &error))
    {
        return check(false, "delayed-reader channel setup");
    }

    int original_flags = -1;
    if (!check(set_nonblocking(sender, &original_flags), "set nonblocking channel mode")) {
        return false;
    }
    int pipe_handles[2];
    if (::pipe2(pipe_handles, O_CLOEXEC) != 0) {
        return check(false, "descriptor-transfer pipe setup");
    }

    auto request = large_request();
    request.process.inherited = {{pipe_handles[1], 9}};
    custody::detail::Owner_message received;
    custody::Receive_status receive_status = custody::Receive_status::FAILED;
    std::string receive_error;
    std::thread reader([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        receive_status = custody::detail::receive_message(
            receiver, &received, 2000, &receive_error);
    });
    const bool sent = custody::send_owner_start_until(
        sender, request, std::chrono::steady_clock::now() + 2s, &error);
    if (!sent) {
        (void)::shutdown(sender.native(), SHUT_WR);
    }
    reader.join();

    bool ok = check(sent, "bounded START tolerates transient backpressure");
    ok &= check(receive_status == custody::Receive_status::MESSAGE &&
            received.kind == custody::detail::Message_kind::START &&
            received.start.process.environment == request.process.environment,
        "fragmented START is reassembled exactly once without byte loss");
    ok &= check(received.start.process.inherited.size() == 1U &&
            received.start.process.inherited.front().child_number == 9,
        "the initial record transfers exactly one intended descriptor");
    if (received.start.process.inherited.size() == 1U) {
        ok &= check(::fcntl(
                received.start.process.inherited.front().parent_handle, F_GETFD) >= 0,
            "the received descriptor remains valid");
    }
    custody::detail::close_start_descriptors(received.start);
    (void)::close(pipe_handles[0]);
    (void)::close(pipe_handles[1]);
    ok &= check(::fcntl(sender.native(), F_GETFL, 0) == (original_flags | O_NONBLOCK),
        "deadline sender preserves the caller's nonblocking mode");

    std::vector<std::uint8_t> buffer(custody::k_owner_message_bytes);
    const auto extra = receiver.receive(buffer.data(), buffer.size(), 0);
    for (const int descriptor : extra.descriptors) {
        (void)::close(descriptor);
    }
    ok &= check(extra.status == custody::Receive_status::TIMEOUT,
        "no duplicate request record remains queued");
    return ok;
}

bool absent_reader_times_out()
{
    custody::Duplex_channel sender;
    custody::Duplex_channel receiver;
    std::string error;
    if (!custody::Duplex_channel::create_pair(
            custody::k_owner_message_bytes, &sender, &receiver, &error))
    {
        return check(false, "blocked-sender channel setup");
    }

    int original_flags = -1;
    if (!check(set_nonblocking(sender, &original_flags), "set nonblocking blocked sender")) {
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + 80ms;
    const bool sent = custody::send_owner_start_until(sender, large_request(), deadline, &error);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    bool ok = check(!sent, "an absent reader cannot complete a large fragmented START");
    ok &= check(elapsed >= 80ms && elapsed < 2s, "all fragments share one absolute deadline");
    ok &= check(::fcntl(sender.native(), F_GETFL, 0) == (original_flags | O_NONBLOCK),
        "timeout preserves the caller's channel mode");
    ok &= check(::shutdown(sender.native(), SHUT_WR) == 0,
        "caller can abandon a partial request by sending EOF");

    custody::detail::Owner_message partial;
    const auto status = custody::detail::receive_message(receiver, &partial, 500, &error);
    custody::detail::close_start_descriptors(partial.start);
    ok &= check(status == custody::Receive_status::FAILED,
        "partial START followed by EOF is rejected");
    return ok;
}

bool expired_deadline_sends_nothing()
{
    custody::Duplex_channel sender;
    custody::Duplex_channel receiver;
    std::string error;
    if (!custody::Duplex_channel::create_pair(
            custody::k_owner_message_bytes, &sender, &receiver, &error))
    {
        return false;
    }
    int original_flags = -1;
    if (!set_nonblocking(sender, &original_flags)) {
        return check(false, "set nonblocking expired sender");
    }

    const bool sent = custody::send_owner_start_until(
        sender, large_request(), std::chrono::steady_clock::now() - 1ms, &error);
    bool ok = check(!sent && error.find("timed out") != std::string::npos,
        "an expired deadline rejects the request");
    std::vector<std::uint8_t> buffer(custody::k_owner_message_bytes);
    const auto received = receiver.receive(buffer.data(), buffer.size(), 0);
    for (const int descriptor : received.descriptors) {
        (void)::close(descriptor);
    }
    ok &= check(received.status == custody::Receive_status::TIMEOUT,
        "an expired deadline sends no fragment");
    return ok;
}

bool interrupted_receive_keeps_its_deadline()
{
    custody::Duplex_channel sender;
    custody::Duplex_channel receiver;
    std::string error;
    if (!custody::Duplex_channel::create_pair(
            custody::k_owner_message_bytes, &sender, &receiver, &error))
    {
        return check(false, "interrupted-receive channel setup");
    }

    struct sigaction action{};
    struct sigaction previous_action{};
    action.sa_handler = interrupt_receive;
    ::sigemptyset(&action.sa_mask);
    if (::sigaction(SIGUSR1, &action, &previous_action) != 0) {
        return check(false, "install receive-interrupt handler");
    }

    sigset_t signals;
    sigset_t previous_mask;
    ::sigemptyset(&signals);
    ::sigaddset(&signals, SIGUSR1);
    if (::pthread_sigmask(SIG_UNBLOCK, &signals, &previous_mask) != 0) {
        (void)::sigaction(SIGUSR1, &previous_action, nullptr);
        return check(false, "unblock receive-interrupt signal");
    }

    receive_signal_observed = 0;
    const pthread_t receiving_thread = ::pthread_self();
    std::atomic<bool> finished{false};
    std::thread interrupter([&] {
        const auto signal_deadline = std::chrono::steady_clock::now() + 600ms;
        while (!finished.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < signal_deadline)
        {
            std::this_thread::sleep_for(5ms);
            if (!finished.load(std::memory_order_acquire)) {
                (void)::pthread_kill(receiving_thread, SIGUSR1);
            }
        }
    });

    const auto started = std::chrono::steady_clock::now();
    custody::Owner_event event;
    const auto status = custody::receive_owner_event(receiver, &event, 80, &error);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    finished.store(true, std::memory_order_release);
    interrupter.join();

    (void)::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
    (void)::sigaction(SIGUSR1, &previous_action, nullptr);

    std::printf("interrupted receive: requested=80ms elapsed=%lldms\n",
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    bool ok = check(receive_signal_observed != 0, "the receiving thread was interrupted");
    ok &= check(status == custody::Receive_status::TIMEOUT, "silent owner receive times out");
    ok &= check(elapsed >= 80ms && elapsed < 300ms,
        "receive interruptions consume, rather than restart, the finite timeout");
    return ok;
}

bool negative_timeout_waits_for_message()
{
    custody::Duplex_channel sender;
    custody::Duplex_channel receiver;
    std::string error;
    if (!custody::Duplex_channel::create_pair(
            custody::k_owner_message_bytes, &sender, &receiver, &error))
    {
        return check(false, "infinite-receive channel setup");
    }

    const std::array<std::uint8_t, 1> payload{0x5a};
    auto send_status = custody::Io_status::FAILED;
    std::thread writer([&] {
        std::this_thread::sleep_for(30ms);
        send_status = sender.send(payload.data(), payload.size());
    });

    std::array<std::uint8_t, custody::k_owner_message_bytes> received_payload{};
    const auto received = receiver.receive(
        received_payload.data(), received_payload.size(), -1);
    writer.join();

    bool ok = check(send_status == custody::Io_status::TRANSFERRED,
        "infinite-receive fixture sends one message");
    ok &= check(received.status == custody::Receive_status::MESSAGE &&
            received.size == payload.size() && received_payload[0] == payload[0],
        "negative timeout waits indefinitely until a message arrives");
    return ok;
}

bool closed_peer_fails_without_sigpipe()
{
    custody::Duplex_channel sender;
    custody::Duplex_channel receiver;
    std::string error;
    if (!custody::Duplex_channel::create_pair(
            custody::k_owner_message_bytes, &sender, &receiver, &error))
    {
        return false;
    }
    int original_flags = -1;
    if (!set_nonblocking(sender, &original_flags)) {
        return check(false, "set nonblocking closed-peer sender");
    }
    receiver.close();
    const bool sent = custody::send_owner_start_until(
        sender, large_request(), std::chrono::steady_clock::now() + 1s, &error);
    return check(!sent && error.find("closed") != std::string::npos,
        "peer loss fails without SIGPIPE");
}

} // namespace

int main()
{
    bool ok = true;
    for (const int delay_ms : std::array{0, 50, 200}) {
        ok &= delayed_reader(delay_ms);
    }
    ok &= absent_reader_times_out();
    ok &= expired_deadline_sends_nothing();
    ok &= closed_peer_fails_without_sigpipe();
    ok &= interrupted_receive_keeps_its_deadline();
    ok &= negative_timeout_waits_for_message();
    std::puts(ok
        ? "PASS: deadline START and receive timeout semantics remain bounded"
        : "FAIL: deadline START");
    return ok ? 0 : 1;
}
