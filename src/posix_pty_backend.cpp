#include "vnm_terminal/internal/posix_pty_backend.h"

#if defined(__linux__) || defined(__APPLE__)

#include "native_backend_io_core.h"
#include "native_backend_cleanup_owner.h"
#include "posix_process_group_custody.h"
#include "posix_pty_owner.h"
#ifdef VNM_TERMINAL_CAPTURE_TEST_HOOKS
#include "posix_pty_io_test_hook.h"
#endif
#include <QDir>
#include <QFile>
#include <QProcessEnvironment>
#include <QStringList>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace vnm_terminal::internal {

namespace {

#ifdef VNM_TERMINAL_CAPTURE_TEST_HOOKS
std::atomic<Posix_pty_io_hook_for_testing> s_pty_io_hook{nullptr};

void observe_pty_io_for_testing(Posix_pty_io_point_for_testing point, int descriptor)
{
    if (const auto hook = s_pty_io_hook.load(std::memory_order_acquire)) {
        hook(point, descriptor);
    }
}
#endif

constexpr std::chrono::milliseconds k_exit_output_drain_timeout(250);
// Upper bound on how long an I/O thread blocks in a single poll() before it
// re-checks its stop flags. poll() on a PTY master is unreliable on macOS once
// the child has exited while a descendant still holds the slave open (the
// master is neither readable/writable nor hung up, and the wake-pipe write may
// not interrupt the poll), so the I/O threads must not wait on it indefinitely
// or teardown deadlocks joining them. The reader bounds its master POLLIN poll
// with this; the writer never polls the master at all and instead waits on its
// wake pipe with this bound (see wait_for_write_capacity). The cost is one idle
// wake-up per interval per thread when the terminal is silent.
constexpr std::chrono::milliseconds k_native_master_poll_interval(100);

QString posix_error_message(QStringView context, int code)
{
    return QStringLiteral("%1: %2")
        .arg(context.toString(), QString::fromLocal8Bit(std::strerror(code)));
}

bool size_fits_pty(terminal_grid_size_t grid_size)
{
    return
        is_valid_grid_size(grid_size)                                &&
        grid_size.rows <= std::numeric_limits<unsigned short>::max() &&
        grid_size.columns <= std::numeric_limits<unsigned short>::max();
}

// The winsize pixel fields carry the text-area size. Readers take zero as
// unknown, which is also what a size beyond the field's range reports.
unsigned short winsize_pixel_extent(int cells, int cell_pixels)
{
    const long long extent = static_cast<long long>(cells) * cell_pixels;
    if (extent > std::numeric_limits<unsigned short>::max()) {
        return 0U;
    }

    return static_cast<unsigned short>(extent);
}

winsize winsize_from_grid_size(
    terminal_grid_size_t       grid_size,
    terminal_cell_pixel_size_t cell_pixel_size)
{
    winsize size{};
    size.ws_row    = static_cast<unsigned short>(grid_size.rows);
    size.ws_col    = static_cast<unsigned short>(grid_size.columns);
    size.ws_xpixel = winsize_pixel_extent(grid_size.columns, cell_pixel_size.width);
    size.ws_ypixel = winsize_pixel_extent(grid_size.rows, cell_pixel_size.height);
    return size;
}

class Unique_fd
{
public:
    Unique_fd() = default;

    explicit Unique_fd(int value)
    :
        m_fd(value)
    {}

    ~Unique_fd()
    {
        reset();
    }

    Unique_fd(const Unique_fd&)            = delete;
    Unique_fd& operator=(const Unique_fd&) = delete;

    Unique_fd(Unique_fd&& other) noexcept
    :
        m_fd(std::exchange(other.m_fd, -1))
    {}

    Unique_fd& operator=(Unique_fd&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_fd = std::exchange(other.m_fd, -1);
        }

        return *this;
    }

    int get() const
    {
        return m_fd;
    }

    void reset(int value = -1)
    {
        if (m_fd >= 0) {
            ::close(m_fd);
        }

        m_fd = value;
    }

    explicit operator bool() const
    {
        return m_fd >= 0;
    }

private:
    int m_fd = -1;
};

struct Queued_write
{
    QByteArray                 bytes;
};

struct Native_launch_data
{
    std::vector<QByteArray>    argv_bytes;
    std::vector<QByteArray>    env_bytes;
    QByteArray                 executable;
    QByteArray                 working_directory;
};

bool string_list_has_nul(const QStringList& values)
{
    for (const QString& value : values) {
        if (value.contains(QChar(u'\0'))) {
            return true;
        }
    }

    return false;
}

std::optional<QByteArray> environment_value(
    const QProcessEnvironment& environment,
    const QString&             name)
{
    if (!environment.contains(name)) {
        return std::nullopt;
    }

    return environment.value(name).toLocal8Bit();
}

std::optional<QByteArray> resolved_executable(
    const QByteArray&          executable,
    const QProcessEnvironment& environment,
    const QString&             working_directory)
{
    const auto usable_candidate = [&working_directory](QByteArray candidate)
        -> std::optional<QByteArray> {
        QString candidate_text = QFile::decodeName(candidate);
        if (!QDir::isAbsolutePath(candidate_text)) {
            candidate_text = QDir(working_directory).absoluteFilePath(candidate_text);
        }
        candidate = QFile::encodeName(QDir::cleanPath(candidate_text));
        if (::access(candidate.constData(), X_OK) == 0) {
            return candidate;
        }
        return std::nullopt;
    };

    if (executable.contains('/')) {
        return usable_candidate(executable);
    }

    QByteArray path = environment_value(environment, QStringLiteral("PATH"))
        .value_or(QByteArrayLiteral("/usr/local/bin:/usr/bin:/bin"));
    if (path.isEmpty()) {
        path = QByteArrayLiteral(".");
    }

    int start = 0;
    for (;;) {
        const int separator = path.indexOf(':', start);
        QByteArray directory =
            separator < 0 ? path.mid(start) : path.mid(start, separator - start);
        if (directory.isEmpty()) {
            directory = QByteArrayLiteral(".");
        }

        QByteArray candidate = directory;
        if (!candidate.endsWith('/')) {
            candidate.append('/');
        }
        candidate.append(executable);
        if (std::optional<QByteArray> resolved =
                usable_candidate(std::move(candidate));
            resolved.has_value())
        {
            return resolved;
        }

        if (separator < 0) {
            break;
        }

        start = separator + 1;
    }

    return std::nullopt;
}

