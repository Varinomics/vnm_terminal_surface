#include "helpers/test_check.h"
#include "native_backend_cleanup_owner.h"
#include "posix_pty_io_test_hook.h"
#include "vnm_terminal/internal/posix_pty_backend.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;
using vnm_terminal::test_helpers::check;
using Clock = std::chrono::steady_clock;
using Worker = term::Posix_pty_io_worker_for_testing;
using Phase = term::Posix_pty_poll_phase_for_testing;

namespace {

constexpr auto k_wait_timeout = std::chrono::seconds(10);
constexpr auto k_quiet_interval = std::chrono::milliseconds(400);
constexpr qsizetype k_input_bytes = 1024 * 1024;
constexpr std::string_view k_output_marker = "readiness-output\n";
constexpr std::string_view k_input_marker = "readiness-input-complete\n";

class Owned_descriptor
{
public:
    explicit Owned_descriptor(int descriptor = -1)
    :
        m_descriptor(descriptor)
    {}

    ~Owned_descriptor() { reset(); }
    Owned_descriptor(const Owned_descriptor&) = delete;
    Owned_descriptor& operator=(const Owned_descriptor&) = delete;

    int get() const { return m_descriptor; }

    void reset(int descriptor = -1)
    {
        if (m_descriptor >= 0) {
            ::close(m_descriptor);
        }
        m_descriptor = descriptor;
    }

private:
    int m_descriptor;
};

bool wait_for_descriptor(int descriptor, short events)
{
    const auto deadline = Clock::now() + k_wait_timeout;
    for (;;) {
        const auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero()) {
            return false;
        }
        pollfd ready{descriptor, events, 0};
        const auto timeout = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
        const int result = ::poll(&ready, 1, static_cast<int>(timeout));
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return result > 0 && (ready.revents & events) != 0;
    }
}

bool send_control(int descriptor, char command)
{
    ssize_t count;
    do {
        count = ::send(descriptor, &command, 1, MSG_NOSIGNAL);
    }
    while (count < 0 && errno == EINTR);
    return count == 1;
}

bool receive_control(int descriptor, char expected)
{
    if (!wait_for_descriptor(descriptor, POLLIN)) {
        return false;
    }
    char command = 0;
    ssize_t count;
    do {
        count = ::recv(descriptor, &command, 1, 0);
    }
    while (count < 0 && errno == EINTR);
    return count == 1 && command == expected;
}

bool write_output(std::string_view bytes)
{
    while (!bytes.empty()) {
        const ssize_t count = ::write(STDOUT_FILENO, bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
    return true;
}

char input_byte(qsizetype index)
{
    return static_cast<char>('a' + index % 23);
}

bool read_input()
{
    char buffer[4096];
    qsizetype offset = 0;
    while (offset < k_input_bytes) {
        const ssize_t count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0 || count > k_input_bytes - offset) {
            return false;
        }
        for (ssize_t index = 0; index < count; ++index) {
            if (buffer[index] != input_byte(offset + index)) {
                return false;
            }
        }
        offset += count;
    }
    return true;
}

int run_child(const char* socket_path)
{
    ::alarm(25);
    termios settings{};
    if (::tcgetattr(STDIN_FILENO, &settings) != 0) {
        return 2;
    }
    ::cfmakeraw(&settings);
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &settings) != 0) {
        return 3;
    }
    Owned_descriptor connection(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (std::strlen(socket_path) >= sizeof(address.sun_path)) {
        return 4;
    }
    std::strcpy(address.sun_path, socket_path);
    if (connection.get() < 0 ||
        ::connect(connection.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        !send_control(connection.get(), 'R'))
    {
        return 5;
    }

    for (;;) {
        if (!wait_for_descriptor(connection.get(), POLLIN)) {
            return 6;
        }
        char command = 0;
        ssize_t count;
        do {
            count = ::recv(connection.get(), &command, 1, 0);
        }
        while (count < 0 && errno == EINTR);
        if (count != 1) {
            return 7;
        }
        switch (command) {
            case 'O':
                if (!write_output(k_output_marker)) {
                    return 8;
                }
                break;
            case 'I':
                if (!read_input() || !write_output(k_input_marker) ||
                    !send_control(connection.get(), 'C'))
                {
                    return 9;
                }
                break;
            case 'Q': return 0;
            default:  return 10;
        }
    }
}

struct Poll_sample
{
    Worker worker;
    Phase phase;
    std::vector<pollfd> descriptors;
    int timeout_ms;
    int result;
    int error_code;
};

struct Readiness_probe
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Poll_sample> polls;
    QByteArray output;
    std::optional<term::Terminal_backend_exit> exit;
    bool error = false;
};

std::mutex readiness_probe_mutex;
std::shared_ptr<Readiness_probe> readiness_probe;

void observe_poll(const term::posix_pty_poll_observation_t& observation)
{
    std::shared_ptr<Readiness_probe> probe;
    {
        const std::lock_guard lock(readiness_probe_mutex);
        probe = readiness_probe;
    }
    if (!probe) {
        return;
    }
    const std::lock_guard lock(probe->mutex);
    probe->polls.push_back({
        observation.worker,
        observation.phase,
        {observation.descriptors, observation.descriptors + observation.descriptor_count},
        observation.timeout_ms,
        observation.result,
        observation.error_code});
    probe->changed.notify_all();
}

