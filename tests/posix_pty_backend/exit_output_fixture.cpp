#include "vnm_terminal/internal/posix_pty_backend.h"
#include "exit_output_fixture.h"
#include "posix_pty_io_test_hook.h"
#include <QFileInfo>
#include <QStringList>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <atomic>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

using Clock = std::chrono::steady_clock;

namespace {

const auto started = Clock::now();
std::mutex log_mutex;
std::atomic_size_t delivered = 0;
std::atomic_size_t root_bytes = 0;
std::atomic_int observed_master = -1;
std::mutex cleanup_mutex;
std::condition_variable cleanup_changed;
bool cleanup_closed = false;
std::atomic_bool boundary_seen = false;
std::atomic_bool inject_peer_failure = false;
std::atomic_bool peer_valid = false;
std::atomic_int peer_descriptor = -1;
std::atomic_llong boundary_time = -1;
std::atomic_int failed_root_observer = -1;
char cleanup_tail[512];

void write_cleanup_tail(int)
{
    std::size_t offset = 0;
    while (offset != sizeof(cleanup_tail)) {
        const auto count = ::write(STDOUT_FILENO, cleanup_tail + offset, sizeof(cleanup_tail) - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) std::_Exit(20);
        offset += static_cast<std::size_t>(count);
    }
    std::_Exit(0);
}

bool check(bool condition, const char* message)
{
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

std::string finite_output(std::string_view mode)
{
    std::string bytes(mode == "small" ? 8192 : 16000, 'R');
    if (mode != "detached_tail") {
        for (std::size_t index = 0; index != bytes.size(); ++index) {
            bytes[index] = static_cast<char>('A' + index % 26);
        }
    }
    return bytes;
}

long long elapsed_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
}

void event(const char* name, long long first = 0, long long second = 0, long long third = 0)
{
    const std::lock_guard lock(log_mutex);
    std::fprintf(stderr, "PROBE t_ms=%lld event=%s a=%lld b=%lld c=%lld delivered=%zu root_bytes=%zu\n",
        elapsed_ms(), name, first, second, third, delivered.load(), root_bytes.load());
    std::fflush(stderr);
}

void sample(const char* name, int descriptor)
{
    int output_count = -1;
    int input_count = -1;
    errno = 0;
    const int output_result = ::ioctl(descriptor, TIOCOUTQ, &output_count);
    const int output_error = errno;
    errno = 0;
    const int input_result = ::ioctl(descriptor, FIONREAD, &input_count);
    const int input_error = errno;
    event(name, output_result == 0 ? output_count : -output_error,
        input_result == 0 ? input_count : -input_error, descriptor);
}

void io_hook(vnm_terminal::internal::Posix_pty_io_point_for_testing point, int descriptor)
{
#if defined(__linux__)
    if (point == vnm_terminal::internal::Posix_pty_io_point_for_testing::DRAIN_PEER_OPEN && inject_peer_failure) {
        const pid_t root = ::tcgetpgrp(descriptor);
        if (root > 0) failed_root_observer = static_cast<int>(::syscall(SYS_pidfd_open, root, 0));
        // Force the real ioctl to reject its descriptor without racing close
        // against another worker or leaving an unrelated recycled fd to close.
        const int replacement = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        if (replacement >= 0) {
            (void)::dup2(replacement, descriptor);
            ::close(replacement);
        }
    }
    if (point == vnm_terminal::internal::Posix_pty_io_point_for_testing::DRAIN_PEER_READY) {
        peer_descriptor = descriptor;
        peer_valid = (::fcntl(descriptor, F_GETFD) & FD_CLOEXEC) != 0 &&
            (::fcntl(descriptor, F_GETFL) & O_NONBLOCK) != 0 && ::isatty(descriptor);
        return;
    }
#endif
    if (point == vnm_terminal::internal::Posix_pty_io_point_for_testing::OUTPUT_BOUNDARY) {
        boundary_time = elapsed_ms();
        boundary_seen = true;
        cleanup_changed.notify_all();
        sample("output_boundary", descriptor);
    }
    if (point == vnm_terminal::internal::Posix_pty_io_point_for_testing::MASTER_CLOSED) {
        event("master_closed");
        const std::lock_guard lock(cleanup_mutex);
        cleanup_closed = true;
        cleanup_changed.notify_all();
        return;
    }
    observed_master = descriptor;
    if (point == vnm_terminal::internal::Posix_pty_io_point_for_testing::ROOT_EXIT) {
        sample("root_exit_observed_queue", descriptor);
    }
    if (point == vnm_terminal::internal::Posix_pty_io_point_for_testing::FINISH_BEGIN) {
        sample("owner_finish_begin_queue", descriptor);
    }
    if (point == vnm_terminal::internal::Posix_pty_io_point_for_testing::FINISH_END) {
        sample("owner_finish_end_queue", descriptor);
    }
    if (point == vnm_terminal::internal::Posix_pty_io_point_for_testing::SHUTDOWN_JOIN) {
        sample("shutdown_join_queue", descriptor);
    }
}

class Watchdog
{
public:
    explicit Watchdog(std::chrono::seconds timeout) : m_thread([this, timeout] {
        std::unique_lock lock(m_mutex);
        if (!m_changed.wait_for(lock, timeout, [this] { return m_done; })) {
            event("watchdog_expired");
            std::_Exit(124);
        }
    }) {}

