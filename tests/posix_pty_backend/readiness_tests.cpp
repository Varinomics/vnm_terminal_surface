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
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
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

    bool start(const QString& executable)
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
        if (!check(m_backend->start(launch, callbacks_for(m_probe)).code ==
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
    if (scenario != "quiet" && scenario != "capacity") {
        return 2;
    }
    return test_readiness(QFileInfo(QString::fromLocal8Bit(argv[0])).absoluteFilePath(),
        scenario == "capacity") ? 0 : 1;
}