std::optional<Native_launch_data> make_native_launch_data(
    const Terminal_effective_launch_config& config)
{
    if (string_list_has_nul(config.argv)) {
        return std::nullopt;
    }

    QStringList env_entries = config.environment.toStringList();
    if (string_list_has_nul(env_entries)) {
        return std::nullopt;
    }

    Native_launch_data data;
    data.argv_bytes.reserve(static_cast<std::size_t>(config.argv.size()));
    for (const QString& argument : config.argv) {
        data.argv_bytes.push_back(QFile::encodeName(argument));
    }

    data.env_bytes.reserve(static_cast<std::size_t>(env_entries.size()));
    for (const QString& entry : env_entries) {
        data.env_bytes.push_back(entry.toLocal8Bit());
    }

    data.working_directory = QFile::encodeName(config.working_directory);
    return data;
}

int set_fd_nonblocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return errno;
    }

    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return errno;
    }

    return 0;
}

int set_fd_cloexec(int fd)
{
    const int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags < 0) {
        return errno;
    }

    if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        return errno;
    }

    return 0;
}

int create_pipe(Unique_fd& read_end, Unique_fd& write_end, bool nonblocking)
{
    int fds[2] = {-1, -1};
    if (::pipe(fds) < 0) {
        return errno;
    }

    read_end.reset(fds[0]);
    write_end.reset(fds[1]);

    int result = set_fd_cloexec(read_end.get());
    if (result != 0) {
        return result;
    }

    result = set_fd_cloexec(write_end.get());
    if (result != 0) {
        return result;
    }

    if (nonblocking) {
        result = set_fd_nonblocking(read_end.get());
        if (result != 0) {
            return result;
        }

        result = set_fd_nonblocking(write_end.get());
        if (result != 0) {
            return result;
        }
    }

    return 0;
}

Terminal_exit_reason exit_reason_from_wait_status(
    int                                    status,
    std::optional<Terminal_exit_reason>    override_reason)
{
    if (override_reason.has_value()) {
        return *override_reason;
    }

    if (WIFSIGNALED(status)) {
        const int signal_number = WTERMSIG(status);
        if (signal_number == SIGINT) {
            return Terminal_exit_reason::INTERRUPTED;
        }

        return Terminal_exit_reason::TERMINATED;
    }

    return Terminal_exit_reason::EXITED;
}

int exit_code_from_wait_status(int status)
{
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return 1;
}

int poll_timeout_until(std::optional<std::chrono::steady_clock::time_point> deadline)
{
    if (!deadline.has_value()) {
        return -1;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= *deadline) {
        return 0;
    }

    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
    return static_cast<int>(std::min<std::int64_t>(
        remaining.count(),
        std::numeric_limits<int>::max()));
}


}

#ifdef VNM_TERMINAL_CAPTURE_TEST_HOOKS
void set_posix_pty_io_hook_for_testing(Posix_pty_io_hook_for_testing hook)
{
    s_pty_io_hook.store(hook, std::memory_order_release);
}
#endif

class Posix_pty_backend::Impl
{
public:
    Impl()
        : m_cleanup(this,
            [](void* context) noexcept {
                auto* impl = static_cast<Impl*>(context);
                wait_for_native_backend_public_calls(impl->call_state());
                impl->shutdown();
            },
            [](void* context) noexcept { delete static_cast<Impl*>(context); },
            [](void* context) noexcept {
                return static_cast<Impl*>(context)->m_owner.confirmed();
            })
    {}

    // This reservation predates native birth. Both failed-start cleanup and
    // facade destruction use it; neither allocates or creates a thread after
    // the child exists. No terminal/native completion is published here.
    void request_deferred_cleanup() noexcept
    {
        revoke_callbacks();
        m_cleanup.request();
    }

    void release_to_cleanup() noexcept
    {
        revoke_callbacks();
        m_cleanup.release();
    }

    ~Impl()
    {
        shutdown();
    }

    Terminal_backend_result start(
        const Terminal_launch_config&  config,
        Terminal_backend_callbacks     callbacks)
    {
        Native_backend_start_gate start_gate{
            m_mutex,
            m_running,
            m_start_attempted,
            m_start_in_progress,
        };
        Native_backend_start_precheck precheck = validate_native_backend_start_preconditions(
            config,
            callbacks,
            start_gate,
            QStringLiteral("POSIX PTY"));
        if (is_backend_rejection(precheck.result)) {
            return precheck.result;
        }

        callbacks = guard_native_backend_callbacks(std::move(callbacks), m_callback_gate);
        Terminal_effective_launch_config effective_config =
            std::move(*precheck.effective_config);

        const auto reject_start = [&] (
            Terminal_backend_error_code code,
            QString                     message,
            bool                        native_dispatch_occurred = false) {
            return reject_native_backend_start_attempt(
                callbacks,
                start_gate,
                code,
                std::move(message),
                native_dispatch_occurred);
        };

        if (!size_fits_pty(effective_config.initial_grid_size)) {
            return
                reject_start(
                    Terminal_backend_error_code::INVALID_INITIAL_GRID_SIZE,
                    QStringLiteral("initial terminal size is outside the PTY range"));
        }

        std::optional<Native_launch_data> native_launch =
            make_native_launch_data(effective_config);
        if (!native_launch.has_value()) {
            return
                reject_start(
                    Terminal_backend_error_code::INVALID_LAUNCH_CONFIG,
                    QStringLiteral("launch argv/environment is invalid"));
        }

        const std::optional<QByteArray> executable = resolved_executable(
            native_launch->argv_bytes.front(),
            effective_config.environment,
            effective_config.working_directory);
        if (!executable.has_value()) {
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    QStringLiteral("executable lookup failed in the final environment"));
        }
        native_launch->executable = *executable;

        Unique_fd read_wake_read;
        Unique_fd read_wake_write;
        int pipe_result = create_pipe(read_wake_read, read_wake_write, true);
        if (pipe_result != 0) {
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    posix_error_message(QStringLiteral("pipe backend read wake"), pipe_result));
        }

        Unique_fd write_wake_read;
        Unique_fd write_wake_write;
        pipe_result = create_pipe(write_wake_read, write_wake_write, true);
        if (pipe_result != 0) {
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    posix_error_message(QStringLiteral("pipe backend write wake"), pipe_result));
        }

        terminal_cell_pixel_size_t cell_pixel_size;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cell_pixel_size = m_cell_pixel_size;
        }
        winsize initial_winsize =
            winsize_from_grid_size(effective_config.initial_grid_size, cell_pixel_size);
        int master_fd = -1;
        pid_t child_pid = -1;
        pty_custody::Owner_start_request owner_request;
        owner_request.mode = pty_custody::Owner_mode::PTY;
        owner_request.process.executable = native_launch->executable.toStdString();
        owner_request.process.working_directory = native_launch->working_directory.toStdString();
        for (const auto& argument : native_launch->argv_bytes) {
            owner_request.process.argv.emplace_back(argument.toStdString());
        }
        for (const auto& variable : native_launch->env_bytes) {
            owner_request.process.environment.emplace_back(variable.toStdString());
        }
        // Preserve the previous exec inheritance contract, but transfer exact
        // descriptors rather than making remote numeric descriptors authority.
