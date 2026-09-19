#pragma once

#if defined(__linux__)

#include <vnm_process_custody/owner.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace vnm_terminal::internal {

namespace pty_custody = vnm::process_custody;

// Protocol client only. The helper owns every workload wait and tree signal.
// The backend's wait worker is the sole receiver after launch; shutdown joins
// it before draining a failed-start channel and collecting the direct helper.
class Posix_pty_owner
{
public:
    bool launch(const pty_custody::Owner_start_request& request,
        int* master, pid_t* root, std::string* error)
    {
        pty_custody::Duplex_channel child;
        if (!pty_custody::Duplex_channel::create_pair(
                pty_custody::k_owner_message_bytes, &m_channel, &child, error)) {
            return false;
        }
        const auto executable_path = QFileInfo(QStringLiteral("/proc/self/exe")).symLinkTarget();
        if (executable_path.isEmpty()) {
            *error = "cannot locate the hosting executable for the PTY owner";
            return false;
        }
        const auto helper_path = QFile::encodeName(QDir(QFileInfo(executable_path).absolutePath())
            .absoluteFilePath(QStringLiteral("vnm_process_custody_owner"))).toStdString();
        pty_custody::Spawn_request helper;
        helper.executable = helper_path;
        helper.argv = {helper_path, "--control", pty_custody::inherited_argument(0)};
        helper.inherited = {{child.native(), 3}};
        // The helper has no terminal stdio of its own. Give it explicit null
        // streams even when a GUI host has closed 0/1/2, so native helper
        // resources cannot be mistaken for inherited standard descriptors.
        QFile null_stream(QStringLiteral("/dev/null"));
        if (!null_stream.open(QIODevice::ReadWrite)) {
            *error = "cannot open null standard streams for the PTY owner";
            return false;
        }
        for (int descriptor = 0; descriptor < 3; ++descriptor) {
            helper.inherited.push_back({null_stream.handle(), descriptor});
        }
        helper.environment = request.process.environment;
        helper.new_session = true;
        m_helper = pty_custody::spawn(helper);
        child.close();
        if (!m_helper.ok) {
            *error = m_helper.error;
            return false;
        }
        m_settlement = pty_custody::Tree_settlement::PENDING;
        if (!pty_custody::send_owner_start(m_channel, request, error)) {
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            pty_custody::Owner_event event;
            const auto status = receive(&event, error);
            if (status == pty_custody::Receive_status::TIMEOUT) {
                continue;
            }
            if (status != pty_custody::Receive_status::MESSAGE) {
                return false;
            }
            if (event.kind == pty_custody::Owner_event_kind::STARTED && event.pty_master >= 0 &&
                event.root_exit.process_id > 1) {
                *master = event.pty_master;
                *root = static_cast<pid_t>(event.root_exit.process_id);
                return true;
            }
            if (event.pty_master >= 0) {
                ::close(event.pty_master);
            }
            if (event.kind == pty_custody::Owner_event_kind::START_REJECTED) {
                *error = event.detail;
                return false;
            }
        }
        *error = "PTY owner startup timed out; native settlement remains pending";
        return false;
    }

    bool stop(int grace_ms, std::string* error) noexcept
    {
        const std::lock_guard lock(m_send_mutex);
        try {
            return pty_custody::send_owner_stop(m_channel, grace_ms, error);
        }
        catch (...) {
            // Closing just the sending direction preserves the sole receiver
            // while asking the surviving owner to settle on control EOF.
            if (m_channel.valid()) {
                (void)::shutdown(m_channel.native(), SHUT_WR);
            }
            return false;
        }
    }

    std::optional<pty_custody::Exit_status> wait_root()
    {
        std::string error;
        while (m_settlement == pty_custody::Tree_settlement::PENDING) {
            pty_custody::Owner_event event;
            if (receive(&event, &error) != pty_custody::Receive_status::MESSAGE) {
                continue;
            }
            if (event.pty_master >= 0) {
                ::close(event.pty_master);
            }
            if (event.kind == pty_custody::Owner_event_kind::ROOT_EXITED) {
                return event.root_exit;
            }
        }
        return {};
    }

    void finish()
    {
        std::string error;
        while (m_settlement == pty_custody::Tree_settlement::PENDING) {
            pty_custody::Owner_event event;
            if (receive(&event, &error) == pty_custody::Receive_status::MESSAGE && event.pty_master >= 0) {
                ::close(event.pty_master);
            }
        }
        // Closing a failed protocol still asks the surviving owner to clean up.
        // It never turns its eventual exit into a tree-empty receipt.
        {
            const std::lock_guard lock(m_send_mutex);
            m_channel.close();
        }
        if (m_helper.process_id > 0 && !m_helper_reaped) {
            pid_t result;
            do {
                result = ::waitpid(static_cast<pid_t>(m_helper.process_id), nullptr, 0);
            } while (result < 0 && errno == EINTR);
            m_helper_reaped = true;
        }
        if (m_helper.process_handle >= 0) {
            ::close(m_helper.process_handle);
            m_helper.process_handle = -1;
        }
    }

    bool confirmed() const noexcept
    {
        return m_settlement == pty_custody::Tree_settlement::CONFIRMED;
    }

private:
    pty_custody::Receive_status receive(pty_custody::Owner_event* event, std::string* error) noexcept
    {
        pty_custody::Receive_status status;
        try {
            status = pty_custody::receive_owner_event(m_channel, event, 100, error);
        }
        catch (...) {
            // In particular, decoder allocation failure cannot unwind through
            // the noexcept native-cleanup worker or prove tree emptiness.
            status = pty_custody::Receive_status::FAILED;
        }
        // STARTED transfers an observation reference, not wait ownership.
        // PTY callbacks use the reported exit status; no numeric reopen or
        // additional wait owner is needed, and every received fd must retire.
        if (event->root_reference >= 0) {
            ::close(event->root_reference);
            event->root_reference = pty_custody::k_invalid_handle;
        }
        if (status == pty_custody::Receive_status::MESSAGE &&
            event->kind == pty_custody::Owner_event_kind::TREE_SETTLEMENT) {
            m_settlement = event->settlement;
        }
        else if (status != pty_custody::Receive_status::MESSAGE &&
                 status != pty_custody::Receive_status::TIMEOUT) {
            m_settlement = pty_custody::Tree_settlement::UNCONFIRMED;
        }
        return status;
    }

    pty_custody::Duplex_channel m_channel;
    pty_custody::Spawn_result m_helper;
    pty_custody::Tree_settlement m_settlement = pty_custody::Tree_settlement::CONFIRMED;
    std::mutex m_send_mutex;
    bool m_helper_reaped = false;
};

} // namespace vnm_terminal::internal

#endif
