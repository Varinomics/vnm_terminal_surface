#include <vnm_process_custody/process_tree.h>

#include <vnm_process_custody/process_clock.h>

#include <fcntl.h>
#include <libproc.h>
#include <mach/mach_time.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <map>

namespace vnm::process_custody {
namespace {

constexpr auto k_scope = Cleanup_scope::OWNED_PROCESS_GROUPS;
constexpr int k_group_probe_ms = 20;

// Bound discovery even if processes keep forking during a sample. Reaching
// the bound is unavailable, rather than a silently truncated family sample.
constexpr std::size_t k_max_cpu_members = 262144;

struct cpu_observation_t
{
    pid_t process_id = 0;
    pid_t parent_id = 0;
    std::uint64_t birth_token = 0;
    std::uint64_t cpu_ticks = 0;
};

std::uint64_t birth_token(const proc_bsdinfo& info)
{
    return info.pbi_start_tvsec * 1'000'000ULL + info.pbi_start_tvusec;
}

bool read_cpu_observation(pid_t process_id, cpu_observation_t* observation)
{
    struct proc_taskallinfo info{};
    errno = 0;
    if (proc_pidinfo(process_id, PROC_PIDTASKALLINFO, 0, &info, sizeof(info)) != sizeof(info)) {
        return false;
    }
    if (info.pbsd.pbi_pid != static_cast<std::uint32_t>(process_id) || birth_token(info.pbsd) == 0 ||
        info.ptinfo.pti_total_user > std::numeric_limits<std::uint64_t>::max() - info.ptinfo.pti_total_system)
    {
        errno = EINVAL;
        return false;
    }
    *observation = {process_id, static_cast<pid_t>(info.pbsd.pbi_ppid), birth_token(info.pbsd),
        info.ptinfo.pti_total_user + info.ptinfo.pti_total_system};
    return true;
}

bool list_cpu_children(pid_t parent, std::vector<pid_t>* children)
{
    children->resize(64);
    while (true) {
        errno = 0;
        // libproc returns a PID count here, although buffersize is in bytes.
        // Its underlying proc_listpids returns zero on error, as well as for
        // an empty result; clearing errno distinguishes those two cases.
        const int count = proc_listchildpids(parent, children->data(),
            static_cast<int>(children->size() * sizeof(pid_t)));
        if (count < 0 || (count == 0 && errno != 0)) {
            return false;
        }
        if (static_cast<std::size_t>(count) < children->size()) {
            children->resize(static_cast<std::size_t>(count));
            return true;
        }
        if (children->size() >= k_max_cpu_members) {
            return false;
        }
        children->resize(children->size() * 2);
    }
}

int observe_child(pid_t pid, siginfo_t* info)
{
    *info = {};
    int result;
    do {
        result = waitid(P_PID, (id_t)pid, info, WEXITED | WNOHANG | WNOWAIT);
    }
    while (result < 0 && errno == EINTR);
    return result;
}

Exit_status exit_status(const siginfo_t& info)
{
    Exit_status status;
    status.process_id = info.si_pid;
    if (info.si_code == CLD_EXITED) {
        status.exit_code = info.si_status;
    }
    else {
        status.signal = info.si_status;
    }
    return status;
}

} // namespace

struct Process_cpu_tracker::Impl
{
    struct member_t
    {
        std::uint64_t birth_token = 0;
        std::optional<std::uint64_t> cpu_ticks;
    };

