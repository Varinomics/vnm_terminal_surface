#pragma once

#include <chrono>
#include <limits>

namespace vnm::process_custody {

/// A steady-clock deadline shared by the custodian, the supervisor, the
/// desktop client and the tests. Every bound of the process protocol
/// (terminate-to-kill, reap, custodian start) is measured against this one
/// clock so a wall-clock adjustment can neither lengthen nor shorten a bound
/// while it is running.
class Deadline
{
public:
    using Clock = std::chrono::steady_clock;

    static Deadline after(std::chrono::milliseconds budget)
    {
        Deadline deadline;
        deadline.m_never = false;
        deadline.m_at    = Clock::now() + budget;
        return deadline;
    }

    static Deadline never() { return Deadline(); }

    bool expired() const { return !m_never && Clock::now() >= m_at; }

    std::chrono::milliseconds remaining() const
    {
        if (m_never) {
            return std::chrono::milliseconds::max();
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(m_at - Clock::now());
        return left.count() < 0 ? std::chrono::milliseconds(0) : left;
    }

    /// Remaining time as a wait timeout: -1 means "no timeout", which is the
    /// value poll() takes and which the Windows wait maps onto INFINITE;
    /// otherwise the remaining milliseconds clamped to [0, INT_MAX].
    int remaining_ms() const
    {
        if (m_never) {
            return -1;
        }
        const auto left = remaining().count();
        if (left > std::numeric_limits<int>::max()) {
            return std::numeric_limits<int>::max();
        }
        return static_cast<int>(left);
    }

    /// The earlier of two wait timeouts in the -1 = "no timeout" convention.
    static int earlier_timeout_ms(int first_ms, int second_ms)
    {
        if (first_ms < 0) {
            return second_ms;
        }
        if (second_ms < 0) {
            return first_ms;
        }
        return first_ms < second_ms ? first_ms : second_ms;
    }

private:
    bool              m_never = true;
    Clock::time_point m_at{};
};

} // namespace vnm::process_custody
