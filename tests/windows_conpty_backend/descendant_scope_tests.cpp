#include "descendant_scope_tests.h"
#include "../helpers/test_check.h"
#include "vnm_terminal/internal/windows_conpty_backend.h"
#include "vnm_terminal/process_cpu_time.h"

#include <QByteArray>
#include <QDir>
#include <QScopeGuard>
#include <QString>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>

namespace term = vnm_terminal::internal;
using vnm_terminal::test_helpers::check;

namespace {

using Fault = term::Windows_conpty_descendant_scan_fault_for_testing;
using Scope = vnm_terminal::Process_cpu_scope;
constexpr std::chrono::seconds k_timeout(20);

template<class Predicate>
bool wait_until(Predicate ready)
{
    const auto deadline = std::chrono::steady_clock::now() + k_timeout;
    do {
        if (ready()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

bool has_scope(term::Windows_conpty_backend& backend, Scope expected, std::string_view message)
{
    const auto sample = backend.process_cpu_time();
    if (sample && sample->scope != expected) {
        std::cerr << "CPU scope: actual=" << (int)sample->scope << " expected=" << (int)expected << '\n';
    }
    return check(sample && sample->scope == expected, message);
}

bool test_failure_scope(Fault fault)
{
    const auto first_control  = std::make_shared<term::Windows_conpty_descendant_scan_control_for_testing>();
    const auto second_control = std::make_shared<term::Windows_conpty_descendant_scan_control_for_testing>();
    std::atomic_bool first_exited{false}, second_exited{false};
    term::Windows_conpty_backend first, second;
    const auto clear_fault = qScopeGuard([&] { first_control->fault.store(Fault::NONE); });
    if (!check(first.set_descendant_scan_control_for_testing(first_control) &&
        second.set_descendant_scan_control_for_testing(second_control),
        "native scanner failure controls are installed before start"))
    {
        return false;
    }

    term::Terminal_launch_config config;
    config.argv = {QDir::fromNativeSeparators(qEnvironmentVariable("ComSpec")),
        QStringLiteral("/d"), QStringLiteral("/q"), QStringLiteral("/k")};
    config.initial_grid_size = {24, 80};
    const auto callbacks = [](std::atomic_bool& exited) {
        term::Terminal_backend_callbacks result;
        result.output_received = [](QByteArray) {};
        result.process_exited  = [&exited](auto) { exited.store(true); };
        result.error_reported  = [](const auto& error) { std::cerr << error.message.toStdString() << '\n'; };
        return result;
    };
    if (!check(!term::is_backend_rejection(first.start(config, callbacks(first_exited))) &&
        !term::is_backend_rejection(second.start(config, callbacks(second_exited))),
        "two ordinary shells start for native scanner scope checks"))
    {
        return false;
    }
    if (!check(wait_until([&] {
        return first_control->successful_member_queries.load() > 0 &&
            second_control->successful_member_queries.load() > 0;
    }), "both families complete native membership queries before fault injection"))
    {
        return false;
    }
    bool ok = has_scope(first, Scope::WORKLOAD_FAMILY, "successful discovery retains native workload scope");
    ok &= has_scope(second, Scope::WORKLOAD_FAMILY, "the other family starts with native workload scope");

    first_control->fault.store(fault);
    // The second fault proves that the preceding pass has completed its catch
    // path; sampling cannot race the first injected failure's notification.
    if (!check(wait_until([&] { return first_control->injected_failures.load() >= 2; }),
        "the requested native scanner failure boundary is reached"))
    {
        return false;
    }
    ok &= has_scope(first, Scope::OBSERVED_FAMILY,
        fault == Fault::MEMBER_QUERY ? "member query failure downgrades the public CPU sample"
                                    : "active-family allocation failure downgrades the public CPU sample");
    const Scope other_scope = fault == Fault::MEMBER_QUERY ? Scope::WORKLOAD_FAMILY : Scope::OBSERVED_FAMILY;
    ok &= has_scope(second, other_scope,
        "member failure is local, while failed collection also marks families not yet collected");

    const auto first_queries  = first_control->successful_member_queries.load();
    const auto second_queries = second_control->successful_member_queries.load();
    first_control->fault.store(Fault::NONE);
    if (!check(wait_until([&] {
        return first_control->successful_member_queries.load() > first_queries &&
            second_control->successful_member_queries.load() > second_queries;
    }), "native membership queries recover after the injected failure clears"))
    {
        return false;
    }
    ok &= has_scope(first, Scope::OBSERVED_FAMILY, "discovery failure scope remains sticky after recovery");
    ok &= has_scope(second, other_scope, "recovery preserves the other family's coverage history");

    (void)second.terminate();
    if (!check(wait_until([&] { return second_exited.load(); }), "the second family settles before disabling checks")) {
        return false;
    }
    if (fault == Fault::MEMBER_QUERY) {
        const auto failures = first_control->injected_failures.load();
        first_control->fault.store(Fault::ACTIVE_FAMILY_ALLOCATION);
        if (!check(wait_until([&] { return first_control->injected_failures.load() >= failures + 2; }),
            "collection fails again after the unaffected family is disabled"))
        {
            return false;
        }
        ok &= has_scope(second, Scope::WORKLOAD_FAMILY, "disabled families do not acquire discovery failures");
        first_control->fault.store(Fault::NONE);
    }
    (void)first.terminate();
    ok &= check(wait_until([&] { return first_exited.load(); }), "the first family settles after scope checks");
    return ok;
}

}

bool test_descendant_scan_failure_scopes()
{
    // The documented CPU contract downgrades coverage when scanner resources
    // prevent discovery, while native accounting itself remains available.
    bool ok = test_failure_scope(Fault::MEMBER_QUERY);
    ok &= test_failure_scope(Fault::ACTIVE_FAMILY_ALLOCATION);
    return ok;
}