#if defined(__linux__)
        for (const auto& name : QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
            bool valid = false;
            const int fd = name.toInt(&valid);
            if (valid && fd > 2) {
                const int flags = ::fcntl(fd, F_GETFD);
                if (flags >= 0 && (flags & FD_CLOEXEC) == 0) {
                    owner_request.process.inherited.push_back({fd, fd});
                }
            }
        }
#else
        const int descriptor_limit = ::getdtablesize();
        for (int fd = 3; fd < descriptor_limit; ++fd) {
            const int flags = ::fcntl(fd, F_GETFD);
            if (flags >= 0 && (flags & FD_CLOEXEC) == 0) {
                owner_request.process.inherited.push_back({fd, fd});
            }
        }
#endif
        owner_request.dimensions.rows = initial_winsize.ws_row;
        owner_request.dimensions.columns = initial_winsize.ws_col;
        owner_request.dimensions.pixel_width = initial_winsize.ws_xpixel;
        owner_request.dimensions.pixel_height = initial_winsize.ws_ypixel;
        std::string owner_error;
        bool owner_started = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_start_attempted = true;
        }
        try {
            owner_started = m_owner.launch(owner_request, &master_fd, &child_pid, &owner_error,
                std::move(effective_config.native_receipt_hook));
        }
        catch (const std::exception& error) {
            owner_error = error.what();
        }
        if (!owner_started) {
            auto result = reject_start(Terminal_backend_error_code::START_FAILED,
                QString::fromStdString(owner_error), true);
            result.start_outcome_determinate = false;
            request_deferred_cleanup();
            return result;
        }
        Unique_fd master(master_fd);
        const int nonblocking_result = set_fd_nonblocking(master.get());
        if (nonblocking_result != 0) {
            auto result = reject_start(Terminal_backend_error_code::START_FAILED,
                posix_error_message(QStringLiteral("fcntl PTY nonblocking"), nonblocking_result), true);
            result.start_outcome_determinate = false;
            request_deferred_cleanup();
            return result;
        }
#if defined(__linux__)
        // Retain the exact slave without reopening a pathname or acquiring a
        // controlling terminal. It lets the reader close output admission at
        // root exit while preserving bytes already accepted by the PTY.
#ifdef VNM_TERMINAL_CAPTURE_TEST_HOOKS
        observe_pty_io_for_testing(Posix_pty_io_point_for_testing::DRAIN_PEER_OPEN, master.get());
#endif
        Unique_fd drain_peer(::ioctl(master.get(), TIOCGPTPEER,
            O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC));
        if (!drain_peer) {
            auto result = reject_start(Terminal_backend_error_code::START_FAILED,
                posix_error_message(QStringLiteral("open PTY drain peer"), errno), true);
            result.start_outcome_determinate = false;
            request_deferred_cleanup();
            return result;
        }
#ifdef VNM_TERMINAL_CAPTURE_TEST_HOOKS
        observe_pty_io_for_testing(Posix_pty_io_point_for_testing::DRAIN_PEER_READY, drain_peer.get());
#endif
#endif

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_callbacks                          = std::move(callbacks);
            m_master                             = std::move(master);
#if defined(__linux__)
            m_drain_peer                         = std::move(drain_peer);
