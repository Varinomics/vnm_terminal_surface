#include <vnm_process_custody/process_tree.h>

#include <vnm_process_custody/process_clock.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <fstream>
#include <sstream>
#include <thread>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>

namespace vnm::process_custody {

namespace {

// SIGCHLD reports only a direct child's exit. A descendant reparented to this
// subreaper when an intermediate parent outside the direct children exits
// arrives without one, so the wait for the tree is bounded by this tick.
constexpr int k_child_probe_tick_ms = 50;

} // namespace


namespace {

// proc mountinfo escapes whitespace and backslashes with octal sequences.
std::string unescape_mount_path(const std::string& text)
{
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 3 < text.size() &&
            text[i + 1] >= '0' && text[i + 1] <= '7' &&
            text[i + 2] >= '0' && text[i + 2] <= '7' &&
            text[i + 3] >= '0' && text[i + 3] <= '7')
        {
            result.push_back(static_cast<char>((text[i + 1] - '0') * 64 +
                (text[i + 2] - '0') * 8 + text[i + 3] - '0'));
            i += 3;
        }
        else result.push_back(text[i]);
    }
    return result;
}

bool read_control_file(int directory, const char* name, std::string* text)
{
    const int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    text->clear();
    bool complete = false;
    char buffer[512];
    for (;;) {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) break;
        if (count == 0) { complete = true; break; }
        if (text->size() + static_cast<std::size_t>(count) > 65536) break;
        text->append(buffer, static_cast<std::size_t>(count));
    }
    close(fd);
    return complete;
}

bool cgroup_empty(int directory)
{
    std::string text;
    if (!read_control_file(directory, "cgroup.events", &text)) return false;
    std::istringstream lines(text);
    std::string line;
    bool found = false;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string key, value, extra;
        if (!(fields >> key >> value) || (fields >> extra)) return false;
        if (key == "populated") {
            if (found || value != "0") return false;
            found = true;
        }
    }
    return found;
}

bool kill_cgroup(int directory)
{
    const int fd = openat(directory, "cgroup.kill", O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    ssize_t count;
    do { count = write(fd, "1\n", 2); } while (count < 0 && errno == EINTR);
    const int saved = errno;
    close(fd);
    errno = saved;
    return count == 2;
}

std::string delegated_cgroup_directory()
{
    std::ifstream membership("/proc/self/cgroup");
    std::string line, relative;
    while (std::getline(membership, line)) {
        if (line.compare(0, 3, "0::") == 0) relative = line.substr(3);
    }
    if (membership.bad() || relative.empty() || relative.front() != '/') return {};
    // A namespace-relative path outside the mounted hierarchy is not a
    // delegated generation authority. Never resolve parent traversal.
    std::istringstream components(relative);
    std::string component;
    while (std::getline(components, component, '/'))
        if (component == "." || component == "..") return {};
    std::ifstream mounts("/proc/self/mountinfo");
    while (std::getline(mounts, line)) {
        const auto separator = line.find(" - cgroup2 ");
        if (separator == std::string::npos) continue;
        std::istringstream fields(line.substr(0, separator));
        std::string id, parent, device, mount_root, mount_point;
        if (!(fields >> id >> parent >> device >> mount_root >> mount_point)) continue;
        mount_root = unescape_mount_path(mount_root);
        mount_point = unescape_mount_path(mount_point);
        if (relative == mount_root) return mount_point;
        if (mount_root == "/") return mount_point + relative;
        if (relative.compare(0, mount_root.size() + 1, mount_root + "/") == 0)
            return mount_point + relative.substr(mount_root.size());
    }
    return {};
}

} // namespace

struct Process_tree_containment::Impl
{
    int parent = -1;
    int directory = -1;
    std::string name;
};

Process_tree_containment::Process_tree_containment() : m_impl(std::make_unique<Impl>()) {}
Process_tree_containment::~Process_tree_containment()
{
    // This is only last-resort containment, never a completion receipt.
    if (available()) (void)kill_cgroup(m_impl->directory);
    release();
}