    std::map<pid_t, member_t> members;
    pid_t root = 0;
    std::uint64_t root_birth_token = 0;
    std::uint64_t observed_ticks = 0;
    bool overflowed = false;
};

Process_cpu_tracker::Process_cpu_tracker()
:
    m_impl(std::make_unique<Impl>())
{}
Process_cpu_tracker::~Process_cpu_tracker() = default;

void Process_cpu_tracker::set_root(std::int64_t process_id, std::uint64_t root_birth_token) noexcept
{
    reset();
    if (process_id > 1 && process_id <= std::numeric_limits<pid_t>::max() && root_birth_token != 0) {
        m_impl->root = static_cast<pid_t>(process_id);
        m_impl->root_birth_token = root_birth_token;
    }
}

void Process_cpu_tracker::reset() noexcept
{
    m_impl->members.clear();
    m_impl->root = 0;
    m_impl->root_birth_token = 0;
    m_impl->observed_ticks = 0;
    m_impl->overflowed = false;
}

std::optional<std::uint64_t> Process_cpu_tracker::cpu_time_usec()
{
    if (m_impl->root > 1 && m_impl->members.count(m_impl->root) == 0) {
        m_impl->members.emplace(m_impl->root, Impl::member_t{m_impl->root_birth_token, std::nullopt});
    }
    std::map<pid_t, Impl::member_t> next_members;
    std::vector<cpu_observation_t> observations;
    auto observed_ticks = m_impl->observed_ticks;
    bool overflowed = m_impl->overflowed;
    bool available = true;
    for (const auto& [process_id, member] : m_impl->members) {
        cpu_observation_t current;
        if (!read_cpu_observation(process_id, &current)) {
            if (errno != ESRCH) {
                // A transient read failure retains the identity for retry;
                // confirmed disappearance and PID reuse retire it below.
                next_members.emplace(process_id, member);
                available = false;
            }
            continue;
        }
        if (current.birth_token != member.birth_token) {
            continue;
        }
        if (member.cpu_ticks) {
            if (current.cpu_ticks < *member.cpu_ticks) {
                available = false;
            }
            else {
                const auto delta = current.cpu_ticks - *member.cpu_ticks;
                if (delta > std::numeric_limits<std::uint64_t>::max() - observed_ticks) {
                    overflowed = true;
                }
                else {
                    observed_ticks += delta;
                }
            }
        }
        next_members.emplace(process_id, Impl::member_t{current.birth_token, current.cpu_ticks});
        observations.push_back(current);
    }

    for (std::size_t index = 0; index < observations.size(); ++index) {
        const auto parent = observations[index];
        std::vector<pid_t> children;
        if (!list_cpu_children(parent.process_id, &children)) {
            available = false;
            continue;
        }
        std::vector<cpu_observation_t> candidates;
        for (pid_t child : children) {
            if (child <= 1 || next_members.count(child) != 0) {
                continue;
            }
            cpu_observation_t current;
            if (!read_cpu_observation(child, &current)) {
                if (errno != ESRCH) {
                    available = false;
                }
                continue;
            }
            if (current.parent_id == parent.process_id && current.birth_token >= parent.birth_token) {
                candidates.push_back(current);
            }
        }
        // The parent can exit or its PID can be reused during enumeration.
        // Admit new children only while both sides of that relationship are
        // observed against the same parent occurrence.
        struct proc_bsdinfo parent_info{};
        errno = 0;
        if (proc_pidinfo(parent.process_id, PROC_PIDTBSDINFO, 0, &parent_info, sizeof(parent_info)) !=
            sizeof(parent_info))
        {
            if (errno != ESRCH) {
                available = false;
            }
            continue;
        }
        if (parent_info.pbi_pid != static_cast<std::uint32_t>(parent.process_id) ||
            birth_token(parent_info) != parent.birth_token)
        {
            continue;
        }
        for (const auto& child : candidates) {
            if (next_members.size() >= k_max_cpu_members) {
                available = false;
                break;
            }
            // Baseline newly seen processes. Adding their lifetime counters
            // would attribute old work to the current sampling interval.
            if (next_members.emplace(child.process_id,
                Impl::member_t{child.birth_token, child.cpu_ticks}).second)
            {
                observations.push_back(child);
            }
        }
    }
    m_impl->members = std::move(next_members);
    m_impl->observed_ticks = observed_ticks;
    m_impl->overflowed = overflowed;
    if (!available || observations.empty() || m_impl->overflowed) {
        return std::nullopt;
    }
    // PROC_PIDTASKALLINFO reports Mach absolute ticks, including terminated
    // threads. The timebase differs between Intel and Apple silicon.
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.denom == 0) {
        return std::nullopt;
    }
    const auto microseconds = static_cast<__uint128_t>(m_impl->observed_ticks) * timebase.numer /
        (static_cast<std::uint64_t>(timebase.denom) * 1000);
    if (microseconds > std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(microseconds);
}

struct Process_tree_containment::Impl {};

Process_tree_containment::Process_tree_containment()
:
    m_impl(std::make_unique<Impl>())
{}
Process_tree_containment::~Process_tree_containment() = default;

bool Process_tree_containment::establish(const std::string&, std::string* error)
{
    if (error) {
        *error = "Darwin has no independent generation containment; final owner loss leaves cleanup unconfirmed";
    }
    return false;
}
bool Process_tree_containment::available() const { return false; }
void Process_tree_containment::configure_spawn(Spawn_request&) const {}
void Process_tree_containment::release() {}
bool Process_tree_containment::terminate_and_observe_empty(std::chrono::milliseconds, std::string* error)
{
    return establish({}, error);
}

struct Process_tree_owner::Impl
{
    pid_t root = 0;
    int observer = -1;
    bool root_reported = false;
    bool root_reaped = false;
    bool ownership_lost = false;
    bool terminal_unconfirmed = false;
    bool pty = false;
    bool pty_settled = false;
    int master = -1;
    int slave = -1;
    std::uint32_t tty_device = 0;
    pid_t owner = 0;
    std::vector<Exit_status> collected_exits;
    std::string error;