#endif
            m_read_wake_read                     = std::move(read_wake_read);
            m_read_wake_write                    = std::move(read_wake_write);
            m_write_wake_read                    = std::move(write_wake_read);
            m_write_wake_write                   = std::move(write_wake_write);
            m_child_pid                          = child_pid;
            m_running                            = true;
            m_start_attempted                    = true;
            m_start_in_progress                  = false;
            m_stopping                           = false;
            m_process_stopping                   = false;
            m_output_paused                      = false;
            m_exit_reported                      = false;
            m_reader_finished                    = false;
            m_writer_failed                      = false;
            m_startup_aborted                    = false;
            m_child_exit_observed                = false;
            m_child_exit_confirmed               = false;
            m_paused_output_delivery_in_progress = false;
            m_termination_policy                 = effective_config.termination_policy;
            m_paused_output_limits =
                derive_native_backend_output_delivery_limits(
                    effective_config.output_delivery_limits);
            m_exit_reason_override.reset();
            m_queued_write_bytes = 0U;
            m_write_queue.clear();
        }

        Terminal_backend_result worker_result = start_native_backend_workers(
            call_state(),
            m_reader_thread,
            m_writer_thread,
            m_wait_thread,
            [this] {
                read_loop();
            },
            [this] {
                write_loop();
            },
            [this] {
                wait_loop();
            },
            [this](const std::exception& error) {
                const QString message =
                    QStringLiteral("POSIX PTY worker thread startup failed: %1")
                        .arg(QString::fromLocal8Bit(error.what()));
                report_native_backend_error_with_snapshot(
                    m_mutex,
                    m_callbacks,
                    Terminal_backend_error_code::START_FAILED,
                    message);
                request_deferred_cleanup();
                return backend_reject(Terminal_backend_error_code::START_FAILED, message);
            });
        worker_result.native_dispatch_occurred = true;
        if (worker_result.code == Terminal_backend_result_code::REJECTED) {
            // The pre-reserved owner continues native cleanup after this
            // bounded response. Returning from start is not settlement.
            worker_result.start_outcome_determinate = false;
        }
        return worker_result;
    }

    Terminal_backend_result write(QByteArray bytes)
    {
        if (bytes.isEmpty()) {
            return
                backend_reject(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("POSIX PTY write requires bytes"));
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        // The wait thread stops input once it observes the child's exit,
        // before it reports the exit.
        if (m_child_exit_confirmed) {
            return backend_write_after_child_exit(QStringLiteral("POSIX PTY backend is not writable"));
        }
        if (!m_running || m_process_stopping || m_stopping || m_writer_failed || !m_master) {
            return
                backend_reject(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("POSIX PTY backend is not writable"));
        }

        const std::size_t byte_count = static_cast<std::size_t>(bytes.size());
        if (!native_backend_write_queue_can_accept(m_queued_write_bytes, byte_count)) {
            return
                backend_reject(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("POSIX PTY write queue limit reached"));
        }

        add_native_backend_queued_write_bytes(m_queued_write_bytes, byte_count);
        m_write_queue.push_back({std::move(bytes)});
        m_write_cv.notify_one();
        return backend_accept();
    }

    Terminal_backend_result resize(Terminal_backend_resize_request request)
    {
        if (!size_fits_pty(request.grid_size)) {
            return
                backend_reject(
                    Terminal_backend_error_code::RESIZE_FAILED,
                    QStringLiteral("POSIX PTY resize requires a positive unsigned-short grid"));
        }

        int master = -1;
        terminal_cell_pixel_size_t cell_pixel_size;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running || m_process_stopping || m_stopping || !m_master || m_child_pid <= 0) {
                return
                    backend_reject(
                        Terminal_backend_error_code::RESIZE_FAILED,
                        QStringLiteral("POSIX PTY resize requires a running process"));
            }

            master          = m_master.get();
            cell_pixel_size = m_cell_pixel_size;
        }

        winsize size = winsize_from_grid_size(request.grid_size, cell_pixel_size);
        if (::ioctl(master, TIOCSWINSZ, &size) < 0) {
            return
                backend_reject(
                    Terminal_backend_error_code::RESIZE_FAILED,
                    posix_error_message(QStringLiteral("TIOCSWINSZ"), errno));
        }

        return backend_accept();
    }

    Terminal_backend_result set_cell_pixel_size(terminal_cell_pixel_size_t cell_pixel_size)
    {
        int master = -1;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cell_pixel_size = cell_pixel_size;
            // Before start the size waits for the spawn winsize, and a child
            // that is stopping has no terminal left to tell.
            if (!m_running || m_process_stopping || m_stopping || !m_master || m_child_pid <= 0) {
                return backend_accept();
            }

            master = m_master.get();
        }

        // Only the pixel fields change. The grid is read back from the terminal
        // rather than mirrored here, so this cannot restore a stale one.
        winsize size{};
        if (::ioctl(master, TIOCGWINSZ, &size) < 0) {
            return
                backend_reject(
                    Terminal_backend_error_code::RESIZE_FAILED,
                    posix_error_message(QStringLiteral("TIOCGWINSZ"), errno));
        }

        size.ws_xpixel = winsize_pixel_extent(size.ws_col, cell_pixel_size.width);
        size.ws_ypixel = winsize_pixel_extent(size.ws_row, cell_pixel_size.height);
        if (::ioctl(master, TIOCSWINSZ, &size) < 0) {
            return
                backend_reject(
                    Terminal_backend_error_code::RESIZE_FAILED,
                    posix_error_message(QStringLiteral("TIOCSWINSZ"), errno));
        }

        return backend_accept();
    }

    Terminal_backend_result set_output_paused(bool paused)
    {
        QByteArray paused_output;
        bool paused_output_delivery_started = false;
        bool should_wake_reader = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_process_stopping || m_stopping) {
                m_output_paused = false;
                should_wake_reader = true;
            }
            else {
                m_output_paused = paused;
                should_wake_reader = !paused;
            }

            if (!m_output_paused &&
                !m_paused_output_delivery_in_progress &&
                !m_paused_output.isEmpty())
            {
                paused_output_delivery_started =
                    take_paused_output_for_delivery_locked(paused_output);
            }
        }

        if (!paused_output.isEmpty()) {
            deliver_native_backend_output_with_snapshot(
                m_mutex,
                m_callbacks,
                std::move(paused_output));
        }
        finish_paused_output_delivery(paused_output_delivery_started);

        if (should_wake_reader && !paused_output_delivery_started) {
            m_output_cv.notify_all();
            wake_io_threads();
        }

        return backend_accept();
    }

    Terminal_backend_result interrupt()
    {
        int master = -1;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running || m_process_stopping || m_stopping || !m_master || m_child_pid <= 0) {
                return
                    backend_reject(
                        Terminal_backend_error_code::INTERRUPT_FAILED,
                        QStringLiteral("POSIX PTY interrupt requires a running process"));
            }

            master = m_master.get();
        }

        const pid_t foreground_pgid = ::tcgetpgrp(master);
        if (foreground_pgid < 0) {
            return
                backend_reject(
                    Terminal_backend_error_code::INTERRUPT_FAILED,
                    posix_error_message(QStringLiteral("tcgetpgrp"), errno));
        }
        if (foreground_pgid == 0) {
            return
                backend_reject(
                    Terminal_backend_error_code::INTERRUPT_FAILED,
                    QStringLiteral("POSIX PTY has no foreground process group"));
        }

        int interrupt_error = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running || m_process_stopping || m_stopping) {
                return
                    backend_reject(
                        Terminal_backend_error_code::INTERRUPT_FAILED,
                        QStringLiteral("POSIX PTY interrupt requires a running process"));
            }

#if defined(TIOCSIG)
            if (::ioctl(master, TIOCSIG, SIGINT) < 0) {
                interrupt_error = errno;
            }
#else
            const auto target = foreground_pgid == m_child_group_custody->identity()
                ? m_child_group_custody
                : Posix_process_group_custody::capture_foreground(
                    foreground_pgid, m_child_group_custody->identity());
            interrupt_error = target ? target->signal_group(SIGINT) : ENOTSUP;
#endif
        }

        if (interrupt_error != 0) {
            return
                backend_reject(
                    Terminal_backend_error_code::INTERRUPT_FAILED,
                    posix_error_message(QStringLiteral("SIGINT"), interrupt_error));
        }

        return backend_accept();
    }

    Terminal_backend_result terminate()
    {
        QByteArray paused_output;
        bool paused_output_delivery_started = false;
        Terminal_termination_policy policy;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running || m_process_stopping || m_stopping || m_child_pid <= 0) {
                return
                    backend_reject(
                        Terminal_backend_error_code::TERMINATE_FAILED,
                        QStringLiteral("POSIX PTY terminate requires a running process"));
            }

            policy = m_termination_policy;

            m_process_stopping     = true;
            m_output_paused        = false;
            m_exit_reason_override = Terminal_exit_reason::TERMINATED;
            if (!m_paused_output_delivery_in_progress && !m_paused_output.isEmpty()) {
                paused_output_delivery_started =
                    take_paused_output_for_delivery_locked(paused_output);
            }
            m_write_queue.clear();
            m_queued_write_bytes = 0U;
        }

        if (!paused_output.isEmpty()) {
            deliver_native_backend_output_with_snapshot(
                m_mutex,
                m_callbacks,
                std::move(paused_output));
        }
        finish_paused_output_delivery(paused_output_delivery_started);
        m_output_cv.notify_all();
        m_write_cv.notify_all();
        wake_io_threads();
        std::string error;
        if (!m_owner.stop(static_cast<int>(policy.graceful_interval.count()), &error)) {
            return backend_stop_error(QString::fromStdString(error));
        }
        return backend_accept();
    }

    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_shutdown_started) {
                return;
            }
            m_shutdown_started = true;
            m_stopping = true;
            m_process_stopping = true;
            m_output_paused = false;
            m_paused_output_delivery_in_progress = false;
            m_callbacks = {};
        }

        m_output_cv.notify_all();
        m_write_cv.notify_all();
        wake_io_threads();

        std::string owner_error;
        (void)m_owner.stop(0, &owner_error);

        // Workers can still hold raw descriptor snapshots taken before the
        // stop flags. Keep the owning master open until every worker has
        // joined, so an ordinary open cannot reuse their descriptor number.
