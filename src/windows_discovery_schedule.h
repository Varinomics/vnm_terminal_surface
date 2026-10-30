#pragma once

#include <chrono>
#include <optional>

namespace vnm_terminal::internal {

// A deadline exists only for actual pending activity. Starting a pass consumes
// that activity; notifications during the pass request one subsequent pass.
class Windows_discovery_schedule
{
public:
    using Clock = std::chrono::steady_clock;

    void request() { m_pending = true; }
    bool pending() const { return m_pending; }

    bool ready(Clock::time_point now) const
    {
        return m_pending && (!m_next_start || now >= *m_next_start);
    }

    std::optional<Clock::time_point> deadline() const
    {
        return m_pending ? m_next_start : std::nullopt;
    }

    void started(Clock::time_point now)
    {
        m_pending = false;
        execution_started(now);
    }

    void execution_started(Clock::time_point now) { m_next_start = now + std::chrono::seconds(1); }

private:
    bool m_pending = false;
    std::optional<Clock::time_point> m_next_start;
};

} // namespace vnm_terminal::internal