    bool attempt_tty_signal(int signal_number)
    {
        // Only discovery uses numeric identities. TIOCSPGRP checks membership
        // in this retained controlling session and retains the selected group;
        // TIOCSIG acts through that kernel reference, never a saved group ID.
        if (terminal_unconfirmed || pty_settled || getsid(0) != owner || getpgrp() != owner) {
            error = "retained PTY owner session is unavailable";
            return false;
        }
        // TIOCSIG otherwise discards unread output, even for an exited root's
        // retained group. Preserve it during cleanup; running terminals keep
        // their workload-selected flushing policy for interactive signals.
        termios attributes{};
        bool preserved = tcgetattr(slave, &attributes) == 0;
        if (preserved && (attributes.c_lflag & NOFLSH) == 0) {
            attributes.c_lflag |= NOFLSH;
            preserved = tcsetattr(slave, TCSANOW, &attributes) == 0;
        }
        if (!preserved) {
            const int action_error = errno;
            error = "PTY cleanup output preservation failed (errno " + std::to_string(action_error) +
                "): " + std::strerror(action_error);
        }
        // The direct-child wait pins the root independently of tty discovery.
        // Even a discovery failure must not suppress this remaining authority.
        if (!root_reaped) {
            siginfo_t info{};
            if (!observe(&info)) {
                return false;
            }
            if (info.si_pid == 0 && kill(root, signal_number) != 0 && errno != ESRCH) {
                const int action_error = errno;
                error = "original PTY root signal failed (errno " + std::to_string(action_error) +
                    "): " + std::strerror(action_error);
            }
        }
        std::vector<pid_t> members(256);
        int copied = -1;
        while (members.size() <= 262144) {
            const int capacity = static_cast<int>(members.size() * sizeof(pid_t));
            copied = proc_listpids(PROC_TTY_ONLY, tty_device, members.data(), capacity);
            if (copied <= 0 || copied % sizeof(pid_t) != 0) {
                error = "PTY membership discovery failed";
                return true;
            }
            if (copied < capacity) {
                break;
            }
            members.resize(members.size() * 2);
        }
        if (members.size() > 262144) {
            error = "PTY membership discovery exceeded its bounded capacity";
            return true;
        }
        std::vector<pid_t> groups;
        for (std::size_t index = 0; index < static_cast<std::size_t>(copied) / sizeof(pid_t); ++index) {
            if (members[index] <= 0 || members[index] == owner) {
                continue;
            }
            struct proc_bsdinfo info{};
            // Include zombies for candidate discovery; their state never proves
            // group emptiness. A raced or rejected candidate remains unknown.
            if (proc_pidinfo(members[index], PROC_PIDTBSDINFO, 1, &info, sizeof(info)) != sizeof(info)) {
                error = "PTY member metadata became unavailable";
                continue;
            }
            const pid_t group = static_cast<pid_t>(info.pbi_pgid);
            if (group <= 0 || group == owner || std::find(groups.begin(), groups.end(), group) != groups.end()) {
                continue;
            }
            groups.push_back(group);
        }
        for (pid_t group : groups) {
            if (ioctl(slave, TIOCSPGRP, &group) != 0 || ioctl(master, TIOCSIG, signal_number) != 0) {
                const int action_error = errno;
                error = "PTY group action failed (errno " + std::to_string(action_error) +
                    "): " + std::strerror(action_error);
            }
        }
        return true;
    }