#ifdef VNM_TERMINAL_CAPTURE_TEST_HOOKS
        observe_pty_io_for_testing(Posix_pty_io_point_for_testing::SHUTDOWN_JOIN, m_master.get());
#endif
        join_native_backend_threads(
            m_reader_thread,
            m_writer_thread,
            m_wait_thread,
            m_termination_thread);
#ifdef VNM_TERMINAL_CAPTURE_TEST_HOOKS
        observe_pty_io_for_testing(Posix_pty_io_point_for_testing::FINISH_BEGIN, m_master.get());
#endif
        m_owner.finish();
#ifdef VNM_TERMINAL_CAPTURE_TEST_HOOKS
        observe_pty_io_for_testing(Posix_pty_io_point_for_testing::FINISH_END, m_master.get());
#endif

        std::lock_guard<std::mutex> lock(m_mutex);
#if defined(__linux__)
        m_drain_peer.reset();
#endif
        m_master.reset();
#ifdef VNM_TERMINAL_CAPTURE_TEST_HOOKS
        observe_pty_io_for_testing(Posix_pty_io_point_for_testing::MASTER_CLOSED, -1);
#endif
        m_read_wake_read.reset();
        m_read_wake_write.reset();
        m_write_wake_read.reset();
        m_write_wake_write.reset();
        m_write_queue.clear();
        m_queued_write_bytes  = 0U;
        m_running             = false;
    }

    native_backend_call_state_t call_state()
    {
        return native_backend_call_state_t{
            m_mutex,
            m_public_call_cv,
            m_public_call_depth,
            m_worker_thread_ids,
            m_startup_aborted,
        };
    }

    void revoke_callbacks() noexcept
    {
        {
            const std::lock_guard lock(m_mutex);
            m_stopping = true;
            m_output_paused = false;
            m_callbacks = {};
        }
        m_output_cv.notify_all();
        m_write_cv.notify_all();
        m_callback_gate->revoke_and_drain();
    }

