#include "bounded_render_retry.h"

#include <array>
#include <cstdio>
#include <latch>
#include <optional>
#include <thread>

namespace {

using Retry = vnm_terminal::internal::Bounded_render_retry;

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

bool finite_budget_and_single_pending_ticket()
{
    Retry retry;
    bool ok = true;
    constexpr std::array delays{16, 32, 64, 128, 256, 512};
    for (const int delay : delays) {
        const auto ticket = retry.failed();
        if (!check(ticket.has_value(), "each permitted retry produces a ticket")) {
            return false;
        }
        ok &= check(ticket->delay_ms == delay, "retry delay follows the finite backoff");
        ok &= check(retry.current(*ticket), "new ticket is current");
        ok &= check(!retry.failed(), "repeated preparation cannot queue another update");
        ok &= check(retry.consume(*ticket), "current ticket is consumed exactly once");
        ok &= check(!retry.current(*ticket) && !retry.consume(*ticket),
            "consumed ticket cannot schedule another repaint");
    }
    const auto terminal = retry.failed();
    if (!check(terminal.has_value(), "exhaustion has one terminal notification")) {
        return false;
    }
    ok &= check(terminal->delay_ms < 0, "terminal notification does not request a repaint");
    ok &= check(retry.consume(*terminal), "terminal notification can be consumed once");
    ok &= check(!retry.failed(), "consuming exhaustion never opens another retry");
    return ok;
}

bool replacement_and_close_revoke_tickets()
{
    Retry retry;
    const auto old = retry.failed();
    if (!old) {
        return false;
    }
    retry.reset();
    bool ok = check(!retry.current(*old) && !retry.consume(*old), "replacement revokes old timers");
    const auto fresh = retry.failed();
    if (!fresh) {
        return false;
    }
    ok &= check(fresh->delay_ms == 16 && fresh->epoch != old->epoch && fresh->serial != old->serial,
        "new ownership opens a fresh budget without ticket aliasing");
    retry.close();
    ok &= check(!retry.current(*fresh) && !retry.consume(*fresh), "destruction revokes pending GUI work");
    retry.reset();
    ok &= check(!retry.failed(), "reset cannot resurrect a closed owner");
    return ok;
}

bool concurrent_failure_has_one_owner()
{
    Retry retry;
    std::latch start(1);
    std::array<std::optional<Retry::ticket_t>, 2> tickets;
    std::thread first([&] {
        start.wait();
        tickets[0] = retry.failed();
    });
    std::thread second([&] {
        start.wait();
        tickets[1] = retry.failed();
    });
    start.count_down();
    first.join();
    second.join();
    return check(tickets[0].has_value() != tickets[1].has_value(),
        "concurrent failures still own only one queued repaint");
}

} // namespace

int main()
{
    bool ok = finite_budget_and_single_pending_ticket();
    ok &= replacement_and_close_revoke_tickets();
    ok &= concurrent_failure_has_one_owner();
    std::puts(ok ? "PASS: finite retry budget, ticket ownership, revocation and concurrent admission"
                 : "FAIL: bounded render retry");
    return ok ? 0 : 1;
}