    ~Watchdog()
    {
        {
            const std::lock_guard lock(m_mutex);
            m_done = true;
        }
        m_changed.notify_all();
        m_thread.join();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_done = false;
    std::thread m_thread;
};

class Socket_path
{
public:
    Socket_path()
    {
        char pattern[] = "/tmp/vnm-pty-output-XXXXXX";
        if (const auto* directory = ::mkdtemp(pattern)) {
            m_directory = directory;
            m_path = m_directory + "/control";
        }
    }

    ~Socket_path()
    {
        if (!m_path.empty()) (void)::unlink(m_path.c_str());
        if (!m_directory.empty()) (void)::rmdir(m_directory.c_str());
    }

    const std::string& path() const { return m_path; }

private:
    std::string m_directory;
    std::string m_path;
};

bool transfer(int descriptor, void* bytes, std::size_t size, bool sending)
{
    auto* data = static_cast<char*>(bytes);
    const auto deadline = Clock::now() + std::chrono::seconds(4);
    while (size != 0 && Clock::now() < deadline) {
        pollfd ready{descriptor, static_cast<short>(sending ? POLLOUT : POLLIN), 0};
        if (::poll(&ready, 1, 100) <= 0) {
            continue;
        }
        const ssize_t count = sending ? ::write(descriptor, data, size) : ::read(descriptor, data, size);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        data += count;
        size -= static_cast<std::size_t>(count);
    }
    return size == 0;
}

struct ready_t
{
    pid_t pid;
    char slave[128];
};

struct done_t
{
    std::uint64_t accepted;
    int last_error;
};

done_t write_until_released(int descriptor, int release, std::atomic_bool* running = nullptr,
    std::atomic_size_t* progress = nullptr)
{
    const int flags = ::fcntl(descriptor, F_GETFL);
    (void)::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
    const std::string bytes(16384, 'd');
    const auto deadline = Clock::now() + std::chrono::seconds(8);
    done_t result{};
    while (Clock::now() < deadline && (!running || running->load())) {
        pollfd control{release, POLLIN | POLLHUP, 0};
        if (release >= 0 && ::poll(&control, 1, 0) > 0) {
            break;
        }
        const auto count = ::write(descriptor, bytes.data(), bytes.size());
        if (count > 0) {
            result.accepted += static_cast<std::uint64_t>(count);
            if (progress) *progress += static_cast<std::size_t>(count);
            continue;
        }
        result.last_error = errno;
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            if (running) event("producer_write_error", errno, result.accepted);
            break;
        }
        pollfd writable{descriptor, POLLOUT, 0};
        (void)::poll(&writable, 1, 1);
    }
    return result;
}

int fixture(const char* socket_path, std::string_view mode)
{
    termios settings{};
    if (::tcgetattr(STDIN_FILENO, &settings) != 0) return 16;
    settings.c_lflag &= ~ECHO;
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &settings) != 0) return 17;
    const int connection = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socket_path, sizeof(address.sun_path) - 1);
    if (connection < 0 || ::connect(connection, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        return 2;
    }
    pid_t cleanup_child = -1;
    if (mode == "cleanup_tail" || mode == "graceful_cleanup_tail") {
        if (mode == "graceful_cleanup_tail") {
            // Keep the session leader alive until the controller observes the
            // cleanup output. Darwin revokes its TTY when this leader exits.
            (void)::signal(SIGTERM, SIG_IGN);
        }
        int handshake[2];
        if (::pipe(handshake) != 0) return 18;
        const auto child = ::fork();
        if (child < 0) return 19;
        if (child == 0) {
            ::close(connection);
            ::close(handshake[0]);
            std::fill(std::begin(cleanup_tail), std::end(cleanup_tail), 'C');
            (void)::signal(SIGHUP, SIG_IGN);
            (void)::signal(SIGTERM, write_cleanup_tail);
            if (::write(handshake[1], "G", 1) != 1) std::_Exit(21);
            ::close(handshake[1]);
            for (;;) ::pause();
        }
        cleanup_child = child;
        ::close(handshake[1]);
        char ready;
        const bool armed = transfer(handshake[0], &ready, 1, false);
        ::close(handshake[0]);
        if (!armed) return 22;
    }
    if (mode == "detached" || mode == "detached_tail") {
        int handshake[2];
        if (::pipe(handshake) != 0) return 3;
        const auto child = ::fork();
        if (child < 0) return 4;
        if (child == 0) {
            ::close(handshake[0]);
            if (::setsid() < 0) std::_Exit(5);
            ready_t ready{};
            ready.pid = ::getpid();
            std::strncpy(ready.slave, ::ttyname(STDOUT_FILENO), sizeof(ready.slave) - 1);
            if (!transfer(connection, &ready, sizeof(ready), true)) std::_Exit(6);
            char go = 0;
            if (!transfer(connection, &go, 1, false)) std::_Exit(7);
            if (::write(handshake[1], &go, 1) != 1) std::_Exit(8);
            ::close(handshake[1]);
            auto result = write_until_released(STDOUT_FILENO, connection);
            (void)transfer(connection, &result, sizeof(result), true);
            ::close(connection);
            std::_Exit(0);
        }
        ::close(handshake[1]);
        ::close(connection);
        char ready;
        const bool detached = transfer(handshake[0], &ready, 1, false);
        ::close(handshake[0]);
        if (!detached) return 9;
    }
    else {
        ready_t ready{};
        ready.pid = ::getpid();
        std::strncpy(ready.slave, ::ttyname(STDOUT_FILENO), sizeof(ready.slave) - 1);
        if (!transfer(connection, &ready, sizeof(ready), true)) return 10;
        if (mode != "flow" && mode != "graceful_cleanup_tail") ::close(connection);
    }
    char go = 0;
    // A detached writer changes O_NONBLOCK on the shared slave description.
    // Wait for input explicitly instead of mistaking EAGAIN for a root failure.
    if (!transfer(STDIN_FILENO, &go, 1, false)) return 11;
    if (mode == "graceful_cleanup_tail") {
        char admitted = 'G';
        if (!transfer(connection, &admitted, 1, true)) return 24;
        int status = 0;
        pid_t waited;
        do {
            waited = ::waitpid(cleanup_child, &status, 0);
        } while (waited < 0 && errno == EINTR);
        char release = 0;
        const bool delivered = transfer(connection, &release, 1, false) && release == 'D';
        ::close(connection);
        return waited == cleanup_child && WIFEXITED(status) && WEXITSTATUS(status) == 0 && delivered ? 0 : 23;
    }
    if (mode == "flow") {
        if (::tcflow(STDOUT_FILENO, TCOOFF) != 0) return 13;
        const int flags = ::fcntl(STDOUT_FILENO, F_GETFL);
        if (::fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK) != 0) return 14;
        const std::string bytes(512, 'R');
        errno = 0;
        const auto count = ::write(STDOUT_FILENO, bytes.data(), bytes.size());
        done_t result{count > 0 ? static_cast<std::uint64_t>(count) : 0, errno};
        const bool sent = transfer(connection, &result, sizeof(result), true);
        ::close(connection);
        return sent ? 0 : 15;
    }
    if (mode == "finite" || mode == "small" || mode == "detached_tail" || mode == "reset" || mode == "bounded_reset") {
        const auto tail = finite_output(mode);
        std::size_t offset = 0;
        while (offset != tail.size()) {
            const auto count = ::write(STDOUT_FILENO, tail.data() + offset, tail.size() - offset);
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (count <= 0) return 12;
            offset += static_cast<std::size_t>(count);
        }
    }
    return 0;
}