private:
    native_backend_exit_publication_state_t exit_publication_state()
    {
        return native_backend_exit_publication_state_t{
            m_mutex,
            m_output_cv,
            m_write_cv,
            m_callbacks,
            m_exit_reported,
            m_running,
            m_stopping,
            m_output_paused,
        };
    }

    void report_exit_once(int wait_status)
    {
        std::optional<Terminal_exit_reason> override_reason;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            override_reason = m_exit_reason_override;
        }

        report_exit_once(
            exit_reason_from_wait_status(wait_status, override_reason),
            exit_code_from_wait_status(wait_status));
    }

    void report_exit_once(Terminal_exit_reason reason, int exit_code)
    {
        report_native_backend_exit_once(
            exit_publication_state(),
            reason,
            exit_code,
            [](Terminal_exit_reason resolved_reason) {
                // This backend pre-resolves the reported reason from the wait status
                // before the once-section (see the wait-status overload above), so the
                // shared skeleton takes it as given. The Windows backend resolves its
                // reason inside the once-section instead. The two placements are not
                // interchangeable and neither is shared.
                return resolved_reason;
            },
            [this] {
                // Claims this backend's extra stop flag under the once-lock, after the
                // shared stop flags are set; the returned action wakes the I/O threads
                // after the output and write waiters are woken. Windows has neither
                // and passes the empty claim.
                m_process_stopping = true;
                return [this] {
                    wake_io_threads();
                };
            });
    }

    bool take_paused_output_for_delivery_locked(QByteArray& paused_output)
    {
        return
            take_native_backend_paused_output_for_delivery_locked(
                m_paused_output,
                paused_output,
                m_paused_output_delivery_in_progress,
                m_paused_output_limits.delivery_chunk_bytes);
    }

    void finish_paused_output_delivery(bool delivery_started)
    {
        finish_native_backend_paused_output_delivery(
            m_mutex,
            m_output_cv,
            m_paused_output,
            m_output_paused,
            m_paused_output_delivery_in_progress,
            m_paused_output_limits.delivery_chunk_bytes,
            delivery_started,
            [this] {
                // This backend drains through m_process_stopping || m_stopping and
                // wakes its I/O threads when the drain finishes. The Windows backend
                // drains through m_stopping || m_shutdown_started instead and has no
                // I/O threads to wake. The two stop predicates are not
                // interchangeable and neither is the shared default.
                return m_process_stopping || m_stopping;
            },
            [this](QByteArray bytes) {
                deliver_native_backend_output_with_snapshot(
                    m_mutex,
                    m_callbacks,
                    std::move(bytes));
            },
            [this] {
                wake_io_threads();
            });
    }

    void drain_paused_output_before_exit_report()
    {
        drain_native_backend_paused_output_before_exit_report(
            m_mutex,
            m_output_cv,
            m_paused_output,
            m_output_paused,
            m_paused_output_delivery_in_progress,
            m_paused_output_limits.delivery_chunk_bytes,
            [this] {
                return m_process_stopping || m_stopping;
            },
            [this](QByteArray bytes) {
                deliver_native_backend_output_with_snapshot(
                    m_mutex,
                    m_callbacks,
                    std::move(bytes));
            },
            [this] {
                wake_io_threads();
            });
    }

    void deliver_or_buffer_output(QByteArray bytes)
    {
        const qsizetype bytes_size = bytes.size();
        deliver_or_buffer_native_backend_output(
            m_mutex,
            m_output_cv,
            m_callbacks,
            m_paused_output,
            m_output_paused,
            m_paused_output_delivery_in_progress,
            std::move(bytes),
            [this, bytes_size] {
                return
                    m_paused_output_limits.delivery_chunk_bytes > 0 &&
                    m_paused_output.size() <=
                        m_paused_output_limits.high_watermark_bytes &&
                    bytes_size <=
                        m_paused_output_limits.high_watermark_bytes -
                            m_paused_output.size();
            },
            [this] {
                return m_stopping;
            });
    }

    // Block until the exit-drain window (reap_time + k_exit_output_drain_timeout)
    // elapses, returning early if shutdown begins. Uses m_output_cv, which
    // shutdown notifies after setting m_stopping, so teardown is never delayed.
    void wait_out_exit_drain_window(std::chrono::steady_clock::time_point reap_time)
    {
        const auto deadline = reap_time + k_exit_output_drain_timeout;
        std::unique_lock<std::mutex> lock(m_mutex);
        m_output_cv.wait_until(lock, deadline, [&] {
            return m_stopping;
        });
    }

    void wake_io_threads()
    {
        int read_wake_write = -1;
        int write_wake_write = -1;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            read_wake_write = m_read_wake_write.get();
            write_wake_write = m_write_wake_write.get();
        }

        const unsigned char byte = 1U;
        if (read_wake_write >= 0) {
            const ssize_t result = ::write(read_wake_write, &byte, sizeof(byte));
            (void)result;
        }

        if (write_wake_write >= 0) {
            const ssize_t result = ::write(write_wake_write, &byte, sizeof(byte));
            (void)result;
        }
    }

    void drain_wake_pipe(int wake_read)
    {
        unsigned char buffer[64];
        for (;;) {
            const ssize_t count = ::read(wake_read, buffer, sizeof(buffer));
            if (count > 0) {
                continue;
            }

            if (count < 0 && errno == EINTR) {
                continue;
            }

            break;
        }
    }

    bool write_stopping()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stopping || m_process_stopping;
    }

    void read_loop()
    {
        std::vector<char> buffer(k_native_backend_output_read_chunk_bytes);
        std::optional<std::chrono::steady_clock::time_point> final_drain_deadline;
        bool output_admission_closed = false;
#if defined(__APPLE__)
        std::size_t remaining_output = 0;
#endif
        const auto report_drain_error = [&](QStringView operation) {
            const int error = errno;
            if (!is_native_backend_stopping(m_mutex, m_stopping)) {
                report_native_backend_error_with_snapshot(m_mutex, m_callbacks,
                    Terminal_backend_error_code::READ_FAILED, posix_error_message(operation, error));
            }
        };

        for (;;) {
            int         master        = -1;
            int         wake_read     = -1;
            std::size_t bytes_to_read = buffer.size();
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                // Keep draining the master while output is paused (buffering
                // app-side) until the paused buffer reaches the high-water mark;
                // only then park for backpressure. This stops a child that emits
                // a final line and exits while paused from blocking forever in
                // write() on macOS. The paused high-water mark is derived from
                // the owning session's callback queue hard/high-water limits.
                const auto reader_can_proceed = [&] {
                    const bool direct_delivery_available =
                        !m_output_paused && m_paused_output.isEmpty();
                    const bool paused_buffer_room_available =
                        m_paused_output.size() <
                            m_paused_output_limits.high_watermark_bytes;
                    return
                        m_stopping ||
                        (!m_paused_output_delivery_in_progress &&
                         (direct_delivery_available || paused_buffer_room_available));
                };
                m_output_cv.wait(lock, reader_can_proceed);

                if (m_stopping) {
                    break;
                }

                if (m_child_exit_observed && !final_drain_deadline.has_value()) {
                    final_drain_deadline =
                        std::chrono::steady_clock::now() + k_exit_output_drain_timeout;
                }

                master = m_master.get();
                wake_read = m_read_wake_read.get();
                bytes_to_read =
                    static_cast<std::size_t>(m_paused_output_limits.read_chunk_bytes);
                if ((m_output_paused || !m_paused_output.isEmpty()) &&
                    m_paused_output.size() <
                        m_paused_output_limits.high_watermark_bytes)
                {
                    const qsizetype paused_output_room =
                        m_paused_output_limits.high_watermark_bytes -
                            m_paused_output.size();
                    bytes_to_read = static_cast<std::size_t>(std::min<qsizetype>(
                        static_cast<qsizetype>(bytes_to_read),
                        paused_output_room));
                }
            }

            pollfd fds[2] = {
                { master, POLLIN | POLLHUP | POLLERR, 0 },
                { wake_read, POLLIN, 0 },
            };

            if (!output_admission_closed && final_drain_deadline.has_value() &&
                std::chrono::steady_clock::now() >= *final_drain_deadline)
            {
#if defined(__linux__)
                // FIONREAD excludes accepted bytes still in the flip buffer.
                // Stop new writes, then let read/poll flush that buffer before
                // using their no-input result as the end of the finite drain.
                if (::tcflow(m_drain_peer.get(), TCOOFF) != 0) {
                    report_drain_error(u"freeze PTY output");
                    break;
                }
#else
                // Darwin's output queue includes accepted flow-stopped bytes.
                // Resume them and retain only this FIFO prefix: a detached
                // producer must not extend the drain by continually refilling it.
                int queued = 0;
                if (::ioctl(master, TIOCSTART) != 0 || ::ioctl(master, TIOCOUTQ, &queued) != 0) {
                    report_drain_error(u"snapshot PTY output");
                    break;
                }
                remaining_output = static_cast<std::size_t>(queued);
#endif
                output_admission_closed = true;
#ifdef VNM_TERMINAL_CAPTURE_TEST_HOOKS
                observe_pty_io_for_testing(Posix_pty_io_point_for_testing::OUTPUT_BOUNDARY, master);
#endif
            }
#if defined(__APPLE__)
            if (output_admission_closed) {
                if (remaining_output == 0) {
                    break;
                }
                bytes_to_read = std::min(bytes_to_read, remaining_output);
            }
#endif

            // Bound the wait: when a drain deadline is armed it caps the poll;
            // otherwise fall back to the heartbeat so the reader re-checks its
            // stop flags rather than trusting the wake pipe to interrupt a poll
            // on the PTY master (unreliable on macOS). A heartbeat tick with no
            // deadline just means no master/wake activity yet -- keep waiting.
            const int poll_timeout = output_admission_closed ? 0 : final_drain_deadline.has_value()
                ? poll_timeout_until(final_drain_deadline)
                : static_cast<int>(k_native_master_poll_interval.count());
            const int poll_result = ::poll(fds, 2, poll_timeout);
            if (poll_result == 0) {
                if (!output_admission_closed) {
                    continue;
                }
#if defined(__linux__)
                break;
#endif
            }

            if (poll_result < 0) {
                if (errno == EINTR) {
                    continue;
                }

                if (!is_native_backend_stopping(m_mutex, m_stopping)) {
                    report_native_backend_error_with_snapshot(
                        m_mutex,
                        m_callbacks,
                        Terminal_backend_error_code::READ_FAILED,
                        posix_error_message(QStringLiteral("poll PTY output"), errno));
                }
                break;
            }

            if ((fds[1].revents & POLLIN) != 0) {
                drain_wake_pipe(wake_read);
                if (is_native_backend_stopping(m_mutex, m_stopping)) {
                    break;
                }
            }

            if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
#if defined(__APPLE__)
                if (output_admission_closed) {
                    int queued = 0;
                    if (::ioctl(master, TIOCOUTQ, &queued) != 0) {
                        report_drain_error(u"inspect PTY output");
                        break;
                    }
                    if (queued == 0) {
                        break;
                    }
                    if (::ioctl(master, TIOCSTART) != 0) {
                        report_drain_error(u"resume PTY output");
                        break;
                    }
                    // A concurrent flow stop must not turn this into a busy
                    // loop. Reset still interrupts through the wake descriptor.
                    (void)::poll(fds + 1, 1, static_cast<int>(k_native_master_poll_interval.count()));
                }
#endif
                continue;
            }

#ifdef VNM_TERMINAL_CAPTURE_TEST_HOOKS
            observe_pty_io_for_testing(Posix_pty_io_point_for_testing::READ, master);
