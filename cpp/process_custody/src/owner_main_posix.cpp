#include "owner_protocol.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

namespace custody = vnm::process_custody;
namespace detail = custody::detail;
using namespace std::chrono_literals;

namespace {

bool resize_pty(int master, custody::Pty_dimensions dimensions)
{
    winsize size{};
    size.ws_row = dimensions.rows;
    size.ws_col = dimensions.columns;
    size.ws_xpixel = dimensions.pixel_width;
    size.ws_ypixel = dimensions.pixel_height;
    return ioctl(master, TIOCSWINSZ, &size) == 0;
}

int run_owner(custody::Duplex_channel& channel)
{
    // Capture inherited stdio before our signalfd, PTY or pidfds can occupy
    // vacant low slots. Only these original streams belong to the workload.
    const auto inherited_standard_streams = custody::inherit_standard_streams();
    std::string error;
    custody::Process_tree_owner tree;
    const auto emit = [&](custody::Owner_event event) {
        return detail::send_event(channel, event, &error);
    };
    const auto reject = [&](const std::string& reason) {
        custody::Owner_event event;
        event.kind = custody::Owner_event_kind::START_REJECTED;
        event.detail = reason;
        (void)emit(event);
        event.kind = custody::Owner_event_kind::TREE_SETTLEMENT;
        event.settlement = custody::Tree_settlement::CONFIRMED;
        (void)emit(event);
        return 0;
    };
    if (!tree.establish(&error)) return reject(error);
    detail::Owner_message message;
    if (detail::receive_message(channel, &message, 10000, &error) != custody::Receive_status::MESSAGE ||
        message.kind != detail::Message_kind::START)
    {
        detail::close_start_descriptors(message.start);
        return reject("owner start was not admitted");
    }
    auto& request = message.start;
    struct Request_handles
    {
        custody::Owner_start_request& request;
        ~Request_handles() { detail::close_start_descriptors(request); }
    } request_handles{request};
    if (request.process.executable.empty() || request.process.executable.front() != '/' ||
        request.process.argv.empty()) return reject("owner requires an absolute executable and argv[0]");
    for (const auto& mapping : request.process.inherited) {
        if ((request.mode == custody::Owner_mode::PTY || request.use_owner_standard_streams) &&
            mapping.child_number <= 2) return reject("conflicting standard descriptor mapping");
    }
    if (request.mode == custody::Owner_mode::PTY && request.use_owner_standard_streams)
        return reject("PTY mode cannot inherit owner standard streams");

    int master = -1;
    int slave = -1;
    struct Pty_handles
    {
        int& master;
        int& slave;
        ~Pty_handles() { if (master >= 0) close(master); if (slave >= 0) close(slave); }
    } pty_handles{master, slave};
    if (request.mode == custody::Owner_mode::PTY) {
        master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        char slave_name[256];
        if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0 ||
            ptsname_r(master, slave_name, sizeof(slave_name)) != 0 ||
            (slave = open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC)) < 0 ||
            !resize_pty(master, request.dimensions)) return reject("owner could not establish PTY");
        request.process.controlling_terminal = true;
        // The PTY guard owns the slave, not the transferred-request guard.
    }
    custody::Spawn_request spawn = request.process;
    if (request.mode == custody::Owner_mode::PTY) {
        for (int descriptor = 0; descriptor < 3; ++descriptor) spawn.inherited.push_back({slave, descriptor});
    }
    else if (request.use_owner_standard_streams) {
        for (const auto& descriptor : inherited_standard_streams) spawn.inherited.push_back(descriptor);
    }
    tree.configure_root_spawn(spawn);
    spawn.admit_execution = [&]() {
        detail::Owner_message pending;
        const auto status = detail::receive_message(channel, &pending, 0, &error);
        detail::close_start_descriptors(pending.start);
        return status == custody::Receive_status::TIMEOUT;
    };
    const auto root = custody::spawn(spawn);
    if (!root.ok) return reject(root.error);
    tree.record_root(root);
    detail::close_start_descriptors(request);
    if (slave >= 0) { close(slave); slave = -1; }
    // Only the workload keeps its stdio. In particular, the owner must not
    // keep a pipe writer alive after the root and its descendants close it.
    for (const auto& descriptor : inherited_standard_streams)
        close(descriptor.parent_handle);
    custody::Owner_event started;
    started.kind = custody::Owner_event_kind::STARTED;
    started.root_exit.process_id = root.process_id;
    started.root_reference = root.process_handle;
    started.pty_master = master;
    bool stopping = !emit(started);
    int grace_ms = 250;
    bool root_reported = false;
    const auto report = [&](const std::vector<custody::Exit_status>& exits) {
        for (const auto& exit : exits) {
            if (exit.process_id != root.process_id || root_reported) continue;
            root_reported = true;
            custody::Owner_event event;
            event.kind = custody::Owner_event_kind::ROOT_EXITED;
            event.root_exit = exit;
            (void)emit(event);
        }
    };
    while (!stopping) {
        report(tree.reap_available());
        if (root_reported) break;
        detail::Owner_message incoming;
        const auto status = detail::receive_message(channel, &incoming, 25, &error);
        if (status == custody::Receive_status::TIMEOUT) continue;
        if (status != custody::Receive_status::MESSAGE) { stopping = true; break; }
        if (incoming.kind == detail::Message_kind::RESIZE && master >= 0) {
            if (!resize_pty(master, incoming.dimensions)) stopping = true;
        }
        else {
            if (incoming.kind == detail::Message_kind::STOP) grace_ms = incoming.grace_ms;
            stopping = true;
        }
        detail::close_start_descriptors(incoming.start);
    }
    bool pending_reported = false;
    while (true) {
        const auto outcome = tree.terminate_and_reap(std::chrono::milliseconds(grace_ms), 1000ms);
        report(outcome.exits);
        custody::Owner_event event;
        event.kind = custody::Owner_event_kind::TREE_SETTLEMENT;
        event.settlement = outcome.complete ? custody::Tree_settlement::CONFIRMED : custody::Tree_settlement::PENDING;
        event.detail = outcome.error;
        if (outcome.complete || !pending_reported) (void)emit(event);
        if (outcome.complete) return 0;
        pending_reported = true;
        grace_ms = 0;
        std::this_thread::sleep_for(25ms);
    }
}

} // namespace

int main(int argc, char** argv)
{
    custody::Native_handle control = custody::k_invalid_handle;
    if (argc != 3 || std::string(argv[1]) != "--control" ||
        !custody::parse_native_handle_argument(argv[2], &control) || control < 3 ||
        !custody::native_handle_is_open(control)) return 2;
    std::string error;
    if (!custody::prepare_parent_process(&error)) return 2;
    custody::Duplex_channel channel(control, custody::k_owner_message_bytes);
    if (!channel.prepare_for_owner(&error)) return 2;
    return run_owner(channel);
}