    bool observe(siginfo_t* info)
    {
        if (root <= 0 || root_reaped || ownership_lost) {
            return false;
        }
        if (observe_child(root, info) == 0) {
            return true;
        }
        error = std::string("original child observation failed: ") + std::strerror(errno);
        if (errno == ECHILD) {
            ownership_lost = true;
        }
        return false;
    }

    bool attempt_group_signal(int signal_number)
    {
        if (pty) {
            return attempt_tty_signal(signal_number);
        }
        siginfo_t info{};
        if (!observe(&info)) {
            return false;
        }
        // Only this thread waits this direct child. Keeping its zombie pins
        // both the numeric root and original group until group settlement.
        if (kill(-root, signal_number) != 0 && errno != ESRCH) {
            const int signal_error = errno;
            error = "original process group signal failed (errno " + std::to_string(signal_error) +
                "): " + std::strerror(signal_error);
        }
        if (info.si_pid == 0 && kill(root, signal_number) != 0 && errno != ESRCH) {
            const int signal_error = errno;
            error = "original child signal failed (errno " + std::to_string(signal_error) +
                "): " + std::strerror(signal_error);
        }
        // The attempt had sole-parent authority; a failed syscall remains an
        // error. Darwin can reject a group containing only zombies with EPERM.
        // That cannot prove settlement, but must not prevent bounded negative
        // retirement once the exact root has ended and its wait is collected.
        return true;
    }

    bool collect_root(const siginfo_t& info, std::vector<Exit_status>& exits)
    {
        // A final observation can discover the exit after reap_available() ran.
        // Preserve that exact status before retiring the only workload wait.
        if (!root_reported) {
            exits.push_back(exit_status(info));
            root_reported = true;
        }
        int status;
        pid_t waited;
        do {
            waited = waitpid(root, &status, WNOHANG);
        }
        while (waited < 0 && errno == EINTR);
        if (waited != root) {
            error = "original root reap was not confirmed";
            ownership_lost = waited < 0 && errno == ECHILD;
            return false;
        }
        root_reaped = true;
        close_native_handle(observer);
        observer = -1;
        return true;
    }

    bool settle_group(std::vector<Exit_status>& exits)
    {
        if (pty) {
            if (!root_reaped) {
                siginfo_t info{};
                if (!observe(&info) || info.si_pid != root || !collect_root(info, exits)) {
                    return false;
                }
            }
            // This fixed two-slot query is settlement only, never discovery.
            // A single original owner is necessarily nontruncated. The owner
            // has permanently closed root admission before this query and is
            // the only remaining scoped process that could create a child.
            pid_t members[2]{};
            const int copied = proc_listpids(PROC_TTY_ONLY, tty_device, members, sizeof(members));
            if (copied == sizeof(pid_t) && members[0] == owner && getsid(0) == owner) {
                pty_settled = true;
                error.clear();
                return true;
            }
            if (copied != sizeof(members)) {
                error = "PTY only-owner settlement was not established";
            }
            return false;
        }
        siginfo_t info{};
        if (!observe(&info) || info.si_pid != root) {
            return false;
        }
        const int required = proc_listpids(PROC_PGRP_ONLY, (std::uint32_t)root, nullptr, 0);
        if (required <= 0 || required > 1024 * 1024) {
            error = "original process group membership could not be bounded";
            return false;
        }
        std::vector<pid_t> members((std::size_t)required / sizeof(pid_t) + 32);
        const int capacity = (int)(members.size() * sizeof(pid_t));
        const int copied = proc_listpids(PROC_PGRP_ONLY, (std::uint32_t)root, members.data(), capacity);
        if (copied <= 0 || copied >= capacity || copied % sizeof(pid_t) != 0) {
            error = "original process group membership observation failed or was truncated";
            return false;
        }
        bool found_root = false;
        for (std::size_t index = 0; index < (std::size_t)copied / sizeof(pid_t); ++index) {
            const pid_t member = members[index];
            if (member == root) {
                found_root = true;
            }
            else
            if (member != 0) {
                return false;
            }
        }
        // Do not infer emptiness from individual PID probes: a living member
        // could fork between those probes. The membership snapshot must name
        // only the retained zombie, so no remaining group member can fork.
        if (!found_root) {
            error = "original root missing from its retained process group";
            return false;
        }
        if (!collect_root(info, exits)) {
            return false;
        }
        error.clear();
        return true;
    }