int backend_probe(const char* executable, std::string_view mode,
    const vnm_terminal::test_helpers::Exit_output_hooks& hooks)
{
#if defined(__linux__)
    const int original_terminal = ::open("/dev/tty", O_RDONLY | O_NOCTTY | O_CLOEXEC);
    struct stat original_identity{};
    if (original_terminal >= 0) (void)::fstat(original_terminal, &original_identity);
#endif
    Socket_path socket_path;
    const auto& path = socket_path.path();
    if (path.empty()) return 2;
    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    if (listener < 0 || ::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 1) != 0)
    {
        return 2;
    }
    std::mutex mutex;
    std::condition_variable changed;
    bool exited = false;
    bool root_succeeded = false;
    bool root_terminated_cleanly = false;
    bool reset_done = false;
    bool bounded_cleanup = false;
    int exit_count = 0;
    int error_count = 0;
    std::string output;
    std::size_t output_at_exit = 0;
    bool output_after_exit = false;
    std::size_t largest_chunk = 0;
    std::atomic_bool writing = true;
    std::atomic_bool writer_ended = false;
    std::atomic_size_t external_accepted = 0;
    bool producer_alive_at_exit = false;
    auto backend = hooks.make_backend();
    vnm_terminal::internal::Terminal_backend_callbacks callbacks;
    callbacks.output_received = [&](QByteArray bytes) {
        {
            const std::lock_guard lock(mutex);
            output_after_exit |= exited;
            largest_chunk = std::max(largest_chunk, static_cast<std::size_t>(bytes.size()));
            output.append(bytes.constData(), bytes.size());
        }
        changed.notify_all();
        delivered += static_cast<std::size_t>(bytes.size());
        for (const char byte : bytes) root_bytes += byte == 'R';
        std::this_thread::sleep_for(std::chrono::milliseconds(
            mode == "finite" || mode == "reset" || mode == "bounded_reset" ? 400 : mode == "small" ? 30 : 2));
    };
    callbacks.error_reported = [&](const auto& error) {
        {
            const std::lock_guard lock(mutex);
            ++error_count;
        }
        const std::lock_guard lock(log_mutex);
        std::fprintf(stderr, "PROBE t_ms=%lld backend_error=%s\n", elapsed_ms(), error.message.toLocal8Bit().constData());
    };
    callbacks.process_exited = [&](const auto& status) {
        event("exit_callback", static_cast<int>(status.reason), status.exit_code);
        const std::lock_guard lock(mutex);
        ++exit_count;
        producer_alive_at_exit = !writer_ended;
        output_at_exit = output.size();
        exited = true;
        root_succeeded = status.reason == vnm_terminal::internal::Terminal_exit_reason::EXITED && status.exit_code == 0;
        root_terminated_cleanly = status.reason == vnm_terminal::internal::Terminal_exit_reason::TERMINATED &&
            status.exit_code == 0;
        changed.notify_all();
    };
    vnm_terminal::internal::Terminal_launch_config config;
    config.argv = {QFileInfo(QString::fromLocal8Bit(executable)).absoluteFilePath(),
        QStringLiteral("--fixture"), QString::fromStdString(path), QString::fromUtf8(mode.data(), mode.size())};
    config.initial_grid_size = vnm_terminal::internal::terminal_grid_size_t{24, 80};
    if (mode == "graceful_cleanup_tail") {
        config.termination_policy.graceful_interval = std::chrono::seconds(3);
        config.termination_policy.kill_interval = std::chrono::milliseconds(500);
    }
    if (mode == "small") config.output_delivery_limits = vnm_terminal::internal::Terminal_backend_output_delivery_limits{64, 128};
    vnm_terminal::internal::set_posix_pty_io_hook_for_testing(io_hook);
    if (backend->start(config, callbacks).code != vnm_terminal::internal::Terminal_backend_result_code::ACCEPTED) return 3;
