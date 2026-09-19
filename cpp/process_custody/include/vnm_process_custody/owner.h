#pragma once

#include <vnm_process_custody/process_channel.h>
#include <vnm_process_custody/process_spawn.h>
#include <vnm_process_custody/process_tree.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vnm::process_custody {

enum class Owner_mode : std::uint32_t { COMMAND, PTY };
enum class Tree_settlement : std::uint32_t { PENDING, CONFIRMED, UNCONFIRMED };
enum class Owner_event_kind : std::uint32_t { STARTED, START_REJECTED, ROOT_EXITED, TREE_SETTLEMENT };

struct Pty_dimensions
{
    std::uint16_t rows = 24;
    std::uint16_t columns = 80;
    std::uint16_t pixel_width = 0;
    std::uint16_t pixel_height = 0;
};

// argv includes argv[0]. Descriptor-map parent handles are borrowed for the
// send and transferred by SCM_RIGHTS; no numeric parent descriptor is a remote
// authority. PTY mode owns descriptors 0/1/2 and rejects conflicting mappings.
struct Owner_start_request
{
    Owner_mode mode = Owner_mode::COMMAND;
    bool use_owner_standard_streams = false; // COMMAND only; explicit use of helper's inherited 0/1/2
    Spawn_request process;
    Pty_dimensions dimensions;
};

struct Owner_event
{
    Owner_event_kind kind = Owner_event_kind::START_REJECTED;
    Exit_status root_exit;
    Tree_settlement settlement = Tree_settlement::PENDING;
    Native_handle root_reference = k_invalid_handle; // STARTED: receiver owns exact observation pidfd, not wait authority
    Native_handle pty_master = k_invalid_handle; // receiving caller owns it
    std::string detail;
};

// A control socket is separate from workload stdio/PTY. Only the designated
// launcher waits its direct owner helper; these functions never wait children.
// Launch vnm_process_custody_owner --control <inherited socket descriptor>.
// Close the launcher's duplicate helper end immediately after helper creation.
// The helper establishes its subreaper before acknowledging workload birth.
inline constexpr std::size_t k_owner_message_bytes = 64 * 1024; // socket record, not complete request
bool send_owner_start(Duplex_channel& channel, const Owner_start_request& request, std::string* error);
bool send_owner_stop(Duplex_channel& channel, int grace_ms, std::string* error);
bool send_owner_resize(Duplex_channel& channel, Pty_dimensions dimensions, std::string* error);

// MESSAGE provides one typed event. CLOSED/FAILED without CONFIRMED leaves
// settlement UNCONFIRMED; root exit, helper exit, and PTY EOF are not receipts.
// A receive timeout preserves PENDING and does not discard the living owner.
Receive_status receive_owner_event(Duplex_channel& channel, Owner_event* event,
    int timeout_ms, std::string* error);

} // namespace vnm::process_custody
