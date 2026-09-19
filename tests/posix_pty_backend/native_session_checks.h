#pragma once

#include "helpers/test_check.h"
#include "../../src/native_backend_cleanup_owner.h"
#include "vnm_terminal/internal/terminal_session.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

namespace vnm_terminal::test_helpers {

// Exercise the product session's queued callback ingress, not direct callbacks
// or a scripted backend. The native fixture remains disposable in every case.
template<class Backend_factory>
bool check_native_session_lifecycle(
    Backend_factory factory, internal::Terminal_launch_config launch)
{
    namespace term = internal;
    using Clock = std::chrono::steady_clock;
    bool ok = true;
    for (int scenario = 0; scenario != 3; ++scenario) {
        auto notifications = std::make_shared<std::atomic<unsigned>>(0U);
        term::Terminal_session_config config;
        config.trace_output_chunk_limit = 128U;
        config.backend_event_notifier = [notifications] {
            notifications->fetch_add(1U, std::memory_order_relaxed);
        };
        launch.argv = {launch.argv.front(), scenario == 0
            ? QStringLiteral("--quick-exit") : QStringLiteral("--hold-open")};
        auto session = std::make_unique<term::Terminal_session>(factory(), config);
        const auto started = session->start(launch);
        ok &= check(started.code == term::Terminal_session_result_code::ACCEPTED,
            "real native Terminal_session starts");
        const auto wait_for = [&](auto predicate) {
            const auto deadline = Clock::now() + std::chrono::seconds(15);
            do {
                session->process_backend_callback_events();
                if (predicate()) {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (Clock::now() < deadline);
            return false;
        };
        const QByteArray marker = scenario == 0 ? "quick-exit" : "hold-open";
        const bool output = wait_for([&] {
            QByteArray received;
            for (const auto& chunk : session->output_chunks()) {
                received += chunk;
            }
            return received.contains(marker);
        });
        ok &= check(output, "native output crosses queued session ingress");
        ok &= check(session->latest_render_snapshot().has_value(),
            "real native session publishes a render snapshot");
        if (scenario == 1) {
            ok &= check(session->resize(QSizeF(800, 600), {30, 100}).code ==
                    term::Terminal_session_result_code::ACCEPTED,
                "native session resize is accepted");
            ok &= check(session->write_user_bytes(QByteArrayLiteral("x")).code ==
                    term::Terminal_session_result_code::ACCEPTED,
                "native session write is accepted");
            ok &= check(session->terminate().code == term::Terminal_session_result_code::ACCEPTED,
                "native session termination is accepted");
        }
        const bool exited = scenario != 2 && wait_for([&] {
            return session->exit_status().has_value();
        });
        if (scenario != 2) {
            ok &= check(exited, "native exit crosses session callback ingress");
        }
        if (scenario == 0 && exited) {
            ok &= check(session->exit_status()->exit_code == 0,
                "session preserves normal native exit status");
        }
        session.reset();
        const bool settled = term::Native_backend_cleanup_reservation::wait_until_idle(
            Clock::now() + std::chrono::seconds(15));
        ok &= check(settled, "session disposal settles actual native cleanup");
        ok &= check(notifications->load() != 0U, "product owner notifier receives native events");
        std::cout << "NATIVE_SESSION scenario=" << scenario << " output=" << output
                  << " exit=" << exited << " cleanup_settled=" << settled
                  << " retained=" << term::Native_backend_cleanup_reservation::retained_tasks()
                  << '\n';
    }
    return ok;
}

} // namespace vnm_terminal::test_helpers
