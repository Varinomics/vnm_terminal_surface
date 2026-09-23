#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace vnm_terminal::internal {

// Render and GUI threads share an obligation, never the render node itself.
// Epochs revoke timers on replacement/success; one outstanding ticket prevents
// repeated failed preparations from scheduling an unbounded update stream.
class Bounded_render_retry final
{
public:
    struct ticket_t
    {
        std::uint64_t epoch;
        std::uint64_t serial;
        int           delay_ms; // Negative means report exhaustion without repainting.
    };

    std::optional<ticket_t> failed()
    {
        std::lock_guard lock(m_mutex);
        if (!m_alive || m_pending || m_exhausted) {
            return std::nullopt;
        }
        m_pending = true;
        ++m_serial;
        if (m_attempts == s_delays_ms.size()) {
            m_exhausted = true;
            return ticket_t{m_epoch, m_serial, -1};
        }
        return ticket_t{m_epoch, m_serial, s_delays_ms[m_attempts++]};
    }

    bool current(ticket_t ticket) const
    {
        std::lock_guard lock(m_mutex);
        return matches(ticket);
    }

    bool consume(ticket_t ticket)
    {
        std::lock_guard lock(m_mutex);
        if (!matches(ticket)) {
            return false;
        }
        m_pending = false;
        return true;
    }

    void reset()
    {
        std::lock_guard lock(m_mutex);
        ++m_epoch;
        m_pending   = false;
        m_exhausted = false;
        m_attempts  = 0U;
    }

    void close()
    {
        std::lock_guard lock(m_mutex);
        m_alive   = false;
        m_pending = false;
        ++m_epoch;
    }

private:
    bool matches(ticket_t ticket) const
    {
        return m_alive && m_pending &&
            ticket.epoch == m_epoch && ticket.serial == m_serial;
    }

    static constexpr std::array s_delays_ms{16, 32, 64, 128, 256, 512};

    mutable std::mutex m_mutex;
    std::uint64_t      m_epoch     = 1U;
    std::uint64_t      m_serial    = 0U;
    std::size_t        m_attempts  = 0U;
    bool               m_alive     = true;
    bool               m_pending   = false;
    bool               m_exhausted = false;
};

} // namespace vnm_terminal::internal