    void retire_exhausted_group(std::vector<Exit_status>& exits)
    {
        siginfo_t info{};
        if (!(pty && root_reaped) &&
            (!observe(&info) || info.si_pid != root || !collect_root(info, exits))) {
            return;
        }
        // A dead member held by another parent can keep the group nonempty
        // indefinitely. Exhaustion retires our authority, never proves empty.
        terminal_unconfirmed = true;
        if (error.empty()) {
            error = "original process group cleanup exhausted; scoped settlement is unconfirmed";
        }
    }
};

Process_tree_owner::Process_tree_owner()
:
    m_impl(std::make_unique<Impl>())
{}
Process_tree_owner::~Process_tree_owner()
{
    close_native_handle(m_impl->observer);
}

bool Process_tree_owner::enable_subreaper() { return false; }
bool Process_tree_owner::establish(std::string*) { return true; }

bool Process_tree_owner::establish_pty(Native_handle master, Native_handle slave, std::string* error)
{
    struct stat info{};
    struct sigaction action{};
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    const pid_t owner = getpid();
    // Retiring the controlling terminal sends SIGHUP to its session leader.
    // The owner must survive its own teardown to return the typed outcome;
    // spawn() restores default signal dispositions for the workload.
    if (m_impl->root != 0 || m_impl->pty || master < 0 || slave < 0 ||
        (getsid(0) != owner && setsid() < 0) || sigaction(SIGHUP, &action, nullptr) != 0 ||
        ioctl(slave, TIOCSCTTY, 0) != 0 ||
        sigaction(SIGTTOU, &action, nullptr) != 0 || fstat(slave, &info) != 0)
    {
        if (error) {
            *error = "persistent owner could not retain its controlling PTY";
        }
        return false;
    }
    m_impl->pty = true;
    m_impl->master = master;
    m_impl->slave = slave;
    m_impl->tty_device = static_cast<std::uint32_t>(info.st_rdev);
    m_impl->owner = owner;
    return true;
}

void Process_tree_owner::configure_root_spawn(Spawn_request& request) const
{
    request.new_session = !m_impl->pty;
    if (m_impl->pty) {
        request.new_process_group = true;
        request.controlling_terminal = false;
        request.foreground_terminal = true;
    }
}

void Process_tree_owner::record_root(const Spawn_result& root)
{
    m_impl->root = (pid_t)root.process_id;
    m_impl->observer = root.process_handle;
}

void Process_tree_owner::record_failed_root(const Darwin_spawn_failure& failure)
{
    m_impl->root = static_cast<pid_t>(failure.process_id);
    m_impl->observer = failure.process_handle;
    if (failure.wait_collected) {
        m_impl->root_reaped = true;
        m_impl->root_reported = true;
        Exit_status status;
        status.process_id = failure.process_id;
        if (WIFEXITED(failure.wait_status)) {
            status.exit_code = WEXITSTATUS(failure.wait_status);
        }
        else
        if (WIFSIGNALED(failure.wait_status)) {
            status.signal = WTERMSIG(failure.wait_status);
        }
        m_impl->collected_exits.push_back(status);
    }
}

bool Process_tree_owner::record_descendant(std::int64_t, Native_handle transferred_handle)
{
    // Darwin cannot adopt a supervisor's child. A transferred observer cannot
    // manufacture wait or signal authority over that nonchild.
    close_native_handle(transferred_handle);
    return false;
}

Kill_coordinates Process_tree_owner::coordinates() const
{
    Kill_coordinates result;
    result.cleanup_scope = k_scope;
    if (m_impl->root > 0 && !m_impl->root_reaped) {
        result.process_group = m_impl->root;
        result.process_ids.push_back(m_impl->root);
    }
    return result;
}

std::vector<Native_handle> Process_tree_owner::exit_wait_handles() const
{
    return m_impl->observer >= 0 ? std::vector<Native_handle>{m_impl->observer} : std::vector<Native_handle>{};
}

std::vector<Exit_status> Process_tree_owner::reap_available()
{
    if (!m_impl->collected_exits.empty()) {
        return std::exchange(m_impl->collected_exits, {});
    }
    siginfo_t info{};
    if (!m_impl->root_reported && m_impl->observe(&info) && info.si_pid == m_impl->root) {
        m_impl->root_reported = true;
        return {exit_status(info)};
    }
    return {};
}

bool Process_tree_owner::children_remain() const
{
    if (m_impl->pty && m_impl->root > 0) {
        return !m_impl->pty_settled && !m_impl->terminal_unconfirmed;
    }
    return m_impl->root > 0 && !m_impl->root_reaped;
}

Reap_outcome Process_tree_owner::terminate_and_reap(
    std::chrono::milliseconds grace, std::chrono::milliseconds reap_budget)
{
    Reap_outcome outcome;
    outcome.cleanup_scope = k_scope;
    // A failed spawn may have collected the exact root wait already. Deliver
    // its queued receipt even when no child remains for the cleanup loop.
    outcome.exits = reap_available();
    if (m_impl->terminal_unconfirmed) {
        outcome.terminal_unconfirmed = true;
        outcome.remaining_children.push_back(m_impl->root);
        outcome.error = m_impl->error;
        return outcome;
    }
    const auto started = Deadline::Clock::now();
    outcome.escalated_to_kill = grace.count() <= 0;
    int signal_number = outcome.escalated_to_kill ? SIGKILL : SIGTERM;
    auto deadline = Deadline::after(outcome.escalated_to_kill ? reap_budget : grace);
    bool attempted = m_impl->attempt_group_signal(signal_number);
    while (children_remain()) {
        const auto exits = reap_available();
        outcome.exits.insert(outcome.exits.end(), exits.begin(), exits.end());
        if (m_impl->settle_group(outcome.exits) || m_impl->ownership_lost) {
            break;
        }
        if (deadline.expired()) {
            if (outcome.escalated_to_kill) {
                if (attempted) {
                    m_impl->retire_exhausted_group(outcome.exits);
                }
                break;
            }
            outcome.escalated_to_kill = true;
            signal_number = SIGKILL;
            attempted = m_impl->attempt_group_signal(signal_number);
            deadline = Deadline::after(reap_budget);
        }
        else
        if (m_impl->pty) {
            // Re-discover every pass, including groups born during grace.
            // A discovery error stays negative and never becomes emptiness.
            attempted = m_impl->attempt_group_signal(signal_number) || attempted;
        }
        // Darwin has no event for arbitrary original-group membership. The
        // bounded probe is required after the root's NOTE_EXIT has fired.
        pollfd descriptor{m_impl->root_reported ? -1 : m_impl->observer, POLLIN, 0};
        (void)poll(&descriptor, 1, Deadline::earlier_timeout_ms(deadline.remaining_ms(), k_group_probe_ms));
    }
    outcome.terminal_unconfirmed = m_impl->terminal_unconfirmed;
    outcome.complete = !children_remain() && !m_impl->ownership_lost && !outcome.terminal_unconfirmed;
    outcome.error = m_impl->error;
    if (!outcome.complete) {
        outcome.remaining_children.push_back(m_impl->root);
        if (outcome.error.empty()) {
            outcome.error = "original process group cleanup is pending; detached descendants are outside this scope";
        }
    }
    outcome.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Deadline::Clock::now() - started);
    return outcome;
}

bool peek_process_exit(std::int64_t process_id, Native_handle, Exit_status* status)
{
    siginfo_t info{};
    if (process_id <= 0 || observe_child((pid_t)process_id, &info) != 0 || info.si_pid != process_id) {
        return false;
    }
    *status = exit_status(info);
    return true;
}

bool terminate_coordinates(const Kill_coordinates&, std::chrono::milliseconds, std::chrono::milliseconds)
{
    // The final owner's wait cannot be transferred by passing a PID/PGID or
    // observation descriptor. No numeric fallback is safe after its loss.
    return false;
}

} // namespace vnm::process_custody