#endif
            const ssize_t bytes_read = ::read(master, buffer.data(), bytes_to_read);
            if (bytes_read < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
#if defined(__linux__)
                    if (output_admission_closed) {
                        break;
                    }
#endif
                    continue;
                }

                if (errno != EIO && !is_native_backend_stopping(m_mutex, m_stopping)) {
                    report_native_backend_error_with_snapshot(
                        m_mutex,
                        m_callbacks,
                        Terminal_backend_error_code::READ_FAILED,
                        posix_error_message(QStringLiteral("PTY output read"), errno));
                }
                break;
            }

            if (bytes_read == 0) {
                break;
            }
#if defined(__APPLE__)
            if (output_admission_closed) {
                remaining_output -= static_cast<std::size_t>(bytes_read);
            }
#endif

            deliver_or_buffer_output(QByteArray(
                buffer.data(),
                static_cast<qsizetype>(bytes_read)));
        }

        mark_native_backend_reader_finished(
            m_mutex,
            m_reader_cv,
            m_reader_finished);
    }

    void write_loop()
    {
        for (;;) {
            Queued_write write;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_write_cv.wait(lock, [&] {
                    return m_stopping || m_process_stopping || !m_write_queue.empty();
                });

                if (m_stopping || m_process_stopping) {
                    return;
                }

                write = std::move(m_write_queue.front());
                m_write_queue.pop_front();
                remove_native_backend_queued_write_bytes(
                    m_queued_write_bytes,
                    static_cast<std::size_t>(write.bytes.size()));
            }

            if (!write_all(write.bytes)) {
                mark_writer_failed();
                return;
            }
        }
    }

    void mark_writer_failed()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_stopping && !m_process_stopping) {
            m_writer_failed = true;
        }
    }

    // Wait, bounded by a heartbeat, before retrying a write that returned
    // EAGAIN. This deliberately polls ONLY the wake pipe (an ordinary pipe) and
    // never the PTY master: poll() on a PTY master is unreliable on macOS -- a
    // POLLOUT poll there could be wedged neither by the wake-pipe write nor by a
    // finite timeout once the child has exited while a descendant still holds
    // the slave open, deadlocking the writer thread and teardown. The retried
    // non-blocking write() is itself the authoritative writability test, so we
    // only need a bounded, reliably-interruptible wait here. Returns false when
    // the writer should stop.
    bool wait_for_write_capacity(int wake_read)
    {
        pollfd fds[1] = {
            { wake_read, POLLIN, 0 },
        };

        const int poll_result = ::poll(
            fds,
            1,
            static_cast<int>(k_native_master_poll_interval.count()));
        if (poll_result < 0) {
            if (errno == EINTR) {
                return !write_stopping();
            }

            if (!write_stopping()) {
                report_native_backend_error_with_snapshot(
                    m_mutex,
                    m_callbacks,
                    Terminal_backend_error_code::WRITE_FAILED,
                    posix_error_message(QStringLiteral("poll PTY write wake"), errno));
            }
            return false;
        }

        if ((fds[0].revents & POLLIN) != 0) {
            drain_wake_pipe(wake_read);
        }

        return !write_stopping();
    }

    bool write_all(const QByteArray& bytes)
    {
        qsizetype offset = 0;
        while (offset < bytes.size()) {
            int master = -1;
            int wake_read = -1;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_stopping || m_process_stopping || !m_master) {
                    return false;
                }

                master = m_master.get();
                wake_read = m_write_wake_read.get();
            }

            // Write first: the PTY master is non-blocking, so write() is itself
            // the writability test. Only wait when it reports EAGAIN, and wait
            // on the wake pipe rather than polling the master for POLLOUT (see
            // wait_for_write_capacity). This keeps the writer off the unreliable
            // macOS PTY-master POLLOUT path entirely.
            const qsizetype remaining = bytes.size() - offset;
#ifdef VNM_TERMINAL_CAPTURE_TEST_HOOKS
            observe_pty_io_for_testing(Posix_pty_io_point_for_testing::WRITE, master);
#endif
            const ssize_t count = ::write(
                master,
                bytes.constData() + offset,
                static_cast<std::size_t>(remaining));
            if (count > 0) {
                offset += static_cast<qsizetype>(count);
                continue;
            }

            if (count < 0 && errno == EINTR) {
                continue;
            }

            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!wait_for_write_capacity(wake_read)) {
                    return false;
                }
                continue;
            }

            if (count < 0) {
                if (errno != EIO && !write_stopping()) {
                    report_native_backend_error_with_snapshot(
                        m_mutex,
                        m_callbacks,
                        Terminal_backend_error_code::WRITE_FAILED,
                        posix_error_message(QStringLiteral("PTY input write"), errno));
                }
                return false;
            }

            // count == 0: the write made no progress.
            report_native_backend_error_with_snapshot(
                m_mutex,
                m_callbacks,
                Terminal_backend_error_code::WRITE_FAILED,
                QStringLiteral("PTY input write made no progress"));
            return false;
        }

        return true;
    }

    void wait_loop()
    {
        pid_t child_pid = -1;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            child_pid = m_child_pid;
        }

        if (child_pid <= 0) {
            return;
        }

        const auto root_exit = m_owner.wait_root();
        const int wait_error = root_exit ? 0 : ECHILD;
        const auto reap_time = std::chrono::steady_clock::now();
        if (wait_error != 0) {
            QByteArray paused_output;
            bool paused_output_delivery_started = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                // Wait ownership loss is not proof of native settlement.
                m_child_exit_observed = true;
                m_process_stopping = true;
                m_output_paused = false;
                m_write_queue.clear();
                m_queued_write_bytes = 0U;
                if (!m_paused_output_delivery_in_progress && !m_paused_output.isEmpty()) {
                    paused_output_delivery_started =
                        take_paused_output_for_delivery_locked(paused_output);
                }
            }
            if (!paused_output.isEmpty()) {
                deliver_native_backend_output_with_snapshot(
                    m_mutex,
                    m_callbacks,
                    std::move(paused_output));
            }
            finish_paused_output_delivery(paused_output_delivery_started);
            m_output_cv.notify_all();
            m_write_cv.notify_all();
            wake_io_threads();
            wait_for_native_backend_reader_finished(
                m_mutex,
                m_reader_cv,
                m_reader_finished);
            drain_paused_output_before_exit_report();
            report_native_backend_error_with_snapshot(
                m_mutex,
                m_callbacks,
                Terminal_backend_error_code::TERMINATE_FAILED,
                posix_error_message(QStringLiteral("waitpid"), wait_error));
            return; // No fabricated exit or successful-cleanup notification.
        }

        QByteArray paused_output;
        bool paused_output_delivery_started = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_child_exit_observed = true;
            m_child_exit_confirmed = true;
            m_process_stopping = true;
            m_output_paused    = false;
            m_write_queue.clear();
            m_queued_write_bytes = 0U;
            if (!m_paused_output_delivery_in_progress && !m_paused_output.isEmpty()) {
                paused_output_delivery_started =
                    take_paused_output_for_delivery_locked(paused_output);
            }
        }
