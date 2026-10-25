#pragma once

#if defined(__linux__) || defined(__APPLE__)

#include <vnm_process_custody/owner.h>
#include <vnm_terminal/terminal_native_receipt.h>
#ifndef VNM_PROCESS_CUSTODY_HAS_OWNER_START_DEADLINE
#error "Selected process_custody provider must expose send_owner_start_until; update the maintained provider"
#endif
#if defined(__APPLE__) && !defined(VNM_PROCESS_CUSTODY_HAS_DARWIN_PTY_OUTPUT_RETIREMENT)
#error "Selected process_custody provider must retain Darwin PTY output until client EOF; update the provider"
#endif
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace vnm_terminal::internal {

namespace pty_custody = vnm::process_custody;

// This client deliberately uses only the maintained process_custody surface.
// Native access is used for channel flags and the failed-send EOF fallback.
static_assert(std::is_same_v<pty_custody::Native_handle, int>,
    "PTY ownership requires the process_custody POSIX native handle API");
static_assert(requires(pty_custody::Duplex_channel& channel) {
    { channel.valid() } -> std::same_as<bool>;
    { channel.native() } -> std::same_as<pty_custody::Native_handle>;
}, "PTY ownership requires the maintained Duplex_channel native API");
static_assert(requires(pty_custody::Duplex_channel& channel,
    const pty_custody::Owner_start_request& request, std::string* error,
    std::chrono::steady_clock::time_point deadline)
{
    { pty_custody::send_owner_start_until(channel, request, deadline, error) } -> std::same_as<bool>;
}, "PTY ownership requires deadline-aware process_custody START transmission");

constexpr std::chrono::seconds k_owner_startup_timeout(10);
constexpr std::chrono::milliseconds k_owner_startup_receive_slice(100);

int owner_startup_remaining_ms(std::chrono::steady_clock::time_point deadline)
{
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) {
        return 0;
    }

    return static_cast<int>(std::min<std::int64_t>(
        remaining, std::numeric_limits<int>::max()));
}

