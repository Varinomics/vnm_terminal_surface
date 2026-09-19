#include "native_backend_cleanup_owner.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <stdexcept>
#if !defined(_WIN32)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

// Deterministic allocation failures apply only to the constructing thread.
// Existing cleanup workers are not faulted by another backend's admission.
// Keep the replacement allocation boundary opaque to optimized callers. GCC
// otherwise diagnoses the valid malloc-backed replacement delete as a mismatched
// built-in new/free pair when it inlines the fault-injection implementation.
#if defined(_MSC_VER)
#define TEST_ALLOCATION_BOUNDARY __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define TEST_ALLOCATION_BOUNDARY __attribute__((noinline))
#else
#define TEST_ALLOCATION_BOUNDARY
#endif
namespace { thread_local int allocation_countdown = -1; }
TEST_ALLOCATION_BOUNDARY void* operator new(std::size_t size)
{
    if (allocation_countdown == 0) {
        throw std::bad_alloc();
    }
    if (allocation_countdown > 0) {
        --allocation_countdown;
    }
    if (auto* result = std::malloc(size == 0 ? 1 : size)) {
        return result;
    }
    throw std::bad_alloc();
}
TEST_ALLOCATION_BOUNDARY void operator delete(void* value) noexcept { std::free(value); }
TEST_ALLOCATION_BOUNDARY void operator delete(void* value, std::size_t) noexcept { std::free(value); }
TEST_ALLOCATION_BOUNDARY void* operator new[](std::size_t size) { return ::operator new(size); }
TEST_ALLOCATION_BOUNDARY void operator delete[](void* value) noexcept { ::operator delete(value); }
TEST_ALLOCATION_BOUNDARY void operator delete[](void* value, std::size_t) noexcept { ::operator delete(value); }

#undef TEST_ALLOCATION_BOUNDARY