term::Terminal_backend_callbacks callbacks_for(const std::shared_ptr<Readiness_probe>& probe)
{
    term::Terminal_backend_callbacks callbacks;
    callbacks.output_received = [
            probe
        ](
            QByteArray bytes)
        {
            const std::lock_guard lock(probe->mutex);
            probe->output += bytes;
            probe->changed.notify_all();
        };
    callbacks.process_exited = [
            probe
        ](
            term::Terminal_backend_exit exit)
        {
            const std::lock_guard lock(probe->mutex);
            probe->exit = exit;
            probe->changed.notify_all();
        };
    callbacks.error_reported = [
            probe
        ](
            term::Terminal_backend_error error)
        {
            const std::lock_guard lock(probe->mutex);
            probe->error = true;
            std::fprintf(stderr, "PTY readiness backend error: %s\n",
                error.message.toLocal8Bit().constData());
            probe->changed.notify_all();
        };
    return callbacks;
}

template<class Predicate>
bool wait_for_probe(const std::shared_ptr<Readiness_probe>& probe, Predicate predicate)
{
    std::unique_lock lock(probe->mutex);
    probe->changed.wait_for(lock, k_wait_timeout, [&] {
        return predicate(*probe) || probe->error || probe->exit.has_value();
    });
    return !probe->error && predicate(*probe);
}

std::optional<Poll_sample> first_wait(const Readiness_probe& probe, Worker worker)
{
    for (const auto& sample : probe.polls) {
        if (sample.worker == worker && sample.phase == Phase::BEFORE_WAIT) {
            return sample;
        }
    }
    return std::nullopt;
}

class Readiness_fixture
{
public:
    explicit Readiness_fixture(std::shared_ptr<Readiness_probe> probe)
    :
        m_probe(std::move(probe))
    {}

    ~Readiness_fixture() { finish(); }