// Protocol client only. The helper owns every workload wait and tree signal.
// The backend's wait worker is the sole receiver after launch; shutdown joins
// it before draining a failed-start channel and collecting the direct helper.
class Posix_pty_owner
{
public:
    bool launch(const pty_custody::Owner_start_request& request,
        int* master, pid_t* root, std::string* error, Terminal_native_receipt_hook receipt_hook = {})
    {
#if defined(__APPLE__)
        m_receipt_hook = std::move(receipt_hook);
        if (!m_receipt_hook) {
            *error = "Darwin PTY startup requires a retained native receipt handler";
            return false;
        }
#else
        (void)receipt_hook;
#endif
        pty_custody::Duplex_channel child;
        if (!pty_custody::Duplex_channel::create_pair(
                pty_custody::k_owner_message_bytes, &m_channel, &child, error)) {
            return false;
        }
#if defined(__APPLE__)
        std::uint32_t path_size = 0;
        (void)_NSGetExecutablePath(nullptr, &path_size);
        std::vector<char> path_bytes(path_size);
        const auto executable_path = path_size > 0 && _NSGetExecutablePath(path_bytes.data(), &path_size) == 0
            ? QFileInfo(QFile::decodeName(path_bytes.data())).canonicalFilePath() : QString();
#else
        const auto executable_path = QFileInfo(QStringLiteral("/proc/self/exe")).symLinkTarget();
#endif
        if (executable_path.isEmpty()) {
            *error = "cannot locate the hosting executable for the PTY owner";
            return false;
        }
        const auto helper_path = QFile::encodeName(QDir(QFileInfo(executable_path).absolutePath())
            .absoluteFilePath(QStringLiteral("vnm_process_custody_owner"))).toStdString();
        pty_custody::Spawn_request helper;
        helper.executable = helper_path;
        helper.argv = {helper_path, "--control", pty_custody::inherited_argument(0)};
#if defined(__APPLE__)
        helper.argv.emplace_back(pty_custody::k_darwin_pty_owner_argument);
#endif
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
#if defined(__APPLE__)
        m_receipt.helper_pid = static_cast<std::uint32_t>(m_helper.process_id);
        m_receipt.helper_birth_token = m_helper.process_birth_token;
        if (!publish_native_receipt(Terminal_native_receipt_kind::HELPER_PRE_START)) {
            *error = "Darwin PTY helper observation was not retained before workload START";
            close_sending_direction(error);
            (void)publish_native_receipt(Terminal_native_receipt_kind::NO_BIRTH);
            return false;
        }
#endif

#if defined(__linux__)
        auto start_request = request;
        {
            const std::lock_guard lock(m_cpu_mutex);
            const auto nonce = std::to_string(m_helper.process_id) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            if (m_cpu_scope.establish(nonce, nullptr)) {
                m_cpu_scope.configure_spawn(start_request.process);
            }
        }
#else
        const auto& start_request = request;
#endif
        // The provider resumes only an untransferred fragment under this
        // absolute deadline; the client never replays a partial START.
        const auto startup_deadline =
            std::chrono::steady_clock::now() + k_owner_startup_timeout;
        const int channel_flags = ::fcntl(m_channel.native(), F_GETFL, 0);
        if (channel_flags < 0 ||
            ::fcntl(m_channel.native(), F_SETFL, channel_flags | O_NONBLOCK) < 0)
        {
            if (error) {
                *error = "PTY owner startup control channel could not become nonblocking: " +
                    std::to_string(errno);
            }
            close_sending_direction(error);
            return false;
        }

        bool request_sent = false;
        try {
            request_sent = pty_custody::send_owner_start_until(
                m_channel, start_request, startup_deadline, error);
        }
        catch (...) {
            if (error) {
                *error = "PTY owner startup control transmission threw";
            }
        }

        const int restore_result = ::fcntl(m_channel.native(), F_SETFL, channel_flags);
        if (restore_result < 0 && request_sent) {
            request_sent = false;
            if (error) {
                *error = "PTY owner startup control channel could not restore blocking mode: " +
                    std::to_string(errno);
            }
        }
        if (!request_sent) {
            close_sending_direction(error);
            return false;
        }
        if (std::chrono::steady_clock::now() >= startup_deadline) {
            if (error) {
                *error = "PTY owner startup control transmission exceeded its deadline";
            }
            close_sending_direction(error);
            return false;
        }

        int retained_master = -1;
        struct Retained_master
        {
            int& descriptor;
            ~Retained_master() { if (descriptor >= 0) ::close(descriptor); }
        } retained_master_guard{retained_master};
        while (std::chrono::steady_clock::now() < startup_deadline) {
            pty_custody::Owner_event event;
            const int receive_timeout = std::min(
                owner_startup_remaining_ms(startup_deadline),
                static_cast<int>(k_owner_startup_receive_slice.count()));
            const auto status = receive(&event, error, receive_timeout);
            if (status == pty_custody::Receive_status::TIMEOUT) {
                continue;
            }
            if (status != pty_custody::Receive_status::MESSAGE) {
                close_sending_direction(error);
                return false;
            }
            if (event.kind == pty_custody::Owner_event_kind::STARTED && event.pty_master >= 0 &&
                event.root_exit.process_id > 1) {
                retained_master = event.pty_master;
                event.pty_master = -1;
                m_root = static_cast<pid_t>(event.root_exit.process_id);
            }
            if (event.pty_master >= 0) {
                ::close(event.pty_master);
            }
            if (event.kind == pty_custody::Owner_event_kind::START_REJECTED) {
                *error = event.detail;
                return false;
            }
            if (retained_master >= 0
#if defined(__APPLE__)
                && m_root_retained
#endif
            ) {
                *master = std::exchange(retained_master, -1);
                *root = m_root;
                return true;
            }
            if (m_settlement != pty_custody::Tree_settlement::PENDING) {
                *error = "PTY root receipt could not be retained before startup settlement";
                return false;
            }
        }
        *error = "PTY owner startup timed out; native settlement remains pending";
        close_sending_direction(error);
        return false;
    }

    bool stop(int grace_ms, std::string* error) noexcept
    {
        const std::lock_guard lock(m_send_mutex);
        try {
            if (pty_custody::send_owner_stop(m_channel, grace_ms, error)) {
                return true;
            }
        }
        catch (...) {
            try {
                if (error) {
                    *error = "PTY owner stop send threw";
                }
            }
            catch (...) {}
        }
        // A failed send can leave a partial request. Retire the sending
        // direction so the owner settles on EOF while its sole receiver keeps
        // custody.
        close_sending_direction(error);
        return false;
    }

    std::optional<pty_custody::Exit_status> wait_root()
    {
        if (m_root_exit) {
            return m_root_exit;
        }
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
#if defined(__APPLE__)
            // Settlement precedes output retirement. The retained PTY owner
            // may now release its terminal, even if a duplicate client endpoint
            // survives elsewhere in this process or an inherited descriptor.
            (void)close_sending_direction(&error);
#endif
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
#if defined(__linux__)
        {
            const std::lock_guard lock(m_cpu_mutex);
            m_cpu_accounting_admitted = false;
        }
        // Existing family cleanup retains its authority when the helper dies.
        // A successful group retirement cannot manufacture the lost wait
        // owner's native receipt. No further getter accesses the retiring fd.
        while (!m_cpu_scope.terminate_and_release(std::chrono::seconds(1), nullptr)) {
        }
#elif defined(__APPLE__)
        const std::lock_guard lock(m_cpu_mutex);
        m_cpu_tracker.reset();
#endif
    }