namespace {
using Reservation = vnm_terminal::internal::Native_backend_cleanup_reservation;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

void require(bool condition, const char* diagnostic)
{
    if (!condition) {
        throw std::runtime_error(diagnostic);
    }
}
template<class Predicate> bool await(Predicate predicate)
{
    const auto deadline = Clock::now() + 3s;
    while (!predicate()) {
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

struct Audit
{
    std::atomic_bool calls_drained{true};
    std::atomic_bool observation_allowed{true};
    std::atomic_bool cleanup_entered{false};
    std::atomic_bool cleaned{false};
    std::atomic_bool disposed{false};
    std::atomic_bool native_error{false};
};

struct Owner
{
    std::shared_ptr<Audit> audit;
#if !defined(_WIN32)
    pid_t child = -1;
#endif
    Reservation reservation;

    explicit Owner(std::shared_ptr<Audit> state)
        : audit(std::move(state)), reservation(this, cleanup, dispose)
    {}

    static void cleanup(void* context) noexcept
    {
        auto* owner = static_cast<Owner*>(context);
        owner->audit->cleanup_entered = true;
        while (!owner->audit->calls_drained.load() || !owner->audit->observation_allowed.load()) {
            std::this_thread::sleep_for(1ms);
        }
#if !defined(_WIN32)
        if (owner->child > 0) {
            // This worker is the only consuming waiter after explicit handoff.
            // No other path reaps this exact child or signals it after reaping.
            if (::kill(owner->child, SIGKILL) != 0) {
                owner->audit->native_error = true;
            }
            int status = 0;
            pid_t waited;
            do { waited = ::waitpid(owner->child, &status, 0); } while (waited < 0 && errno == EINTR);
            if (waited != owner->child || !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
                owner->audit->native_error = true;
            }
        }
#endif
        owner->audit->cleaned = true;
    }

    static void dispose(void* context) noexcept
    {
        auto* owner = static_cast<Owner*>(context);
        owner->audit->disposed = true;
        delete owner;
    }

    void launch()
    {
#if !defined(_WIN32)
        child = ::fork();
        if (child == 0) {
            for (;;) {
                ::pause();
            }
        }
        require(child > 0, "Fixture fork failed before native publication");
#endif
    }
};

// Failure cleanup must release the artificial barriers even if a test fails;
// otherwise the correct module-unload barrier would intentionally remain held.
struct Release_guards
{
    std::shared_ptr<Audit> first;
    std::shared_ptr<Audit> second;
    ~Release_guards()
    {
        first->calls_drained = first->observation_allowed = true;
        second->calls_drained = second->observation_allowed = true;
    }
};

void bounded_handoff_and_independent_progress()
{
    auto first = std::make_shared<Audit>();
    auto second = std::make_shared<Audit>();
    Release_guards release{first, second};
    first->observation_allowed = false;
    auto* one = new Owner(first);
    auto* two = new Owner(second);
    one->launch();
    two->launch();
    const auto before = Clock::now();
    one->reservation.release();
    require(Clock::now() - before < 100ms, "Facade release waited for native cleanup");
    two->reservation.release();
    require(await([&] { return second->disposed.load(); }), "One pending backend blocked another cleanup");
    require(first->cleanup_entered && !first->cleaned && !first->disposed,
        "Negative observation became native completion or freed its owner");
    require(!Reservation::wait_until_idle(Clock::now() + 10ms), "Pending native ownership vanished from registry");
    first->observation_allowed = true;
    require(await([&] { return first->disposed.load(); }), "Retained cleanup failed to complete later");
    require(!first->native_error && !second->native_error, "The exact fixture child was not reaped by its owner");
    require(Reservation::wait_until_idle(Clock::now() + 3s), "Completed joinable workers were not reaped");
}

void failed_start_then_facade_release()
{
    auto state = std::make_shared<Audit>();
    auto* owner = new Owner(state);
    owner->launch();
    owner->reservation.request();
    require(await([&] { return owner->reservation.native_complete(); }), "Failed-start cleanup did not run");
    require(!state->disposed, "Failed-start cleanup deleted a still-owned facade Impl");
    owner->reservation.release();
    require(await([&] { return state->disposed.load(); }), "Facade release did not complete disposal");
    require(!state->native_error, "Failed-start native cleanup lost its exact child");
}

void public_call_gate()
{
    auto state = std::make_shared<Audit>();
    auto other = std::make_shared<Audit>();
    Release_guards release{state, other};
    state->calls_drained = false;
    auto* owner = new Owner(state);
    owner->reservation.release();
    require(await([&] { return state->cleanup_entered.load(); }), "Cleanup worker never began");
    require(!state->cleaned && !state->disposed, "An in-flight public call lost its Impl");
    state->calls_drained = true;
    require(await([&] { return state->disposed.load(); }), "Drained public call did not permit disposal");
}

void prebirth_allocation_failures()
{
    int failures = 0;
    for (int point = 0; point != 8; ++point) {
        auto state = std::make_shared<Audit>();
        Owner* owner = nullptr;
        allocation_countdown = point;
        try {
            owner = new Owner(state);
        }
        catch (const std::bad_alloc&) {
            ++failures;
        }
        allocation_countdown = -1;
        // Deliberately no launch: cancellation retires the pre-birth ticket and
        // may not call cleanup/dispose against a destroyed construction object.
        delete owner;
        require(Reservation::wait_until_idle(Clock::now() + 3s), "Pre-birth refusal leaked a reserved worker");
        require(!state->cleanup_entered && !state->disposed, "A failed reservation dispatched a native owner");
    }
    require(failures >= 3, "Allocation injections did not exercise reservation failure paths");
}

void unconfirmed_disposal_retains_outcome()
{
    struct Lost_owner
    {
        std::shared_ptr<Audit> audit;
        Reservation reservation;

        explicit Lost_owner(std::shared_ptr<Audit> state)
            : audit(std::move(state)), reservation(this,
                [](void*) noexcept {},
                [](void* context) noexcept {
                    auto* owner = static_cast<Lost_owner*>(context);
                    owner->audit->disposed = true;
                    delete owner;
                },
                [](void*) noexcept { return false; })
        {}
    };
    auto audit = std::make_shared<Audit>();
    auto* owner = new Lost_owner(audit);
    owner->reservation.request();
    std::this_thread::sleep_for(10ms);
    require(!owner->reservation.native_complete(), "Owner loss fabricated native completion");
    owner->reservation.release();
    require(await([&] { return audit->disposed.load(); }), "Lost owner retained a live Impl indefinitely");
    require(await([] { return Reservation::unconfirmed_tasks() == 1; }),
        "Disposal discarded the unconfirmed outcome record");
    require(!Reservation::wait_until_idle(Clock::now() + 10ms),
        "Disposed unconfirmed record was reported as settled");
    std::puts("CLEANUP_OUTCOME disposed=1 unconfirmed_records=1 settled=0");
}
} // namespace

int main()
{
    try {
        prebirth_allocation_failures();
        bounded_handoff_and_independent_progress();
        failed_start_then_facade_release();
        public_call_gate();
        require(Reservation::wait_until_idle(Clock::now() + 3s), "Cleanup registry was not empty at fixture exit");
        unconfirmed_disposal_retains_outcome();
        std::puts("PASS: pre-birth reservation, bounded handoff, sole-wait native cleanup, failed-start ownership and call drain");
#if defined(_WIN32)
        std::puts("NOTE: core ownership only on Windows; native ConPTY tests are separate");
#endif
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error) {
        allocation_countdown = -1;
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