    bool start(
        const QString& executable,
        std::optional<term::Terminal_backend_callbacks> callbacks = std::nullopt)
    {
        if (!check(m_directory.isValid(), "readiness control directory is available")) {
            return false;
        }
        const auto path = QFile::encodeName(m_directory.filePath(QStringLiteral("control")));
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (!check(path.size() < static_cast<qsizetype>(sizeof(address.sun_path)),
            "readiness control socket path fits the native address"))
        {
            return false;
        }
        std::memcpy(address.sun_path, path.constData(), path.size() + 1);
        m_listener.reset(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
        if (!check(m_listener.get() >= 0 &&
                ::bind(m_listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                ::listen(m_listener.get(), 1) == 0,
            "readiness control socket listens"))
        {
            return false;
        }
        {
            const std::lock_guard lock(readiness_probe_mutex);
            readiness_probe = m_probe;
        }
        term::set_posix_pty_poll_hook_for_testing(observe_poll);

        term::Terminal_launch_config launch;
        launch.argv = {executable, QStringLiteral("--child"), QFile::decodeName(path)};
        launch.working_directory = QFileInfo(executable).absolutePath();
        launch.initial_grid_size = term::terminal_grid_size_t{24, 80};
        m_backend = term::make_posix_pty_backend();
        if (!check(m_backend->start(launch, callbacks
                    ? std::move(*callbacks) : callbacks_for(m_probe)).code ==
                term::Terminal_backend_result_code::ACCEPTED,
            "actual PTY readiness fixture starts") ||
            !check(wait_for_descriptor(m_listener.get(), POLLIN),
                "actual PTY child connects its independent control channel"))
        {
            return false;
        }
        m_connection.reset(::accept4(m_listener.get(), nullptr, nullptr, SOCK_CLOEXEC));
        m_listener.reset();
        return check(m_connection.get() >= 0 && receive_control(m_connection.get(), 'R'),
            "actual PTY child is in raw mode and holds input consumption");
    }

    bool command(char value) const { return send_control(m_connection.get(), value); }
    bool receipt(char value) const { return receive_control(m_connection.get(), value); }
    term::Terminal_backend& backend() const { return *m_backend; }
    std::unique_ptr<term::Terminal_backend> take_backend() { return std::move(m_backend); }

    bool finish()
    {
        if (m_backend) {
            m_backend.reset();
            m_cleanup_ok = check(term::Native_backend_cleanup_reservation::wait_until_idle(
                    Clock::now() + k_wait_timeout),
                "readiness backend joins its native workers before retiring test observation");
        }
        term::set_posix_pty_poll_hook_for_testing(nullptr);
        {
            const std::lock_guard lock(readiness_probe_mutex);
            readiness_probe.reset();
        }
        return m_cleanup_ok;
    }

private:
    std::shared_ptr<Readiness_probe> m_probe;
    QTemporaryDir m_directory;
    Owned_descriptor m_listener;
    Owned_descriptor m_connection;
    std::unique_ptr<term::Terminal_backend> m_backend;
    bool m_cleanup_ok = true;
};

int terminal_descriptor(const Poll_sample& sample)
{
    for (const auto& descriptor : sample.descriptors) {
        if (::isatty(descriptor.fd)) {
            return descriptor.fd;
        }
    }
    return -1;
}

int wake_descriptor(const Poll_sample& sample)
{
    for (const auto& descriptor : sample.descriptors) {
        struct stat identity{};
        if (::fstat(descriptor.fd, &identity) == 0 && S_ISFIFO(identity.st_mode)) {
            return descriptor.fd;
        }
    }
    return -1;
}

short requested_events(const Poll_sample& sample, int descriptor)
{
    for (const auto& item : sample.descriptors) {
        if (item.fd == descriptor) {
            return item.events;
        }
    }
    return 0;
}

struct poll_counts_t
{
    std::size_t returned = 0U;
    std::size_t timeouts = 0U;
    std::size_t terminal_ready = 0U;
    std::size_t wake_ready = 0U;
};

poll_counts_t count_polls(
    const Readiness_probe& probe, Worker worker, int master, int wake,
    short terminal_event, std::size_t begin = 0U)
{
    poll_counts_t counts;
    for (std::size_t index = begin; index < probe.polls.size(); ++index) {
        const auto& sample = probe.polls[index];
        if (sample.worker != worker || sample.phase != Phase::AFTER_WAIT) {
            continue;
        }
        ++counts.returned;
        if (sample.result == 0) {
            ++counts.timeouts;
        }
        if (sample.result > 0) {
            for (const auto& descriptor : sample.descriptors) {
                counts.terminal_ready += descriptor.fd == master &&
                    (descriptor.revents & terminal_event) != 0;
                counts.wake_ready += descriptor.fd == wake && (descriptor.revents & POLLIN) != 0;
            }
        }
    }
    return counts;
}

bool wait_for_output(const std::shared_ptr<Readiness_probe>& probe, std::string_view marker)
{
    return wait_for_probe(probe, [&](const Readiness_probe& state) {
        return state.output.contains(QByteArray(marker.data(), static_cast<qsizetype>(marker.size())));
    });
}

bool finish_normally(Readiness_fixture& fixture, const std::shared_ptr<Readiness_probe>& probe)
{
    bool ok = check(fixture.command('Q'), "readiness child receives its explicit exit command");
    ok &= check(wait_for_probe(probe, [](const Readiness_probe& state) {
            return state.exit.has_value() && state.exit->reason == term::Terminal_exit_reason::EXITED &&
                state.exit->exit_code == 0;
        }),
        "actual PTY root exit is reported after readiness observations");
    ok &= fixture.finish();
    return ok;
}

bool test_readiness(const QString& executable, bool backpressure)
{
    auto probe = std::make_shared<Readiness_probe>();
    Readiness_fixture fixture(probe);
    if (!fixture.start(executable) ||
        !check(wait_for_probe(probe, [](const Readiness_probe& state) {
            return first_wait(state, Worker::READER).has_value();
        }), "quiet actual PTY reader reaches its native wait"))
    {
        return false;
    }

    Poll_sample reader;
    {
        const std::lock_guard lock(probe->mutex);
        reader = *first_wait(*probe, Worker::READER);
    }
    const int master = terminal_descriptor(reader);
    bool ok = check(master >= 0, "readiness observer sees the real PTY master");
    if (backpressure) {
        QByteArray input(k_input_bytes, '\0');
        for (qsizetype index = 0; index < input.size(); ++index) {
            input[index] = input_byte(index);
        }
        if (!check(fixture.backend().write(std::move(input)).code ==
                term::Terminal_backend_result_code::ACCEPTED,
                "backend accepts the bounded input workload") ||
            !check(wait_for_probe(probe, [](const Readiness_probe& state) {
                return first_wait(state, Worker::WRITER).has_value();
            }), "real PTY input write reaches EAGAIN before the child consumes input"))
        {
            return false;
        }

        Poll_sample writer;
        std::size_t release_begin;
        {
            const std::lock_guard lock(probe->mutex);
            writer = *first_wait(*probe, Worker::WRITER);
            release_begin = probe->polls.size();
        }
        const int wake = wake_descriptor(writer);
        ok &= check(fixture.command('I'), "independent control channel releases PTY input consumption");
        ok &= check(fixture.receipt('C'), "child receives and verifies every queued PTY input byte");
        ok &= check(wait_for_output(probe, k_input_marker),
            "input completion returns through the actual PTY output callback");

        {
            const std::lock_guard lock(probe->mutex);
            const auto total = count_polls(*probe, Worker::WRITER, master, wake, POLLOUT);
            const auto released = count_polls(*probe, Worker::WRITER, master, wake, POLLOUT, release_begin);
            std::fprintf(stdout,
                "PTY_READINESS writer timeout_ms=%d descriptors=%zu master_events=%d "
                "wake_events=%d returns=%zu timeouts=%zu capacity_returns=%zu wake_returns=%zu bytes=%lld\n",
                writer.timeout_ms, writer.descriptors.size(), requested_events(writer, master),
                requested_events(writer, wake), total.returned, total.timeouts,
                released.terminal_ready, released.wake_ready, static_cast<long long>(k_input_bytes));
            std::fflush(stdout);
            ok &= check(writer.timeout_ms == -1, "Linux write-capacity wait has no periodic retry timeout");
            ok &= check((requested_events(writer, master) & POLLOUT) != 0,
                "backpressured Linux writer watches the actual PTY master for POLLOUT");
            ok &= check(wake >= 0 && (requested_events(writer, wake) & POLLIN) != 0,
                "backpressured Linux writer also watches its explicit wake pipe");
            ok &= check(total.timeouts == 0U, "Linux input progress does not use periodic timeout retries");
            ok &= check(released.terminal_ready > 0U,
                "native PTY POLLOUT releases the writer after input consumption resumes");
            ok &= check(released.wake_ready == 0U,
                "capacity-driven input progress needs no separate backend wake notification");
        }
    }
    else {
        const int wake = wake_descriptor(reader);
        {
            std::unique_lock lock(probe->mutex);
            probe->changed.wait_for(lock, k_quiet_interval, [&] {
                return probe->error || probe->exit.has_value();
            });
            const auto quiet = count_polls(*probe, Worker::READER, master, wake, POLLIN);
            std::fprintf(stdout,
                "PTY_READINESS reader quiet_ms=%lld timeout_ms=%d descriptors=%zu "
                "master_events=%d wake_events=%d returns=%zu timeouts=%zu\n",
                static_cast<long long>(k_quiet_interval.count()), reader.timeout_ms,
                reader.descriptors.size(), requested_events(reader, master), requested_events(reader, wake),
                quiet.returned, quiet.timeouts);
            std::fflush(stdout);
            ok &= check(!probe->error && !probe->exit.has_value() && probe->output.isEmpty(),
                "actual PTY remains alive and silent throughout quiet observation");
            ok &= check(reader.timeout_ms == -1, "quiet Linux reader waits indefinitely for native events");
            ok &= check((requested_events(reader, master) & POLLIN) != 0,
                "quiet Linux reader watches the actual PTY master for output");
            ok &= check(wake >= 0 && (requested_events(reader, wake) & POLLIN) != 0,
                "quiet Linux reader also watches its explicit wake pipe");
            ok &= check(quiet.timeouts == 0U, "silent Linux PTY has no recurring native timeout wakeups");
        }
        ok &= check(fixture.command('O'), "independent control channel requests new PTY output");
        ok &= check(wait_for_output(probe, k_output_marker), "new PTY output wakes the quiet Linux reader");
        {
            const std::lock_guard lock(probe->mutex);
            ok &= check(count_polls(*probe, Worker::READER, master, wake, POLLIN).terminal_ready > 0U,
                "output delivery follows native PTY readability");
        }
    }
    ok &= finish_normally(fixture, probe);
    return ok;
}

constexpr auto k_barrier_timeout = 3 * k_wait_timeout;
constexpr auto k_delayed_reader_interval = std::chrono::milliseconds(300);

enum class Wake_fault
{
    NONE,
    INTERRUPT_ONCE,
    FILL_PIPE,
};

struct wait_barrier_t
{
    bool hold = false;
    bool reached = false;
    bool release = false;
    bool timed_out = false;
};

struct wake_channel_t
{
    unsigned attempts = 0U;
    std::thread::id first_waker;
    int descriptor = -1;
    struct stat identity{};
    bool identity_valid = false;
    bool identity_stable = true;
    bool nonblocking = false;
    bool interrupted = false;
    bool full = false;
    std::size_t filled_bytes = 0U;
    int fill_error = 0;
};

struct Wake_probe
{
    std::mutex mutex;
    std::condition_variable changed;
    std::array<wait_barrier_t, 2> barriers;
    std::array<wake_channel_t, 2> channels;
    Wake_fault fault = Wake_fault::NONE;
    bool root_seen = false;
    Clock::time_point root_seen_at;
    bool join_reached = false;
    bool boundary_seen = false;
};

std::mutex wake_probe_mutex;
std::shared_ptr<Wake_probe> wake_probe;

std::size_t worker_index(Worker worker)
{
    return worker == Worker::READER ? 0U : 1U;
}

std::shared_ptr<Wake_probe> active_wake_probe()
{
    const std::lock_guard lock(wake_probe_mutex);
    return wake_probe;
}

bool same_identity(const struct stat& first, const struct stat& second)
{
    return first.st_dev == second.st_dev && first.st_ino == second.st_ino &&
        first.st_rdev == second.st_rdev;
}

void observe_controlled_poll(const term::posix_pty_poll_observation_t& observation)
{
    observe_poll(observation);
    const auto probe = active_wake_probe();
    if (!probe || observation.phase != Phase::BEFORE_WAIT) {
        return;
    }
    std::unique_lock lock(probe->mutex);
    auto& barrier = probe->barriers[worker_index(observation.worker)];
    if (!barrier.hold || barrier.reached) {
        return;
    }
    barrier.reached = true;
    probe->changed.notify_all();
    barrier.timed_out = !probe->changed.wait_for(lock, k_barrier_timeout, [&] {
        return barrier.release;
    });
}

int observe_wake_write(Worker worker, int descriptor)
{
    const auto probe = active_wake_probe();
    if (!probe) {
        return 0;
    }
    const std::lock_guard lock(probe->mutex);
    auto& channel = probe->channels[worker_index(worker)];
    struct stat identity{};
    const bool identity_valid = ::fstat(descriptor, &identity) == 0;
    const int flags = ::fcntl(descriptor, F_GETFL);
    ++channel.attempts;
    if (channel.attempts == 1U) {
        channel.first_waker = std::this_thread::get_id();
        channel.descriptor = descriptor;
        channel.identity = identity;
        channel.identity_valid = identity_valid;
        channel.nonblocking = flags >= 0 && (flags & O_NONBLOCK) != 0;
    }
    else {
        channel.identity_stable &= descriptor == channel.descriptor && identity_valid &&
            same_identity(channel.identity, identity);
    }
    probe->changed.notify_all();
    if (channel.attempts != 1U) {
        return 0;
    }
    if (probe->fault == Wake_fault::INTERRUPT_ONCE) {
        channel.interrupted = true;
        return EINTR;
    }
    if (probe->fault != Wake_fault::FILL_PIPE || !channel.nonblocking) {
        return 0;
    }

    char bytes[4096]{};
    std::size_t request = sizeof(bytes);
    for (;;) {
        const ssize_t count = ::write(descriptor, bytes, request);
        if (count > 0) {
            channel.filled_bytes += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (request != 1U) {
                // An atomic large write may fail while a notification byte
                // still fits. Establish saturation for the backend's byte.
                request = 1U;
                continue;
            }
            channel.full = true;
        }
        else {
            channel.fill_error = count < 0 ? errno : EIO;
        }
        return 0;
    }
}

void observe_wake_lifecycle(term::Posix_pty_io_point_for_testing point, int)
{
    const auto probe = active_wake_probe();
    if (!probe) {
        return;
    }
    const std::lock_guard lock(probe->mutex);
    switch (point) {
        case term::Posix_pty_io_point_for_testing::ROOT_EXIT:
            probe->root_seen = true;
            probe->root_seen_at = Clock::now();
            break;
        case term::Posix_pty_io_point_for_testing::SHUTDOWN_JOIN:
            probe->join_reached = true;
            break;
        case term::Posix_pty_io_point_for_testing::OUTPUT_BOUNDARY:
            probe->boundary_seen = true;
            break;
        default:
            return;
    }
    probe->changed.notify_all();
}

void install_wake_observation(const std::shared_ptr<Wake_probe>& probe)
{
    {
        const std::lock_guard lock(wake_probe_mutex);
        wake_probe = probe;
    }
    term::set_posix_pty_poll_hook_for_testing(observe_controlled_poll);
    term::set_posix_pty_wake_write_error_hook_for_testing(observe_wake_write);
    term::set_posix_pty_io_hook_for_testing(observe_wake_lifecycle);
}

void uninstall_wake_observation()
{
    term::set_posix_pty_io_hook_for_testing(nullptr);
    term::set_posix_pty_wake_write_error_hook_for_testing(nullptr);
    const std::lock_guard lock(wake_probe_mutex);
    wake_probe.reset();
}

void release_waiters(const std::shared_ptr<Wake_probe>& probe)
{
    const std::lock_guard lock(probe->mutex);
    for (auto& barrier : probe->barriers) {
        barrier.release = true;
    }
    probe->changed.notify_all();
}

template<class Probe, class Predicate>
bool wait_for_test_state(const std::shared_ptr<Probe>& probe, Predicate predicate)
{
    std::unique_lock lock(probe->mutex);
    return probe->changed.wait_for(lock, k_wait_timeout, [&] { return predicate(*probe); });
}

bool wait_for_barrier(const std::shared_ptr<Wake_probe>& probe, Worker worker)
{
    return check(wait_for_test_state(probe, [&](const Wake_probe& state) {
            return state.barriers[worker_index(worker)].reached;
        }),
        worker == Worker::READER
            ? "reader reaches the controlled native wait boundary"
            : "backpressured writer reaches the controlled native wait boundary");
}

std::size_t poll_position(const std::shared_ptr<Readiness_probe>& probe)
{
    const std::lock_guard lock(probe->mutex);
    return probe->polls.size();
}

bool queue_blocked_input(Readiness_fixture& fixture, const std::shared_ptr<Readiness_probe>& probe)
{
    return check(fixture.backend().write(QByteArray(k_input_bytes, 'w')).code ==
            term::Terminal_backend_result_code::ACCEPTED,
            "wake fixture queues input while the child holds consumption") &&
        check(wait_for_probe(probe, [](const Readiness_probe& state) {
            return first_wait(state, Worker::WRITER).has_value();
        }), "wake fixture reaches actual PTY write backpressure");
}

bool barriers_released(const std::shared_ptr<Wake_probe>& probe)
{
    const std::lock_guard lock(probe->mutex);
    return check(std::none_of(probe->barriers.begin(), probe->barriers.end(), [](const auto& barrier) {
            return barrier.timed_out;
        }), "native wait barriers were explicitly released before their watchdogs");
}

bool test_wake_delivery(const QString& executable, bool full_pipe)
{
    auto probe = std::make_shared<Readiness_probe>();
    Readiness_fixture fixture(probe);
    if (!fixture.start(executable) ||
        !check(wait_for_probe(probe, [](const Readiness_probe& state) {
            return first_wait(state, Worker::READER).has_value();
        }), "wake fixture reader reaches its native wait"))
    {
        return false;
    }
    auto wake = std::make_shared<Wake_probe>();
    for (auto& barrier : wake->barriers) {
        barrier.hold = full_pipe;
    }
    install_wake_observation(wake);

    bool ok = true;
    if (full_pipe) {
        ok &= check(fixture.backend().set_output_paused(false).code ==
                term::Terminal_backend_result_code::ACCEPTED,
            "resume moves the reader to the controlled wait boundary");
        ok &= wait_for_barrier(wake, Worker::READER);
    }
    if (ok) {
        ok &= queue_blocked_input(fixture, probe);
    }
    if (ok && full_pipe) {
        ok &= wait_for_barrier(wake, Worker::WRITER);
    }
    if (ok) {
        const std::size_t begin = poll_position(probe);
        {
            const std::lock_guard lock(wake->mutex);
            wake->channels = {};
            wake->fault = full_pipe ? Wake_fault::FILL_PIPE : Wake_fault::INTERRUPT_ONCE;
        }
        ok &= check(fixture.backend().set_output_paused(false).code ==
                term::Terminal_backend_result_code::ACCEPTED,
            "resume remains accepted while native wake delivery is challenged");

        bool delivery_established = true;
        {
            const std::lock_guard lock(wake->mutex);
            for (std::size_t index = 0; index < wake->channels.size(); ++index) {
                const auto& channel = wake->channels[index];
                std::fprintf(stdout,
                    "PTY_WAKE mode=%s worker=%zu attempts=%u interrupted=%d full=%d "
                    "filled_bytes=%zu fill_error=%d identity_stable=%d\n",
                    full_pipe ? "full" : "eintr", index, channel.attempts, channel.interrupted,
                    channel.full, channel.filled_bytes, channel.fill_error, channel.identity_stable);
                delivery_established &= check(channel.identity_valid && channel.identity_stable &&
                        channel.nonblocking,
                    "wake attempts retain the same live nonblocking pipe endpoint");
                delivery_established &= full_pipe
                    ? check(channel.full && channel.filled_bytes > 0U && channel.fill_error == 0 &&
                            channel.attempts == 1U,
                        "a real one-byte-full wake pipe coalesces notification without retrying")
                    : check(channel.interrupted && channel.attempts == 2U,
                        "an interrupted wake write is retried during the same state transition");
            }
            std::fflush(stdout);
            wake->fault = Wake_fault::NONE;
        }
        ok &= delivery_established;
        release_waiters(wake);
        if (delivery_established) {
            ok &= check(wait_for_probe(probe, [&](const Readiness_probe& state) {
                const auto reader = first_wait(state, Worker::READER);
                const auto writer = first_wait(state, Worker::WRITER);
                return reader && writer &&
                    count_polls(state, Worker::READER, -1, wake_descriptor(*reader), POLLIN, begin).wake_ready > 0U &&
                    count_polls(state, Worker::WRITER, -1, wake_descriptor(*writer), POLLOUT, begin).wake_ready > 0U;
            }), "both native waits observe the delivered pipe notification without another wake");
        }
    }
    release_waiters(wake);
    ok &= finish_normally(fixture, probe);
    ok &= barriers_released(wake);
    uninstall_wake_observation();
    return ok;
}

enum class Callback_stop
{
    REVOKE,
    TERMINATE,
    ROOT_EXIT,
};

struct Callback_probe
{
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    bool returned = false;
    bool timed_out = false;
    bool stop_returned = false;
    bool stop_accepted = false;
    bool destroyed_in_callback = false;
    std::unique_ptr<term::Terminal_backend> backend;
};

void release_callback(const std::shared_ptr<Callback_probe>& probe)
{
    const std::lock_guard lock(probe->mutex);
    probe->release = true;
    probe->changed.notify_all();
}

term::Terminal_backend_callbacks held_output_callbacks(
    const std::shared_ptr<Readiness_probe>& readiness,
    const std::shared_ptr<Callback_probe>& callback)
{
    auto callbacks = callbacks_for(readiness);
    callbacks.output_received = [
            callback,
            receive = std::move(callbacks.output_received)
        ](
            QByteArray bytes)
        {
            receive(std::move(bytes));
            std::unique_lock lock(callback->mutex);
            if (callback->entered) {
                return;
            }
            callback->entered = true;
            callback->changed.notify_all();
            callback->timed_out = !callback->changed.wait_for(lock, k_barrier_timeout, [&] {
                return callback->release;
            });
            callback->returned = true;
            callback->changed.notify_all();
        };
    return callbacks;
}

bool native_read_then_wait(const Readiness_probe& probe, int master, std::size_t begin)
{
    bool readable = false;
    for (std::size_t index = begin; index < probe.polls.size(); ++index) {
        const auto& sample = probe.polls[index];
        if (sample.worker != Worker::READER) {
            continue;
        }
        if (readable && sample.phase == Phase::BEFORE_WAIT) {
            return true;
        }
        if (sample.phase == Phase::AFTER_WAIT && sample.result > 0) {
            readable |= std::any_of(sample.descriptors.begin(), sample.descriptors.end(), [&](const auto& item) {
                return item.fd == master && (item.revents & POLLIN) != 0;
            });
        }
    }
    return false;
}

const char* callback_stop_name(Callback_stop stop)
{
    switch (stop) {
        case Callback_stop::REVOKE:    return "revoke";
        case Callback_stop::TERMINATE: return "terminate";
        case Callback_stop::ROOT_EXIT: return "root_exit";
        default:                       return "unknown";
    }
}

bool test_callback_state_wake(const QString& executable, Callback_stop stop)
{
    auto probe = std::make_shared<Readiness_probe>();
    auto callback = std::make_shared<Callback_probe>();
    Readiness_fixture fixture(probe);
    if (!fixture.start(executable, held_output_callbacks(probe, callback)) ||
        !check(wait_for_probe(probe, [](const Readiness_probe& state) {
            return first_wait(state, Worker::READER).has_value();
        }), "callback wake fixture reader reaches its native wait"))
    {
        return false;
    }
    auto wake = std::make_shared<Wake_probe>();
    wake->barriers[worker_index(Worker::WRITER)].hold = true;
    install_wake_observation(wake);
    bool ok = queue_blocked_input(fixture, probe) && wait_for_barrier(wake, Worker::WRITER);

    int master = -1;
    int writer_wake = -1;
    if (ok) {
        const std::lock_guard lock(probe->mutex);
        master = terminal_descriptor(*first_wait(*probe, Worker::READER));
        writer_wake = wake_descriptor(*first_wait(*probe, Worker::WRITER));
    }
    if (ok && stop != Callback_stop::REVOKE) {
        ok &= check(fixture.backend().set_output_paused(true).code ==
                term::Terminal_backend_result_code::ACCEPTED,
            "callback wake fixture pauses output before buffering its marker");
    }
    const std::size_t output_begin = poll_position(probe);
    if (ok) {
        ok &= check(fixture.command('O'), "callback wake fixture requests controlled output");
    }
    if (ok && stop != Callback_stop::REVOKE) {
        ok &= check(wait_for_probe(probe, [&](const Readiness_probe& state) {
                return state.output.isEmpty() && native_read_then_wait(state, master, output_begin);
            }), "reader buffers paused output before the stop transition claims its replay");
    }
    if (ok && stop == Callback_stop::REVOKE) {
        ok &= check(wait_for_test_state(callback, [](const Callback_probe& state) { return state.entered; }),
            "output callback is admitted before external revocation");
    }

    const std::size_t stop_begin = poll_position(probe);
    std::thread stopper;
    bool transition_started = false;
    if (ok) {
        {
            const std::lock_guard lock(wake->mutex);
            wake->channels = {};
        }
        if (stop == Callback_stop::REVOKE) {
            stopper = std::thread([
                    callback,
                    backend = fixture.take_backend()
                ]() mutable
                {
                    backend.reset();
                    const std::lock_guard lock(callback->mutex);
                    callback->stop_returned = true;
                    callback->changed.notify_all();
                });
            transition_started = true;
        }
        else
        if (stop == Callback_stop::TERMINATE) {
            stopper = std::thread([
                    callback,
                    backend = &fixture.backend()
                ]
                {
                    const auto result = backend->terminate();
                    const std::lock_guard lock(callback->mutex);
                    callback->stop_accepted = result.code == term::Terminal_backend_result_code::ACCEPTED;
                    callback->stop_returned = true;
                    callback->changed.notify_all();
                });
            transition_started = true;
        }
        else {
            ok &= check(fixture.command('Q'), "root exits while output is buffered and writer is held");
        }
        if (ok) {
            ok &= check(wait_for_test_state(callback, [](const Callback_probe& state) { return state.entered; }),
                "stop transition reaches the controlled output callback");
        }
    }
    if (ok) {
        // The child's control timeout can independently trigger root-exit
        // wakes. Revoke/terminate evidence must come from their own caller.
        const bool controlled_transition = stop != Callback_stop::ROOT_EXIT;
        const auto expected_waker = stopper.get_id();
        const bool woke_before_callback_release = wait_for_test_state(wake, [&](const Wake_probe& state) {
            return state.channels[0].attempts > 0U && state.channels[1].attempts > 0U &&
                (!controlled_transition ||
                    (!state.root_seen && state.channels[0].first_waker == expected_waker &&
                     state.channels[1].first_waker == expected_waker));
        });
        {
            const std::lock_guard lock(wake->mutex);
            std::fprintf(stdout,
                "PTY_CALLBACK_STOP mode=%s reader_wakes=%u writer_wakes=%u issuer_check=%d "
                "reader_issuer_match=%d writer_issuer_match=%d root_seen=%d\n",
                callback_stop_name(stop), wake->channels[0].attempts, wake->channels[1].attempts,
                controlled_transition, wake->channels[0].first_waker == expected_waker,
                wake->channels[1].first_waker == expected_waker, wake->root_seen);
            std::fflush(stdout);
        }
        ok &= check(woke_before_callback_release,
            "the stop publisher wakes both I/O workers before the held callback completes");
        release_waiters(wake);
        if (woke_before_callback_release) {
            ok &= check(wait_for_probe(probe, [&](const Readiness_probe& state) {
                return count_polls(state, Worker::WRITER, master, writer_wake, POLLOUT, stop_begin).wake_ready > 0U;
            }), "native writer observes cancellation while the output callback remains held");
        }
        if (controlled_transition) {
            const std::lock_guard lock(wake->mutex);
            ok &= check(!wake->root_seen,
                "controlled cancellation is observed before any independent root exit");
        }
        const std::lock_guard lock(callback->mutex);
        ok &= check(!callback->returned && !callback->stop_returned,
            "native cancellation does not depend on completing the admitted callback");
    }

    release_waiters(wake);
    release_callback(callback);
    if (stopper.joinable()) {
        stopper.join();
    }
    if (stop == Callback_stop::REVOKE && transition_started) {
        ok &= check(term::Native_backend_cleanup_reservation::wait_until_idle(Clock::now() + k_wait_timeout),
            "revocation cleanup joins native workers after the admitted callback returns");
    }
    else
    if (ok) {
        const auto expected = stop == Callback_stop::TERMINATE
            ? term::Terminal_exit_reason::TERMINATED : term::Terminal_exit_reason::EXITED;
        ok &= check(wait_for_probe(probe, [&](const Readiness_probe& state) {
                return state.exit.has_value() && state.exit->reason == expected;
            }), "stop transition reports its native exit after paused callback delivery");
    }
    {
        const std::lock_guard lock(callback->mutex);
        ok &= check(!callback->timed_out, "output callback barrier was explicitly released");
        if (stop == Callback_stop::TERMINATE && transition_started) {
            ok &= check(callback->stop_returned && callback->stop_accepted,
                "termination completes after releasing the paused callback");
        }
    }
    ok &= fixture.finish();
    ok &= barriers_released(wake);
    uninstall_wake_observation();
    return ok;
}

bool test_callback_destruction_with_held_writer(const QString& executable)
{
    auto probe = std::make_shared<Readiness_probe>();
    auto callback = std::make_shared<Callback_probe>();
    auto callbacks = callbacks_for(probe);
    callbacks.output_received = [
            callback,
            receive = std::move(callbacks.output_received)
        ](
            QByteArray bytes)
        {
            receive(std::move(bytes));
            std::unique_ptr<term::Terminal_backend> backend;
            {
                const std::lock_guard lock(callback->mutex);
                backend = std::move(callback->backend);
            }
            const bool destroyed = static_cast<bool>(backend);
            backend.reset();
            {
                const std::lock_guard lock(callback->mutex);
                callback->destroyed_in_callback = destroyed;
                callback->returned = true;
                callback->changed.notify_all();
            }
        };

    Readiness_fixture fixture(probe);
    if (!fixture.start(executable, std::move(callbacks))) {
        return false;
    }
    auto wake = std::make_shared<Wake_probe>();
    wake->barriers[worker_index(Worker::WRITER)].hold = true;
    install_wake_observation(wake);
    bool ok = queue_blocked_input(fixture, probe) && wait_for_barrier(wake, Worker::WRITER);

    int writer_wake = -1;
    struct stat reader_identity{};
    bool backend_transferred = false;
    if (ok) {
        {
            const std::lock_guard lock(probe->mutex);
            writer_wake = wake_descriptor(*first_wait(*probe, Worker::WRITER));
        }
        ok &= check(writer_wake >= 0 && ::fstat(writer_wake, &reader_identity) == 0,
            "held writer retains a known native wake-pipe read endpoint");
    }
    if (ok) {
        {
            const std::lock_guard lock(callback->mutex);
            callback->backend = fixture.take_backend();
        }
        backend_transferred = true;
        ok &= check(fixture.command('O'), "controlled PTY output requests callback-driven destruction");
        ok &= check(wait_for_test_state(callback, [](const Callback_probe& state) {
                return state.returned && state.destroyed_in_callback;
            }), "output callback destroys its backend without waiting for the held writer");
        ok &= check(wait_for_test_state(wake, [](const Wake_probe& state) { return state.join_reached; }),
            "callback-driven cleanup reaches native worker joining while the writer remains held");
    }
    if (ok) {
        wake_channel_t channel;
        {
            const std::lock_guard lock(wake->mutex);
            channel = wake->channels[worker_index(Worker::WRITER)];
        }
        struct stat retained_reader{};
        struct stat retained_writer{};
        const bool reader_retained = ::fstat(writer_wake, &retained_reader) == 0 &&
            same_identity(reader_identity, retained_reader);
        const bool writer_retained = channel.identity_valid &&
            ::fstat(channel.descriptor, &retained_writer) == 0 &&
            same_identity(channel.identity, retained_writer);
        ok &= check(reader_retained && writer_retained && same_identity(reader_identity, channel.identity),
            "callback-driven cleanup retains both ends of the held writer's wake pipe until joining");
        std::fprintf(stdout, "PTY_CALLBACK_DESTROY writer_held=1 reader_retained=%d writer_retained=%d\n",
            reader_retained, writer_retained);
        std::fflush(stdout);
    }

    release_waiters(wake);
    std::unique_ptr<term::Terminal_backend> remaining_backend;
    {
        const std::lock_guard lock(callback->mutex);
        remaining_backend = std::move(callback->backend);
    }
    remaining_backend.reset();
    if (backend_transferred) {
        ok &= check(term::Native_backend_cleanup_reservation::wait_until_idle(Clock::now() + k_wait_timeout),
            "callback-driven native cleanup settles after releasing the held writer");
    }
    ok &= fixture.finish();
    ok &= barriers_released(wake);
    uninstall_wake_observation();
    return ok;
}

bool test_delayed_drain_deadline(const QString& executable)
{
    auto probe = std::make_shared<Readiness_probe>();
    Readiness_fixture fixture(probe);
    if (!fixture.start(executable)) {
        return false;
    }
    auto wake = std::make_shared<Wake_probe>();
    wake->barriers[worker_index(Worker::READER)].hold = true;
    install_wake_observation(wake);
    bool ok = check(fixture.backend().set_output_paused(false).code ==
            term::Terminal_backend_result_code::ACCEPTED,
        "resume moves the reader to the delayed observation barrier");
    ok &= wait_for_barrier(wake, Worker::READER);
    if (ok) {
        ok &= check(fixture.command('Q'), "root exits while the reader is deliberately held");
        ok &= check(wait_for_test_state(wake, [](const Wake_probe& state) { return state.root_seen; }),
            "native root exit is observed before the reader is released");
    }
    if (ok) {
        std::unique_lock lock(wake->mutex);
        // Delaying beyond the established admission deadline distinguishes an
        // absolute deadline from a new window granted when the reader resumes.
        wake->changed.wait_until(lock, wake->root_seen_at + k_delayed_reader_interval, [&] {
            return wake->barriers[worker_index(Worker::READER)].timed_out;
        });
    }
    const std::size_t release_begin = poll_position(probe);
    release_waiters(wake);
    if (ok) {
        ok &= check(wait_for_probe(probe, [](const Readiness_probe& state) {
                return state.exit.has_value() && state.exit->reason == term::Terminal_exit_reason::EXITED &&
                    state.exit->exit_code == 0;
            }), "delayed reader completes the native root-exit protocol");
        {
            const std::lock_guard lock(probe->mutex);
            std::size_t later_waits = 0U;
            std::size_t renewed_waits = 0U;
            for (std::size_t index = release_begin; index < probe->polls.size(); ++index) {
                const auto& sample = probe->polls[index];
                if (sample.worker == Worker::READER && sample.phase == Phase::BEFORE_WAIT) {
                    ++later_waits;
                    renewed_waits += sample.timeout_ms != 0;
                }
            }
            std::fprintf(stdout, "PTY_DRAIN_DEADLINE delayed_ms=%lld later_waits=%zu renewed_waits=%zu\n",
                static_cast<long long>(k_delayed_reader_interval.count()), later_waits, renewed_waits);
            std::fflush(stdout);
            ok &= check(renewed_waits == 0U,
                "delayed root observation grants no new positive or indefinite native drain wait");
        }
        const std::lock_guard lock(wake->mutex);
        ok &= check(wake->boundary_seen, "delayed reader still establishes the native output-admission boundary");
    }
    ok &= fixture.finish();
    ok &= barriers_released(wake);
    uninstall_wake_observation();
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::string_view(argv[1]) == "--child") {
        return run_child(argv[2]);
    }
    if (argc != 2) {
        return 2;
    }
    const std::string_view scenario(argv[1]);
    const auto executable = QFileInfo(QString::fromLocal8Bit(argv[0])).absoluteFilePath();
    if (scenario == "quiet" || scenario == "capacity") {
        return test_readiness(executable, scenario == "capacity") ? 0 : 1;
    }
    if (scenario == "wake_eintr" || scenario == "wake_full") {
        return test_wake_delivery(executable, scenario == "wake_full") ? 0 : 1;
    }
    if (scenario == "callback_revoke") {
        return test_callback_state_wake(executable, Callback_stop::REVOKE) ? 0 : 1;
    }
    if (scenario == "callback_terminate") {
        return test_callback_state_wake(executable, Callback_stop::TERMINATE) ? 0 : 1;
    }
    if (scenario == "callback_root_exit") {
        return test_callback_state_wake(executable, Callback_stop::ROOT_EXIT) ? 0 : 1;
    }
    if (scenario == "callback_destroy") {
        return test_callback_destruction_with_held_writer(executable) ? 0 : 1;
    }
    if (scenario == "drain_deadline") {
        return test_delayed_drain_deadline(executable) ? 0 : 1;
    }
    return 2;
}
