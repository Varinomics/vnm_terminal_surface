#include <vnm_process_custody/owner.h>

#include "../src/owner_protocol.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

namespace {

thread_local std::ptrdiff_t allocation_budget = -1;

} // namespace

[[gnu::noinline]] void* operator new(std::size_t size)
{
    if (allocation_budget == 0) {
        allocation_budget = -1;
        throw std::bad_alloc();
    }
    if (allocation_budget > 0) {
        --allocation_budget;
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

[[gnu::noinline]] void operator delete(void* memory) noexcept { std::free(memory); }
[[gnu::noinline]] void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

namespace custody = vnm::process_custody;

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

bool receive_with_allocation_failure(bool owner_request, std::ptrdiff_t budget, bool* threw)
{
    custody::Duplex_channel sender, receiver;
    std::string error;
    if (!check(custody::Duplex_channel::create_pair(
            owner_request ? custody::k_owner_message_bytes : 64, &sender, &receiver, &error),
        "resource fixture opens the native channel"))
    {
        return false;
    }
    int pipe[2];
    if (!check(::pipe2(pipe, O_CLOEXEC | O_NONBLOCK) == 0,
        "resource fixture opens the descriptor canary"))
    {
        return false;
    }

    bool sent = false;
    if (owner_request) {
        custody::Owner_start_request request;
        request.process.executable = "/usr/bin/fixture";
        request.process.working_directory = std::string(128, 'd');
        request.process.argv = {request.process.executable, std::string(128, 'a')};
        request.process.environment = {"PATH=/usr/bin:/bin", "RESOURCE_FIXTURE=enabled"};
        request.process.inherited = {{pipe[1], 3}, {pipe[1], 4}};
        sent = custody::send_owner_start(sender, request, &error);
    }
    else {
        const std::uint8_t payload[] = {7, 11};
        sent = sender.send_descriptors(payload, sizeof(payload), {pipe[1], pipe[1]}) ==
            custody::Io_status::TRANSFERRED;
    }
    ::close(pipe[1]);
    if (!check(sent, "resource fixture transfers both canary descriptors")) {
        ::close(pipe[0]);
        return false;
    }

    *threw = false;
    bool delivered = false;
    allocation_budget = budget;
    try {
        if (owner_request) {
            custody::detail::Owner_message message;
            const auto status = custody::detail::receive_message(receiver, &message, 1000, &error);
            delivered = status == custody::Receive_status::MESSAGE &&
                message.kind == custody::detail::Message_kind::START &&
                message.start.process.inherited.size() == 2;
            custody::detail::close_start_descriptors(message.start);
        }
        else {
            std::uint8_t payload[64];
            const auto received = receiver.receive(payload, sizeof(payload), 1000);
            delivered = received.status == custody::Receive_status::MESSAGE &&
                received.size == 2 && received.descriptors.size() == 2;
            for (const auto descriptor : received.descriptors) {
                custody::close_native_handle(descriptor);
            }
        }
    }
    catch (const std::bad_alloc&) {
        *threw = true;
    }
    allocation_budget = -1;
    receiver.close();

    // EOF proves that neither an installed right nor an unread channel record
    // retains the writer after a failed receive has unwound and retired.
    char byte;
    const auto remaining = ::read(pipe[0], &byte, sizeof(byte));
    ::close(pipe[0]);
    bool ok = check(*threw || delivered, "successful receive transfers the complete descriptor set");
    ok &= check(remaining == 0, "receive retirement releases every transferred descriptor");
    if (!ok) {
        std::fprintf(stderr, "  owner_request=%d allocation_budget=%td\n", owner_request, budget);
    }
    return ok;
}

bool allocation_failures(bool owner_request)
{
    bool ok = true;
    for (std::ptrdiff_t budget = 0; budget < 64; ++budget) {
        bool threw = false;
        ok &= receive_with_allocation_failure(owner_request, budget, &threw);
        if (!threw) {
            return ok;
        }
    }
    return check(false, "resource fixture reaches an uninterrupted receive");
}

} // namespace

int main()
{
    bool ok = allocation_failures(false);
    ok &= allocation_failures(true);
    return ok ? 0 : 1;
}