#if defined(__linux__)
    const int current_terminal = ::open("/dev/tty", O_RDONLY | O_NOCTTY | O_CLOEXEC);
    struct stat current_identity{};
    if (current_terminal >= 0) (void)::fstat(current_terminal, &current_identity);
    const bool same_terminal = (current_terminal < 0 && original_terminal < 0) ||
        (current_terminal >= 0 && original_terminal >= 0 &&
         current_identity.st_rdev == original_identity.st_rdev &&
         current_identity.st_ino == original_identity.st_ino);
    if (current_terminal >= 0) ::close(current_terminal);
    if (original_terminal >= 0) ::close(original_terminal);
    if (!check(same_terminal, "peer open leaves the host controlling terminal unchanged")) return 15;
    const auto peer_number = std::to_string(peer_descriptor.load());
    const auto verifier = ::fork();
    if (verifier == 0) {
        ::execl(executable, executable, "--peer-not-inherited", peer_number.c_str(), nullptr);
        std::_Exit(125);
    }
    int verifier_status = 0;
    if (!check(verifier > 0 && ::waitpid(verifier, &verifier_status, 0) == verifier &&
        WIFEXITED(verifier_status) && WEXITSTATUS(verifier_status) == 0,
        "drain peer is not inherited across a later exec")) return 16;
