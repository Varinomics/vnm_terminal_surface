#include <vnm_process_custody/process_tree.h>

#include <libproc.h>
#include <poll.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <optional>

namespace custody = vnm::process_custody;

namespace {

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

std::uint64_t process_birth(pid_t process_id)
{
    struct proc_bsdinfo info{};
    if (proc_pidinfo(process_id, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info)) {
        return 0;
    }
    return info.pbi_start_tvsec * 1'000'000ULL + info.pbi_start_tvusec;
}

std::uint64_t process_cpu_usec()
{
    timespec time{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(time.tv_sec) * 1'000'000 +
        static_cast<std::uint64_t>(time.tv_nsec) / 1000;
}

std::uint64_t consume_cpu()
{
    // The independent process clock makes this a unit/accounting check,
    // irrespective of wall-clock scheduling or the host's processor count.
    constexpr std::uint64_t work_usec = 80'000;
    const auto start = process_cpu_usec();
    if (start == 0) {
        return 0;
    }
    auto current = start;
    while (current - start < work_usec) {
        current = process_cpu_usec();
        if (current < start) {
            return 0;
        }
    }
    return current - start;
}

struct response_t
{
    pid_t process_id = 0;
    std::uint64_t cpu_usec = 0;
};

bool receive_response(int descriptor, response_t* response)
{
    pollfd ready{descriptor, POLLIN, 0};
    if (poll(&ready, 1, 3000) != 1) {
        return false;
    }
    return read(descriptor, response, sizeof(*response)) == sizeof(*response);
}

int run_grandchild(int commands, int responses)
{
    response_t response{getpid(), consume_cpu()};
    if (write(responses, &response, sizeof(response)) != sizeof(response)) {
        return 1;
    }
    char command = 0;
    while (read(commands, &command, 1) == 1) {
        if (command != 'W') {
            return 1;
        }
        response.cpu_usec = consume_cpu();
        if (write(responses, &response, sizeof(response)) != sizeof(response)) {
            return 1;
        }
    }
    return 0;
}

class Descendant_fixture
{
public:
    ~Descendant_fixture()
    {
        retire_intermediate();
        retire_grandchild();
        if (m_responses >= 0) {
            close(m_responses);
        }
    }

    bool start()
    {
        int commands[2]{-1, -1};
        int responses[2]{-1, -1};
        int release[2]{-1, -1};
        if (pipe(commands) != 0 || pipe(responses) != 0 || pipe(release) != 0) {
            for (int descriptor : {commands[0], commands[1], responses[0], responses[1], release[0], release[1]}) {
                if (descriptor >= 0) {
                    close(descriptor);
                }
            }
            return false;
        }
        m_intermediate = fork();
        if (m_intermediate == 0) {
            close(commands[1]);
            close(responses[0]);
            close(release[1]);
            const pid_t grandchild = fork();
            if (grandchild == 0) {
                close(release[0]);
                _exit(run_grandchild(commands[0], responses[1]));
            }
            close(commands[0]);
            close(responses[1]);
            if (grandchild < 0) {
                _exit(1);
            }
            char done = 0;
            (void)read(release[0], &done, 1);
            _exit(0);
        }
        close(commands[0]);
        close(responses[1]);
        close(release[0]);
        m_commands = commands[1];
        m_responses = responses[0];
        m_release = release[1];
        response_t ready;
        if (m_intermediate < 0 || !receive_response(m_responses, &ready)) {
            return false;
        }
        m_grandchild_observer = custody::open_process_observer(ready.process_id,
            process_birth(ready.process_id), nullptr);
        m_initial_cpu_usec = ready.cpu_usec;
        return m_grandchild_observer >= 0;
    }

    std::uint64_t initial_cpu_usec() const { return m_initial_cpu_usec; }

    std::uint64_t work()
    {
        response_t response;
        if (write(m_commands, "W", 1) != 1 || !receive_response(m_responses, &response)) {
            return 0;
        }
        return response.cpu_usec;
    }

    bool retire_intermediate()
    {
        if (m_release >= 0) {
            close(m_release);
            m_release = -1;
        }
        if (m_intermediate <= 0) {
            return true;
        }
        int status = 0;
        pid_t waited;
        do {
            waited = waitpid(m_intermediate, &status, 0);
        }
        while (waited < 0 && errno == EINTR);
        m_intermediate = -1;
        return waited > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    bool retire_grandchild()
    {
        if (m_commands >= 0) {
            close(m_commands);
            m_commands = -1;
        }
        if (m_grandchild_observer < 0) {
            return true;
        }
        pollfd exited{m_grandchild_observer, POLLIN, 0};
        const bool observed = poll(&exited, 1, 3000) == 1 && (exited.revents & POLLIN) != 0;
        custody::close_native_handle(m_grandchild_observer);
        m_grandchild_observer = custody::k_invalid_handle;
        return observed;
    }

private:
    pid_t m_intermediate = -1;
    int m_commands = -1;
    int m_responses = -1;
    int m_release = -1;
    std::uint64_t m_initial_cpu_usec = 0;
    custody::Native_handle m_grandchild_observer = custody::k_invalid_handle;
};

bool accounts_work(std::optional<std::uint64_t> before, std::optional<std::uint64_t> after,
    std::uint64_t measured)
{
    // Allow bounded observer/IPC CPU overhead while rejecting wrong native
    // units, omitted descendants, and counting the same work twice.
    return before && after && measured > 0 && *after >= *before &&
        *after - *before >= measured * 3 / 4 &&
        *after - *before <= measured * 5 / 4 + 5000;
}

} // namespace

int main()
{
    const auto root_birth = process_birth(getpid());
    custody::Process_cpu_tracker tracker;
    bool ok = check(!tracker.cpu_time_usec(), "uninitialized tracker is unavailable");
    tracker.set_root(getpid(), root_birth + 1);
    ok &= check(!tracker.cpu_time_usec(), "mismatched birth cannot attribute a recycled PID");
    tracker.set_root(getpid(), root_birth);
    auto before = tracker.cpu_time_usec();
    ok &= check(before && *before == 0, "first observation baselines the root");
    const auto root_work = consume_cpu();
    auto after = tracker.cpu_time_usec();
    ok &= check(accounts_work(before, after, root_work), "root CPU uses independently checked microseconds");
    before = after;
    Descendant_fixture fixture;
    const auto setup_start = process_cpu_usec();
    if (!check(fixture.start(), "native child and grandchild fixture starts")) {
        return 1;
    }
    after = tracker.cpu_time_usec();
    const auto setup_cpu = process_cpu_usec() - setup_start;
    ok &= check(before && after && *after >= *before && fixture.initial_cpu_usec() > 0 &&
            *after - *before <= setup_cpu + fixture.initial_cpu_usec() / 4,
        "late discovery baselines old descendant work without a CPU spike");
    before = after;
    const auto descendant_work = fixture.work();
    after = tracker.cpu_time_usec();
    ok &= check(accounts_work(before, after, descendant_work), "idle root includes its busy grandchild once");
    ok &= check(fixture.retire_intermediate(), "intermediate exit reparents the known grandchild");
    before = tracker.cpu_time_usec();
    const auto reparented_work = fixture.work();
    after = tracker.cpu_time_usec();
    ok &= check(accounts_work(before, after, reparented_work), "known descendant remains counted after reparenting");
    ok &= check(fixture.retire_grandchild(), "descendant exits through its command pipe");
    const auto retired = tracker.cpu_time_usec();
    ok &= check(after && retired && *retired >= *after, "exited descendants retain their credited CPU");
    tracker.reset();
    ok &= check(!tracker.cpu_time_usec(), "reset retires the observed family");
    std::puts(ok ? "PASS: Darwin observed family CPU" : "FAIL: Darwin observed family CPU");
    return ok ? 0 : 1;
}