    std::optional<std::uint64_t> cpu_time_usec() const
    {
#if defined(__linux__)
        const std::lock_guard lock(m_cpu_mutex);
        if (m_cpu_accounting_admitted) {
            return m_cpu_scope.cpu_time_usec();
        }
#elif defined(__APPLE__)
        const std::lock_guard lock(m_cpu_mutex);
        return m_cpu_tracker.cpu_time_usec();
#endif
        return std::nullopt;
    }

    bool confirmed() const noexcept
    {
        return m_settlement == pty_custody::Tree_settlement::CONFIRMED;
    }

private:
#if defined(__APPLE__)
    bool publish_native_receipt(Terminal_native_receipt_kind kind) noexcept
    {
        m_receipt.kind = kind;
        try {
            return m_receipt_hook && m_receipt_hook(m_receipt) == Terminal_native_receipt_ack::RETAINED;
        }
        catch (...) {
            return false;
        }
    }
#endif

    bool close_sending_direction(std::string* error) noexcept
    {
#if defined(__APPLE__)
        int native_error = 0;
        if (m_channel.shutdown_send(&native_error)) {
            return true;
        }
#else
        if (!m_channel.valid()) {
            return true;
        }
        if (::shutdown(m_channel.native(), SHUT_WR) == 0) {
            return true;
        }
        const int native_error = errno;
#endif
        try {
            if (error) {
                *error += "; control EOF fallback failed: " + std::to_string(native_error);
            }
        }
        catch (...) {}
        return false;
    }

    pty_custody::Receive_status receive(pty_custody::Owner_event* event,
        std::string* error, int timeout_ms = 100) noexcept
    {
        pty_custody::Receive_status status;
        try {
            status = pty_custody::receive_owner_event(m_channel, event, timeout_ms, error);
        }
        catch (...) {
            // In particular, decoder allocation failure cannot unwind through
            // the noexcept native-cleanup worker or prove tree emptiness.
            status = pty_custody::Receive_status::FAILED;
        }
        if (status == pty_custody::Receive_status::MESSAGE) {
#if defined(__linux__)
            if (event->kind == pty_custody::Owner_event_kind::STARTED) {
                const std::lock_guard lock(m_cpu_mutex);
                m_cpu_accounting_admitted = event->cpu_accounting_admitted;
            }
#endif
            if (event->kind == pty_custody::Owner_event_kind::ROOT_EXITED) {
                m_root_exit = event->root_exit;
            }
#if defined(__APPLE__)
            if (event->kind == pty_custody::Owner_event_kind::STARTED) {
                m_receipt.root_pid = static_cast<std::uint32_t>(event->root_exit.process_id);
                m_receipt.root_birth_token = event->root_birth_token;
                m_root_retained = publish_native_receipt(Terminal_native_receipt_kind::ROOT_BORN);
                const std::lock_guard lock(m_cpu_mutex);
                m_cpu_tracker.set_root(event->root_exit.process_id, event->root_birth_token);
            }
            else if (event->kind == pty_custody::Owner_event_kind::ROOT_EXITED) {
                m_receipt.root_exit_code = event->root_exit.exit_code;
                m_receipt.root_signal = event->root_exit.signal;
                m_root_retained = publish_native_receipt(Terminal_native_receipt_kind::ROOT_ENDED) || m_root_retained;
            }
            else if (event->kind == pty_custody::Owner_event_kind::START_REJECTED && m_receipt.root_pid == 0) {
                (void)publish_native_receipt(Terminal_native_receipt_kind::NO_BIRTH);
            }
#endif
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
#if defined(__APPLE__)
            if (event->cleanup_scope != pty_custody::Cleanup_scope::OWNED_PROCESS_GROUPS) {
                m_settlement = pty_custody::Tree_settlement::UNCONFIRMED;
            }
#endif
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
    pid_t m_root = -1;
    std::optional<pty_custody::Exit_status> m_root_exit;
    mutable std::mutex m_cpu_mutex;
#if defined(__linux__)
    pty_custody::Process_cpu_scope m_cpu_scope;
    bool m_cpu_accounting_admitted = false;
#endif
#if defined(__APPLE__)
    mutable pty_custody::Process_cpu_tracker m_cpu_tracker;
    Terminal_native_receipt_hook m_receipt_hook;
    Terminal_native_receipt m_receipt;
    bool m_root_retained = false;
#endif
};

} // namespace vnm_terminal::internal

#endif
