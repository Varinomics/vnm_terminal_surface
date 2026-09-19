#pragma once

#include <vnm_process_custody/owner.h>

namespace vnm::process_custody::detail {

enum class Message_kind : std::uint32_t { START = 1, STOP, RESIZE, EVENT };

struct Owner_message
{
    Message_kind kind = Message_kind::STOP;
    Owner_start_request start;
    Owner_event event;
    int grace_ms = 0;
    Pty_dimensions dimensions;
};

Receive_status receive_message(Duplex_channel& channel, Owner_message* message,
    int timeout_ms, std::string* error);
bool send_event(Duplex_channel& channel, const Owner_event& event, std::string* error);
void close_start_descriptors(Owner_start_request& request);

} // namespace vnm::process_custody::detail