#ifdef VNM_TERMINAL_CAPTURE_TEST_HOOKS
        observe_pty_io_for_testing(Posix_pty_io_point_for_testing::ROOT_EXIT, m_master.get());
#endif
        if (!paused_output.isEmpty()) {
            deliver_native_backend_output_with_snapshot(
                m_mutex,
                m_callbacks,
                std::move(paused_output));
        }
        finish_paused_output_delivery(paused_output_delivery_started);
        m_output_cv.notify_all();
        m_write_cv.notify_all();
        wake_io_threads();
        wait_for_native_backend_reader_finished(
            m_mutex,
            m_reader_cv,
            m_reader_finished);
        // Honor the exit-drain window even if PTY EOF ends the reader early.
        // Shutdown can interrupt this output drain; native cleanup belongs to
        // the helper independently of callback delivery.
        wait_out_exit_drain_window(reap_time);
        drain_paused_output_before_exit_report();
        // Root exit is not tree emptiness. Only the dedicated owner consumes
        // workload waits; its explicit receipt controls native settlement.
        const int status = root_exit->signal != 0
            ? root_exit->signal : (root_exit->exit_code << 8);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_child_pid = -1;
        }
        report_exit_once(status);
#ifdef VNM_TERMINAL_CAPTURE_TEST_HOOKS
        observe_pty_io_for_testing(Posix_pty_io_point_for_testing::FINISH_BEGIN, m_master.get());
#endif
        m_owner.finish();
#ifdef VNM_TERMINAL_CAPTURE_TEST_HOOKS
        observe_pty_io_for_testing(Posix_pty_io_point_for_testing::FINISH_END, m_master.get());
#endif
        if (!m_owner.confirmed()) {
            report_native_backend_error_with_snapshot(m_mutex, m_callbacks,
                Terminal_backend_error_code::TERMINATE_FAILED,
                QStringLiteral("PTY cleanup remains unconfirmed"));
        }
    }

    // Allocated before native birth; all queued signals retain this identity.
    Posix_pty_owner m_owner;
#if !defined(TIOCSIG)
    std::shared_ptr<Posix_process_group_custody> m_child_group_custody =
        std::make_shared<Posix_process_group_custody>(-1);
#endif
    bool m_child_exit_observed = false;
    // Set only once the wait has observed the child's exit, unlike
    // m_child_exit_observed, which a lost observation sets as well.
    bool m_child_exit_confirmed = false;
    std::mutex                          m_mutex;
    std::condition_variable             m_output_cv;
    std::condition_variable             m_write_cv;
    std::condition_variable             m_reader_cv;
    std::condition_variable             m_public_call_cv;
    Terminal_backend_callbacks          m_callbacks;
    std::shared_ptr<Native_backend_callback_gate> m_callback_gate =
        std::make_shared<Native_backend_callback_gate>();
    Unique_fd                           m_master;
#if defined(__linux__)
    Unique_fd                           m_drain_peer;
#endif
    Unique_fd                           m_read_wake_read;
    Unique_fd                           m_read_wake_write;
    Unique_fd                           m_write_wake_read;
    Unique_fd                           m_write_wake_write;
    Native_backend_cleanup_reservation m_cleanup;
    std::thread                         m_reader_thread;
    std::thread                         m_writer_thread;
    std::thread                         m_wait_thread;
    std::thread                         m_termination_thread;
    std::set<std::thread::id>           m_worker_thread_ids;
    QByteArray                          m_paused_output;
    std::deque<Queued_write>            m_write_queue;
    std::optional<Terminal_exit_reason> m_exit_reason_override;
    Terminal_termination_policy         m_termination_policy;
    native_backend_output_delivery_limits_t
                                        m_paused_output_limits =
                                            derive_native_backend_output_delivery_limits(
                                                std::nullopt);
    std::size_t                         m_queued_write_bytes = 0U;
    std::size_t                         m_public_call_depth = 0U;
    terminal_cell_pixel_size_t          m_cell_pixel_size;
    pid_t                               m_child_pid = -1;
    bool                                m_running = false;
    bool                                m_start_attempted = false;
    bool                                m_start_in_progress = false;
    bool                                m_startup_aborted = false;
    bool                                m_process_stopping = false;
    bool                                m_stopping = false;
    bool                                m_output_paused = false;
    bool                                m_paused_output_delivery_in_progress = false;
    bool                                m_exit_reported = false;
    bool                                m_shutdown_started = false;
    bool                                m_reader_finished = false;
    bool                                m_writer_failed = false;
};

Posix_pty_backend::Posix_pty_backend()
:
    m_impl(std::make_unique<Impl>())
{}

Posix_pty_backend::~Posix_pty_backend()
{
    if (m_impl) {
        Impl* impl = m_impl.release();
        impl->release_to_cleanup();
    }
}

Terminal_backend_result Posix_pty_backend::start(
    const Terminal_launch_config&  config,
    Terminal_backend_callbacks     callbacks)
{
    Impl* impl = m_impl.get();
    auto guard = Native_backend_public_call_guard(
        impl->call_state());
    return impl->start(config, std::move(callbacks));
}

Terminal_backend_result Posix_pty_backend::write(QByteArray bytes)
{
    Impl* impl = m_impl.get();
    auto guard = Native_backend_public_call_guard(
        impl->call_state());
    return impl->write(std::move(bytes));
}

Terminal_backend_result Posix_pty_backend::resize(
    Terminal_backend_resize_request request)
{
    Impl* impl = m_impl.get();
    auto guard = Native_backend_public_call_guard(
        impl->call_state());
    return impl->resize(request);
}

Terminal_backend_result Posix_pty_backend::set_output_paused(bool paused)
{
    Impl* impl = m_impl.get();
    auto guard = Native_backend_public_call_guard(
        impl->call_state());
    return impl->set_output_paused(paused);
}

Terminal_backend_result Posix_pty_backend::interrupt()
{
    Impl* impl = m_impl.get();
    auto guard = Native_backend_public_call_guard(
        impl->call_state());
    return impl->interrupt();
}

Terminal_backend_result Posix_pty_backend::terminate()
{
    Impl* impl = m_impl.get();
    auto guard = Native_backend_public_call_guard(
        impl->call_state());
    return impl->terminate();
}

Terminal_backend_result Posix_pty_backend::set_cell_pixel_size(
    terminal_cell_pixel_size_t size)
{
    Impl* impl = m_impl.get();
    auto guard = Native_backend_public_call_guard(
        impl->call_state());
    return impl->set_cell_pixel_size(size);
}

std::unique_ptr<Terminal_backend> make_posix_pty_backend()
{
    return std::make_unique<Posix_pty_backend>();
}

}

#endif