#endif
    pollfd accepting{listener, POLLIN, 0};
    if (::poll(&accepting, 1, 4000) != 1) return 4;
    const int connection = ::accept(listener, nullptr, nullptr);
    ::close(listener);
    ::unlink(path.c_str());
    ready_t ready{};
    if (!transfer(connection, &ready, sizeof(ready), false)) return 5;
    event("fixture_ready", ready.pid);
    const bool detached = mode == "detached" || mode == "detached_tail";
    int detached_observer = -1;
    if (detached) {
        if (!hooks.observe_detached) return 6;
        detached_observer = hooks.observe_detached(ready.pid);
        if (detached_observer < 0) return 7;
    }
    done_t external_result{};
    int external_slave = -1;
    std::thread writer;
    if (mode == "external" || mode == "hangup") {
        external_slave = ::open(ready.slave, O_WRONLY | O_NOCTTY | O_NONBLOCK);
        if (external_slave < 0) return 8;
        writer = std::thread([&] {
            external_result = write_until_released(external_slave, -1, &writing, &external_accepted);
            writer_ended = true;
        });
    }
    if (detached) {
        char go = 'G';
        if (!transfer(connection, &go, 1, true)) return 9;
    }
    if (mode == "hangup") {
        const auto producer_deadline = Clock::now() + std::chrono::seconds(2);
        while (external_accepted == 0 && !writer_ended && Clock::now() < producer_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    event("root_exit_released");
    const auto released_at = Clock::now();
    backend->write(QByteArrayLiteral("\n"));
    done_t flow_result{};
    if (mode == "flow") {
        const bool received = transfer(connection, &flow_result, sizeof(flow_result), false);
        event("flow_fixture_accepted", received, flow_result.accepted, flow_result.last_error);
        if (!received) return 13;
    }
    bool ok = true;
    if (mode == "graceful_cleanup_tail") {
        char admitted = 0;
        ok &= check(transfer(connection, &admitted, 1, false) && admitted == 'G',
            "graceful-stop fixture consumes its initial input before termination begins");
        ok &= check(backend->terminate().code == vnm_terminal::internal::Terminal_backend_result_code::ACCEPTED,
            "explicit graceful termination is accepted while the session leader remains alive");
        {
            std::unique_lock lock(mutex);
            ok &= check(changed.wait_for(lock, std::chrono::seconds(2), [&] {
                return output.size() >= sizeof(cleanup_tail) || exited;
            }) && output == std::string(sizeof(cleanup_tail), 'C') && !exited,
                "graceful cleanup handler output is delivered before session-leader exit");
        }
        char release = 'D';
        ok &= check(transfer(connection, &release, 1, true),
            "fixture session leader exits only after its graceful cleanup output is observed");
    }
    bool exit_before_reset = false;
    if (mode == "bounded_reset") {
        const auto output_deadline = Clock::now() + std::chrono::seconds(5);
        while (delivered == 0 && Clock::now() < output_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ok &= check(delivered > 0, "bounded reset begins with native output already in flight");
    }
    else if (mode == "reset") {
        std::unique_lock lock(cleanup_mutex);
        if (!cleanup_changed.wait_for(lock, std::chrono::seconds(25), [] { return boundary_seen.load(); })) {
            return 14;
        }
    }
    else
    {
        std::unique_lock lock(mutex);
        // Darwin's 1024-byte queue and 100ms readiness heartbeat mean the
        // 8192-byte/63-byte-read case can take about 17s before the root exits.
        // This bounds fixture observation, not the backend's final drain.
        const auto root_window = std::chrono::seconds(mode == "finite" || mode == "small" ? 25 : 5);
        (void)changed.wait_for(lock, root_window, [&] { return exited; });
        exit_before_reset = exited;
    }
    if (mode == "hangup") {
        const auto writer_deadline = Clock::now() + std::chrono::seconds(2);
        while (!writer_ended && Clock::now() < writer_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ok &= check(exit_before_reset && external_accepted > 0,
            "ordinary root exit is reported after an outside writer has used the terminal");
        ok &= check(writer_ended && (external_result.last_error == EIO || external_result.last_error == ENXIO),
            "session-leader exit revokes the outside writer's terminal without promising retained tail bytes");
    }
    if (mode == "silent") {
        const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - released_at);
        event("silent_exit_latency", latency.count());
        ok &= check(exit_before_reset && latency < std::chrono::seconds(2), "silent exit stays bounded");
    }
    if (mode == "external") {
        ok &= check(exit_before_reset && producer_alive_at_exit, "exit completes while outside producer is active");
        ok &= check(boundary_seen && elapsed_ms() - boundary_time < 2000,
            "outside producer cannot extend the final drain indefinitely");
    }
    sample("before_reset_queue", observed_master.load());
    if (external_slave >= 0) event("external_before_reset", !writer_ended.load(), external_accepted.load());
    if (detached_observer >= 0) {
        pollfd observed{detached_observer, POLLIN, 0};
        const int ended = ::poll(&observed, 1, 0);
        event("detached_before_reset_observation", ended, observed.revents);
    }
    event("reset_begin_producer_not_released", exit_before_reset);
    std::thread resetting([&] {
        backend.reset();
        event("reset_returned");
        const std::lock_guard lock(cleanup_mutex);
        reset_done = true;
        cleanup_changed.notify_all();
    });
    {
        std::unique_lock lock(cleanup_mutex);
        bounded_cleanup = cleanup_changed.wait_for(lock, std::chrono::seconds(2), [&] {
            return reset_done && cleanup_closed;
        });
        event("reset_and_close_with_producer_within_2s", bounded_cleanup);
    }
    if (external_slave >= 0) event("external_after_reset", !writer_ended.load(), external_accepted.load());
    if (detached) {
        char release = 'X';
        (void)transfer(connection, &release, 1, true);
        done_t result{};
        const bool receipt = transfer(connection, &result, sizeof(result), false);
        event("detached_producer_receipt", receipt, result.accepted, result.last_error);
    }
    if (external_slave >= 0) {
        const auto close_deadline = Clock::now() + std::chrono::seconds(2);
        while (!writer_ended && Clock::now() < close_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ok &= check(writer_ended, "master close ends the outside writer before fixture release");
    }
    writing = false;
    if (writer.joinable()) writer.join();
    if (external_slave >= 0) {
        event("external_producer_stopped", external_result.accepted, external_result.last_error);
        ::close(external_slave);
    }
    ::close(connection);
    resetting.join();
    {
        std::unique_lock lock(cleanup_mutex);
        if (!cleanup_changed.wait_for(lock, std::chrono::seconds(5), [&] { return cleanup_closed; })) {
            event("cleanup_still_pending");
            return 11;
        }
    }
    vnm_terminal::internal::set_posix_pty_io_hook_for_testing(nullptr);
    if (hooks.verify_cleanup) {
        ok &= hooks.verify_cleanup();
    }
#if defined(__linux__)
    ok &= check(peer_valid, "drain peer is nonblocking, close-on-exec and refers to the terminal");
    errno = 0;
    ok &= check(::fcntl(peer_descriptor, F_GETFD) == -1 && errno == EBADF, "cleanup closes the drain peer");
#endif
    if (detached_observer >= 0) {
        pollfd observed{detached_observer, POLLIN, 0};
        const int ended = ::poll(&observed, 1, 3000);
        event("detached_exact_observer_ended", ended, observed.revents);
        ::close(detached_observer);
        if (ended != 1) return 10;
    }
    event("case_complete");
    ok &= check(bounded_cleanup, "reset completes worker joins and actual master close within two seconds");
    ok &= check(error_count == 0 && !output_after_exit, "no backend errors or output after exit");
    ok &= check(largest_chunk <= (mode == "small" ? 63U : 16384U), "callback chunks preserve configured bound");
    if (mode == "graceful_cleanup_tail") {
        ok &= check(root_terminated_cleanly && exit_count == 1 && output_at_exit == output.size(),
            "one clean terminated root exit follows all explicit graceful-stop output");
    }
    else if (mode != "reset" && mode != "bounded_reset") {
        ok &= check(root_succeeded && exit_count == 1 && output_at_exit == output.size(),
            "one successful root exit follows all output");
    }
    if (mode == "finite" || mode == "small") {
        ok &= check(output == finite_output(mode), "all accepted finite root bytes arrive in FIFO order");
    }
    if (mode == "flow") {
        ok &= check(output == std::string(flow_result.accepted, 'R'), "all accepted stopped output is delivered");
    }
    if (mode == "cleanup_tail" || mode == "graceful_cleanup_tail") {
        ok &= check(output == std::string(sizeof(cleanup_tail), 'C'), "cleanup handler output remains deliverable");
    }
#if defined(__linux__)
    if (mode == "external") {
        ok &= check(output == std::string(external_result.accepted, 'd'),
            "freeze preserves every accepted outside-producer byte without admitting more");
    }
#endif
    if (mode == "detached_tail") {
        ok &= check(root_bytes == 16000, "detached producer does not displace the accepted original tail");
    }
    return ok ? 0 : 12;
}

#if defined(__linux__)
int peer_failure(const vnm_terminal::test_helpers::Native_backend_factory& make_backend)
{
    inject_peer_failure = true;
    vnm_terminal::internal::set_posix_pty_io_hook_for_testing(io_hook);
    auto backend = make_backend();
    vnm_terminal::internal::Terminal_launch_config config;
    config.argv = {QStringLiteral("/bin/sleep"), QStringLiteral("30")};
    config.initial_grid_size = vnm_terminal::internal::terminal_grid_size_t{24, 80};
    int errors = 0;
    vnm_terminal::internal::Terminal_backend_callbacks callbacks;
    callbacks.output_received = [](QByteArray) {};
    callbacks.process_exited = [](const auto&) {};
    callbacks.error_reported = [&](const auto& error) {
        if (error.code == vnm_terminal::internal::Terminal_backend_error_code::START_FAILED) ++errors;
    };
    const auto result = backend->start(config, callbacks);
    bool ok = check(result.code == vnm_terminal::internal::Terminal_backend_result_code::REJECTED &&
        result.native_dispatch_occurred && !result.start_outcome_determinate && errors == 1,
        "real peer-open failure reports a dispatched, indeterminate start rejection");
    backend.reset();
    {
        std::unique_lock lock(cleanup_mutex);
        ok &= check(cleanup_changed.wait_for(lock, std::chrono::seconds(5), [] { return cleanup_closed; }),
            "peer-open rejection finishes deferred native cleanup");
    }
    pollfd root{failed_root_observer, POLLIN, 0};
    ok &= check(root.fd >= 0 && ::poll(&root, 1, 1000) == 1,
        "the original admitted root ends after peer-open failure");
    if (root.fd >= 0) ::close(root.fd);
    vnm_terminal::internal::set_posix_pty_io_hook_for_testing(nullptr);
    return ok ? 0 : 1;
}
#endif

} // namespace

int vnm_terminal::test_helpers::run_exit_output_tests(
    int argc, char** argv, const Exit_output_hooks& hooks)
{
    ::signal(SIGPIPE, SIG_IGN);
#if defined(__linux__)
    if (argc == 3 && std::string_view(argv[1]) == "--peer-not-inherited") {
        errno = 0;
        return ::fcntl(std::atoi(argv[2]), F_GETFD) == -1 && errno == EBADF ? 0 : 1;
    }
#endif
    if (argc == 4 && std::string_view(argv[1]) == "--fixture") {
        return fixture(argv[2], argv[3]);
    }
    if (argc != 2) return 1;
    const std::string_view mode(argv[1]);
    if ((mode == "detached" || mode == "detached_tail") && !hooks.observe_detached) {
        return 2;
    }
    Watchdog watchdog(std::chrono::seconds(mode == "finite" || mode == "small" || mode == "reset" || mode == "bounded_reset" ? 40 : 18));
#if defined(__linux__)
    if (mode == "peer_failure") return peer_failure(hooks.make_backend);
#endif
    return backend_probe(argv[0], argv[1], hooks);
}