bool Process_tree_containment::establish(const std::string& nonce, std::string* error)
{
    if (available()) { if (error) *error = "generation containment already established"; return false; }
    if (nonce.empty() || nonce.size() > 128 ||
        nonce.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
            std::string::npos)
    {
        if (error) *error = "invalid generation containment nonce";
        return false;
    }
    const std::string parent_path = delegated_cgroup_directory();
    if (parent_path.empty()) {
        if (error) *error = "no accessible cgroup v2 membership mount";
        return false;
    }
    m_impl->parent = open(parent_path.c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    m_impl->name = "logonomic-generation-" + nonce;
    if (m_impl->parent < 0 || mkdirat(m_impl->parent, m_impl->name.c_str(), 0700) != 0) {
        if (error) *error = std::string("generation cgroup delegation unavailable: ") + std::strerror(errno);
        // Never remove a pre-existing directory whose name collided.
        m_impl->name.clear();
        release();
        return false;
    }
    m_impl->directory = openat(m_impl->parent, m_impl->name.c_str(),
        O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    const int control = m_impl->directory < 0 ? -1 : openat(m_impl->directory,
        "cgroup.kill", O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (control < 0 || !cgroup_empty(m_impl->directory)) {
        if (control >= 0) close(control);
        if (error) *error = "generation cgroup kill/emptiness authority unavailable";
        release();
        return false;
    }
    close(control);
    return true;
}

bool Process_tree_containment::available() const { return m_impl->directory >= 0; }
void Process_tree_containment::configure_spawn(Spawn_request& request) const
{
    request.generation_cgroup = m_impl->directory;
}

bool Process_tree_containment::terminate_and_observe_empty(
    std::chrono::milliseconds budget, std::string* error)
{
    if (!available()) { if (error) *error = "generation containment unavailable"; return false; }
    const Deadline deadline = Deadline::after(budget);
    do {
        if (cgroup_empty(m_impl->directory)) return true;
        if (!kill_cgroup(m_impl->directory) && error)
            *error = std::string("cgroup.kill: ") + std::strerror(errno);
        if (deadline.expired()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (true);
    if (cgroup_empty(m_impl->directory)) return true;
    if (error && error->empty()) *error = "generation cgroup emptiness unconfirmed";
    return false;
}

void Process_tree_containment::release()
{
    if (m_impl->directory >= 0) { close(m_impl->directory); m_impl->directory = -1; }
    if (m_impl->parent >= 0) {
        if (!m_impl->name.empty()) (void)unlinkat(m_impl->parent, m_impl->name.c_str(), AT_REMOVEDIR);
        close(m_impl->parent);
        m_impl->parent = -1;
    }
    m_impl->name.clear();
}

struct Process_tree_owner::Impl
{
    int                        signal_fd     = -1;
    std::int64_t               process_group = 0;
    bool                       root_unreaped = false;
    std::vector<std::int64_t>  process_ids;       // recorded and acquired processes not yet reaped here
    std::vector<Native_handle> process_handles;   // their pidfds, same order

    // The root's number names the tree's original process group only while
    // the root is unreaped: this owner is its sole waiter, so until this
    // owner reaps it the number cannot be reused. Afterwards a group with
    // that number may be unrelated, and only the pidfds carry the signal.
    void signal_tree(int in_signal) const
    {
        for (const int pidfd : process_handles) {
            (void)posix_pidfd_send_signal(pidfd, in_signal);
        }
        if (root_unreaped) {
            (void)kill(static_cast<pid_t>(-process_group), in_signal);
        }
    }

    void forget_reaped(std::int64_t in_process_id)
    {
        if (in_process_id == process_group) {
            root_unreaped = false;
        }
        const auto found = std::find(process_ids.begin(), process_ids.end(), in_process_id);
        if (found == process_ids.end()) {
            return;
        }
        const auto handle = process_handles.begin() + (found - process_ids.begin());
        (void)close(*handle);
        process_handles.erase(handle);
        process_ids.erase(found);
    }

    // Every living descendant is either a direct child of this subreaper or
    // below a living parent in the tree, so acquiring the direct children on
    // each pass reaches a descendant that left the root's session or group.
    // A /proc name only proposes a candidate: the non-consuming waitid proves
    // it is an unreaped child of this process, whose number nothing but this
    // owner's own reap can release, so the pidfd opened next is that child.
    // Each child acquired receives in_signal; a failure is left in out_error.
    void acquire_children(int in_signal, std::string* out_error)
    {
        DIR* const directory = opendir("/proc");
        if (!directory) {
            *out_error = std::string("opendir(/proc): ") + std::strerror(errno);
            return;
        }
        while (true) {
            errno = 0;
            const dirent* const entry = readdir(directory);
            if (!entry) {
                if (errno != 0) {
                    *out_error = std::string("readdir(/proc): ") + std::strerror(errno);
                }
                break;
            }
            int pid = 0;
            const char* const name_end = entry->d_name + std::strlen(entry->d_name);
            const auto parsed = std::from_chars(entry->d_name, name_end, pid);
            if (parsed.ec != std::errc() || parsed.ptr != name_end ||
                std::find(process_ids.begin(), process_ids.end(), pid) != process_ids.end())
            {
                continue;
            }
            siginfo_t info{};
            int result = 0;
            do {
                result = waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT);
            }
            while (result < 0 && errno == EINTR);
            if (result < 0) {
                continue;   // ECHILD: not a child of this process
            }
            const int pidfd = posix_pidfd_open(pid);
            if (pidfd < 0) {
                *out_error = "pidfd_open(child " + std::to_string(pid) + "): " + std::strerror(errno);
                continue;
            }
            process_ids.push_back(pid);
            process_handles.push_back(pidfd);
            (void)posix_pidfd_send_signal(pidfd, in_signal);
        }
        (void)closedir(directory);
    }

    void drain_signal_fd()
    {
        if (signal_fd < 0) {
            return;
        }
        signalfd_siginfo info;
        while (read(signal_fd, &info, sizeof(info)) == static_cast<ssize_t>(sizeof(info))) {
        }
    }

    // Waits for SIGCHLD or the tick, whichever first, bounded by the deadline.
    void wait_for_child_event(const Deadline& deadline)
    {
        pollfd descriptor{};
        descriptor.fd     = signal_fd;
        descriptor.events = POLLIN;
        const int timeout = Deadline::earlier_timeout_ms(deadline.remaining_ms(), k_child_probe_tick_ms);
        (void)poll(&descriptor, signal_fd >= 0 ? 1 : 0, timeout);
        drain_signal_fd();
    }
};

Process_tree_owner::Process_tree_owner()
:
    m_impl(std::make_unique<Impl>())
{}

Process_tree_owner::~Process_tree_owner()
{
    for (const int pidfd : m_impl->process_handles) {
        (void)close(pidfd);
    }
    if (m_impl->signal_fd >= 0) {
        (void)close(m_impl->signal_fd);
    }
}

bool Process_tree_owner::enable_subreaper()
{
    return prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == 0;
}

bool Process_tree_owner::establish(std::string* out_error)
{
    if (!enable_subreaper()) {
        *out_error = std::string("prctl(PR_SET_CHILD_SUBREAPER): ") + std::strerror(errno);
        return false;
    }
    sigset_t child_signal;
    sigemptyset(&child_signal);
    sigaddset(&child_signal, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &child_signal, nullptr) != 0) {
        *out_error = std::string("sigprocmask(SIGCHLD): ") + std::strerror(errno);
        return false;
    }
    m_impl->signal_fd = signalfd(-1, &child_signal, SFD_NONBLOCK | SFD_CLOEXEC);
    if (m_impl->signal_fd < 0) {
        *out_error = std::string("signalfd(SIGCHLD): ") + std::strerror(errno);
        return false;
    }
    return true;
}

void Process_tree_owner::configure_root_spawn(Spawn_request& in_out_request) const
{
    in_out_request.new_session = true;
}

void Process_tree_owner::record_root(const Spawn_result& in_root)
{
    m_impl->process_group = in_root.process_id;
    m_impl->root_unreaped = true;
    m_impl->process_ids.push_back(in_root.process_id);
    m_impl->process_handles.push_back(in_root.process_handle);
}

bool Process_tree_owner::record_descendant(std::int64_t in_process_id, Native_handle in_transferred_handle)
{
    int pidfd = in_transferred_handle;
    if (pidfd < 0) {
        pidfd = posix_pidfd_open(static_cast<int>(in_process_id));
        if (pidfd < 0) {
            return false;
        }
    }
    m_impl->process_ids.push_back(in_process_id);
    m_impl->process_handles.push_back(pidfd);
    return true;
}

Kill_coordinates Process_tree_owner::coordinates() const
{
    Kill_coordinates coordinates;
    coordinates.process_group   = m_impl->process_group;
    coordinates.process_ids     = m_impl->process_ids;
    coordinates.process_handles = m_impl->process_handles;
    return coordinates;
}

std::vector<Native_handle> Process_tree_owner::exit_wait_handles() const
{
    return {m_impl->signal_fd};
}

std::vector<Exit_status> Process_tree_owner::reap_available()
{
    m_impl->drain_signal_fd();

    std::vector<Exit_status> exits;
    while (true) {
        siginfo_t info{};
        info.si_pid = 0;
        const int result = waitid(P_ALL, 0, &info, WEXITED | WNOHANG);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;   // ECHILD: no child at all
        }
        if (info.si_pid == 0) {
            break;   // children exist, none has exited
        }
        Exit_status status;
        status.process_id = info.si_pid;
        if (info.si_code == CLD_EXITED) {
            status.exit_code = info.si_status;
        }
        else {
            status.signal = info.si_status;
        }
        exits.push_back(status);
        m_impl->forget_reaped(info.si_pid);
    }
    return exits;
}

bool Process_tree_owner::children_remain() const
{
    // WNOWAIT leaves any zombie for reap_available(); only the existence of
    // children is asked here.
    siginfo_t info{};
    int result = 0;
    do {
        result = waitid(P_ALL, 0, &info, WEXITED | WNOHANG | WNOWAIT);
    }
    while (result < 0 && errno == EINTR);
    return !(result < 0 && errno == ECHILD);
}

Reap_outcome Process_tree_owner::terminate_and_reap(
    std::chrono::milliseconds in_terminate_to_kill,
    std::chrono::milliseconds in_reap_deadline)
{
    const auto started = Deadline::Clock::now();
    Reap_outcome outcome;

    // A zero grace is the retirement kill: nothing is asked of the tree first,
    // so the statuses it leaves carry SIGKILL and nothing else.
    const bool kill_at_once = in_terminate_to_kill.count() <= 0;
    int signal_number = kill_at_once ? SIGKILL : SIGTERM;
    m_impl->signal_tree(signal_number);
    outcome.escalated_to_kill = kill_at_once;
    Deadline phase = Deadline::after(kill_at_once ? in_reap_deadline : in_terminate_to_kill);

    while (true) {
        const std::vector<Exit_status> reaped = reap_available();
        outcome.exits.insert(outcome.exits.end(), reaped.begin(), reaped.end());

        // This process is the subreaper of the whole tree, so no child left
        // to wait means no descendant is left, whatever session or group it
        // moved to.
        if (!children_remain()) {
            outcome.complete = true;
            outcome.error.clear();
            break;
        }

        outcome.error.clear();
        m_impl->acquire_children(signal_number, &outcome.error);

        if (phase.expired()) {
            if (outcome.escalated_to_kill) {
                outcome.remaining_children = m_impl->process_ids;
                break;
            }
            signal_number = SIGKILL;
            m_impl->signal_tree(signal_number);
            outcome.escalated_to_kill = true;
            phase = Deadline::after(in_reap_deadline);
            continue;
        }

        m_impl->wait_for_child_event(phase);
    }

    outcome.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Deadline::Clock::now() - started);
    return outcome;
}

bool peek_process_exit(std::int64_t in_process_id, Native_handle /*in_handle*/, Exit_status* out_status)
{
    // The supervisor is the adapter's parent, so P_PID needs no pidfd; WNOWAIT
    // reads the status without reaping, leaving the zombie for the custodian.
    siginfo_t info{};
    info.si_pid = 0;
    int result = 0;
    do {
        result = waitid(P_PID, static_cast<id_t>(in_process_id), &info, WEXITED | WNOWAIT | WNOHANG);
    }
    while (result < 0 && errno == EINTR);
    if (result < 0 || info.si_pid == 0) {
        return false;
    }
    out_status->process_id = in_process_id;
    if (info.si_code == CLD_EXITED) {
        out_status->exit_code = info.si_status;
    }
    else {
        out_status->signal = info.si_status;
    }
    return true;
}

bool terminate_coordinates(
    const Kill_coordinates&   in_coordinates,
    std::chrono::milliseconds in_terminate_to_kill,
    std::chrono::milliseconds in_kill_deadline)
{
    // The desktop reaps none of these processes, so nothing holds their
    // numbers: once the tree has ended, the recorded process group can name
    // an unrelated group. Only the recorded pidfds carry a signal, and each
    // becomes readable when its process ends.
    std::vector<pollfd> pending;
    for (const int pidfd : in_coordinates.process_handles) {
        pollfd descriptor{};
        descriptor.fd     = pidfd;
        descriptor.events = POLLIN;
        pending.push_back(descriptor);
    }
    if (pending.empty()) {
        return false;
    }
    const auto signal_pending = [&pending](int in_signal) {
        for (const pollfd& descriptor : pending) {
            (void)posix_pidfd_send_signal(descriptor.fd, in_signal);
        }
    };

    const bool kill_at_once = in_terminate_to_kill.count() <= 0;
    signal_pending(kill_at_once ? SIGKILL : SIGTERM);
    Deadline phase  = Deadline::after(kill_at_once ? in_kill_deadline : in_terminate_to_kill);
    bool     killed = kill_at_once;

    while (true) {
        const int ready = poll(pending.data(), static_cast<nfds_t>(pending.size()), phase.remaining_ms());
        if (ready < 0 && errno != EINTR) {
            return false;
        }
        pending.erase(
            std::remove_if(pending.begin(), pending.end(),
                [](const pollfd& descriptor) { return (descriptor.revents & POLLIN) != 0; }),
            pending.end());
        if (pending.empty()) {
            return true;
        }
        if (phase.expired()) {
            if (killed) {
                return false;
            }
            signal_pending(SIGKILL);
            killed = true;
            phase  = Deadline::after(in_kill_deadline);
        }
    }
}

} // namespace vnm::process_custody
