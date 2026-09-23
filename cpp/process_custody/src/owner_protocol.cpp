#include "owner_protocol.h"

#include <limits>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <optional>
#include <poll.h>

namespace vnm::process_custody {
namespace {

constexpr std::uint32_t k_wire_revision = 1;
constexpr std::size_t k_max_list_entries = 65536;
constexpr std::size_t k_max_request_bytes = 4 * 1024 * 1024;
constexpr std::uint32_t k_fragment_kind = 5;

struct Encoder
{
    std::vector<std::uint8_t> bytes;
    void number(std::uint32_t value)
    {
        for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back((value >> shift) & 255);
    }
    void text(const std::string& value)
    {
        number(static_cast<std::uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void dimensions(Pty_dimensions value)
    {
        number(value.rows); number(value.columns); number(value.pixel_width); number(value.pixel_height);
    }
};

struct Decoder
{
    const std::vector<std::uint8_t>& bytes;
    std::size_t position = 0;
    bool valid = true;
    std::uint32_t number()
    {
        if (bytes.size() - position < 4) { valid = false; return 0; }
        std::uint32_t result = 0;
        for (int i = 0; i < 4; ++i) result = (result << 8) | bytes[position++];
        return result;
    }
    std::string text()
    {
        const auto size = number();
        if (!valid || size > bytes.size() - position) { valid = false; return {}; }
        std::string result(reinterpret_cast<const char*>(bytes.data() + position), size);
        position += size;
        if (result.find('\0') != std::string::npos) valid = false;
        return result;
    }
    Pty_dimensions dimensions()
    {
        Pty_dimensions result;
        std::uint16_t* fields[] = {&result.rows, &result.columns, &result.pixel_width, &result.pixel_height};
        for (auto field : fields) {
            const auto value = number();
            if (value > 65535) valid = false;
            *field = static_cast<std::uint16_t>(value);
        }
        return result;
    }
    std::size_t count()
    {
        const auto value = number();
        if (value > k_max_list_entries) { valid = false; return 0; }
        return value;
    }
};

Encoder prefix(detail::Message_kind kind)
{
    Encoder output;
    output.number(k_wire_revision);
    output.number(static_cast<std::uint32_t>(kind));
    return output;
}

bool send(Duplex_channel& channel, const Encoder& message,
    const std::vector<Native_handle>& descriptors, std::string* error,
    std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt)
{
    int native_error = 0;
    if (message.bytes.size() > k_max_request_bytes || descriptors.size() > 64) {
        if (error) *error = "owner request exceeds transport bounds";
        return false;
    }
    bool transferred = true;
    for (std::size_t offset = 0; offset < message.bytes.size();) {
        Encoder fragment;
        fragment.number(k_wire_revision);
        fragment.number(k_fragment_kind);
        fragment.number(static_cast<std::uint32_t>(message.bytes.size()));
        fragment.number(static_cast<std::uint32_t>(offset));
        const auto count = std::min(k_owner_message_bytes - fragment.bytes.size(), message.bytes.size() - offset);
        fragment.bytes.insert(fragment.bytes.end(), message.bytes.begin() + offset, message.bytes.begin() + offset + count);
        const std::vector<Native_handle> no_descriptors;
        const auto& fragment_descriptors = offset == 0 ? descriptors : no_descriptors;
        while (true) {
            if (deadline && std::chrono::steady_clock::now() >= *deadline) {
                if (error) {
                    *error = "owner control send timed out";
                }
                return false;
            }

            native_error = 0;
            const auto status = channel.send_descriptors(
                fragment.bytes.data(), fragment.bytes.size(), fragment_descriptors, &native_error);
            if (status == Io_status::TRANSFERRED) {
                break;
            }
            if (status == Io_status::CLOSED) {
                if (error) {
                    *error = "owner control channel closed during send";
                }
                return false;
            }
            const bool would_block = status == Io_status::WOULD_BLOCK ||
                (status == Io_status::FAILED &&
                    (native_error == EAGAIN || native_error == EWOULDBLOCK));
            if (!deadline || !would_block) {
                transferred = false;
                break;
            }

            // A failed SEQPACKET send transferred nothing. Resume this record,
            // not the complete request, and keep the original deadline.
            while (true) {
                const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                    *deadline - std::chrono::steady_clock::now()).count();
                if (remaining <= 0) {
                    if (error) {
                        *error = "owner control send timed out";
                    }
                    return false;
                }
                pollfd descriptor{channel.native(), POLLOUT, 0};
                const int timeout_ms = static_cast<int>(std::min<std::int64_t>(
                    remaining, std::numeric_limits<int>::max()));
                const int ready = ::poll(&descriptor, 1, timeout_ms);
                if (ready < 0 && errno == EINTR) {
                    continue;
                }
                if (ready == 0) {
                    continue;
                }
                if (ready < 0 ||
                    (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
                {
                    if (error) {
                        *error = "owner control channel failed during send readiness wait";
                    }
                    return false;
                }
                if ((descriptor.revents & POLLOUT) != 0) {
                    break;
                }
            }
        }
        if (!transferred) {
            break;
        }
        offset += count;
    }
    if (transferred) return true;
    if (error) *error = "owner control send failed: " + std::to_string(native_error);
    return false;
}

} // namespace

static bool send_owner_start_impl(Duplex_channel& channel, const Owner_start_request& request,
    std::string* error, std::optional<std::chrono::steady_clock::time_point> deadline)
{
    auto message = prefix(detail::Message_kind::START);
    message.number(static_cast<std::uint32_t>(request.mode));
    message.number(request.use_owner_standard_streams ? 1 : 0);
    message.dimensions(request.dimensions);
    message.text(request.process.executable);
    message.text(request.process.working_directory);
    message.number(static_cast<std::uint32_t>(request.process.argv.size()));
    for (const auto& argument : request.process.argv) {
        if (const auto* text = std::get_if<std::string>(&argument.value)) {
            message.number(0); message.text(*text);
        }
        else {
            message.number(1);
            message.number(static_cast<std::uint32_t>(std::get<Inherited_reference>(argument.value).inherited_index));
        }
    }
    message.number(static_cast<std::uint32_t>(request.process.environment.size()));
    for (const auto& entry : request.process.environment) message.text(entry);
    std::vector<Native_handle> descriptors;
    message.number(static_cast<std::uint32_t>(request.process.inherited.size()));
    for (const auto& entry : request.process.inherited) {
        message.number(static_cast<std::uint32_t>(entry.child_number));
        descriptors.push_back(entry.parent_handle);
    }
    return send(channel, message, descriptors, error, deadline);
}

bool send_owner_start(Duplex_channel& channel, const Owner_start_request& request, std::string* error)
{
    return send_owner_start_impl(channel, request, error, std::nullopt);
}

bool send_owner_start_until(Duplex_channel& channel, const Owner_start_request& request,
    std::chrono::steady_clock::time_point deadline, std::string* error)
{
    const int flags = ::fcntl(channel.native(), F_GETFL, 0);
    if (flags < 0 || (flags & O_NONBLOCK) == 0) {
        if (error) {
            *error = "deadline owner start requires a nonblocking channel";
        }
        return false;
    }
    return send_owner_start_impl(channel, request, error, deadline);
}

bool send_owner_stop(Duplex_channel& channel, int grace_ms, std::string* error)
{
    auto message = prefix(detail::Message_kind::STOP);
    message.number(static_cast<std::uint32_t>(grace_ms));
    return send(channel, message, {}, error);
}

bool send_owner_resize(Duplex_channel& channel, Pty_dimensions dimensions, std::string* error)
{
    auto message = prefix(detail::Message_kind::RESIZE);
    message.dimensions(dimensions);
    return send(channel, message, {}, error);
}

namespace detail {

void close_start_descriptors(Owner_start_request& request)
{
    for (const auto& entry : request.process.inherited) close_native_handle(entry.parent_handle);
    request.process.inherited.clear();
}

bool send_event(Duplex_channel& channel, const Owner_event& event, std::string* error)
{
    auto message = prefix(Message_kind::EVENT);
    message.number(static_cast<std::uint32_t>(event.kind));
    message.number(static_cast<std::uint32_t>(event.root_exit.process_id));
    message.number(static_cast<std::uint32_t>(event.root_exit.exit_code));
    message.number(static_cast<std::uint32_t>(event.root_exit.signal));
    message.number(static_cast<std::uint32_t>(event.settlement));
    message.text(event.detail);
    std::vector<Native_handle> descriptors;
    if (event.root_reference != k_invalid_handle) descriptors.push_back(event.root_reference);
    if (event.pty_master != k_invalid_handle) descriptors.push_back(event.pty_master);
    return send(channel, message, descriptors, error);
}

Receive_status receive_message(Duplex_channel& channel, Owner_message* message,
    int timeout_ms, std::string* error)
{
    std::vector<std::uint8_t> bytes(k_owner_message_bytes);
    auto received = channel.receive(bytes.data(), bytes.size(), timeout_ms);
    if (received.status != Receive_status::MESSAGE) {
        for (auto handle : received.descriptors) close_native_handle(handle);
        return received.status;
    }
    bytes.resize(received.size);
    std::vector<std::uint8_t> assembled;
    const auto fragment_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::size_t total = 0;
    while (true) {
        Decoder fragment{bytes};
        const auto revision = fragment.number();
        const auto kind = fragment.number();
        const auto announced = fragment.number();
        const auto offset = fragment.number();
        if (!fragment.valid || revision != k_wire_revision || kind != k_fragment_kind ||
            announced == 0 || announced > k_max_request_bytes || offset != assembled.size() ||
            (total != 0 && total != announced) || bytes.size() == fragment.position ||
            bytes.size() - fragment.position > announced - std::min<std::size_t>(offset, announced))
        {
            for (auto handle : received.descriptors) close_native_handle(handle);
            if (error) *error = "malformed owner control fragment";
            return Receive_status::FAILED;
        }
        total = announced;
        assembled.insert(assembled.end(), bytes.begin() + fragment.position, bytes.end());
        if (assembled.size() == total) break;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(fragment_deadline -
            std::chrono::steady_clock::now()).count();
        bytes.resize(k_owner_message_bytes);
        auto continuation = channel.receive(bytes.data(), bytes.size(), static_cast<int>(std::max<std::int64_t>(0, left)));
        const bool invalid = continuation.status != Receive_status::MESSAGE || !continuation.descriptors.empty();
        for (auto handle : continuation.descriptors) close_native_handle(handle);
        if (invalid) {
            for (auto handle : received.descriptors) close_native_handle(handle);
            if (error) *error = "incomplete owner control request";
            return Receive_status::FAILED;
        }
        bytes.resize(continuation.size);
    }
    bytes = std::move(assembled);
    Decoder input{bytes};
    if (input.number() != k_wire_revision) input.valid = false;
    message->kind = static_cast<Message_kind>(input.number());
    std::size_t expected_descriptors = 0;
    switch (message->kind) {
        case Message_kind::START: {
            auto& request = message->start;
            request.mode = static_cast<Owner_mode>(input.number());
            const auto use_standard_streams = input.number();
            if (use_standard_streams > 1) input.valid = false;
            request.use_owner_standard_streams = use_standard_streams != 0;
            if (request.mode != Owner_mode::COMMAND && request.mode != Owner_mode::PTY) input.valid = false;
            request.dimensions = input.dimensions();
            request.process.executable = input.text();
            request.process.working_directory = input.text();
            const auto arguments = input.count();
            for (std::size_t i = 0; i < arguments && input.valid; ++i) {
                const auto kind = input.number();
                if (kind == 0) request.process.argv.emplace_back(input.text());
                else if (kind == 1) request.process.argv.emplace_back(Inherited_reference{input.number()});
                else input.valid = false;
            }
            const auto environment = input.count();
            for (std::size_t i = 0; i < environment && input.valid; ++i)
                request.process.environment.push_back(input.text());
            expected_descriptors = input.count();
            if (expected_descriptors != received.descriptors.size()) input.valid = false;
            for (std::size_t i = 0; i < expected_descriptors && input.valid; ++i) {
                const auto target = input.number();
                if (target > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) input.valid = false;
                request.process.inherited.push_back({received.descriptors[i], static_cast<int>(target)});
            }
            break;
        }
        case Message_kind::STOP: {
            const auto grace = input.number();
            if (grace > 60000) input.valid = false;
            message->grace_ms = static_cast<int>(grace);
            break;
        }
        case Message_kind::RESIZE:
            message->dimensions = input.dimensions();
            break;
        case Message_kind::EVENT: {
            auto& event = message->event;
            event.kind = static_cast<Owner_event_kind>(input.number());
            event.root_exit.process_id = input.number();
            event.root_exit.exit_code = static_cast<std::int32_t>(input.number());
            event.root_exit.signal = static_cast<int>(input.number());
            event.settlement = static_cast<Tree_settlement>(input.number());
            event.detail = input.text();
            if (event.kind > Owner_event_kind::TREE_SETTLEMENT || event.settlement > Tree_settlement::UNCONFIRMED)
                input.valid = false;
            expected_descriptors = received.descriptors.size();
            if (expected_descriptors > 2 ||
                (event.kind == Owner_event_kind::STARTED ? expected_descriptors == 0 : expected_descriptors != 0))
                input.valid = false;
            if (expected_descriptors >= 1) event.root_reference = received.descriptors[0];
            if (expected_descriptors == 2) event.pty_master = received.descriptors[1];
            break;
        }
        default: input.valid = false; break;
    }
    if (!input.valid || input.position != bytes.size() || expected_descriptors != received.descriptors.size()) {
        for (auto handle : received.descriptors) close_native_handle(handle);
        message->start.process.inherited.clear();
        message->event.pty_master = k_invalid_handle;
        message->event.root_reference = k_invalid_handle;
        if (error) *error = "malformed owner control message";
        return Receive_status::FAILED;
    }
    return Receive_status::MESSAGE;
}

} // namespace detail

Receive_status receive_owner_event(Duplex_channel& channel, Owner_event* event,
    int timeout_ms, std::string* error)
{
    detail::Owner_message message;
    auto status = detail::receive_message(channel, &message, timeout_ms, error);
    if (status == Receive_status::MESSAGE && message.kind != detail::Message_kind::EVENT) {
        detail::close_start_descriptors(message.start);
        if (error) *error = "unexpected owner control message";
        status = Receive_status::FAILED;
    }
    if (status == Receive_status::MESSAGE) *event = std::move(message.event);
    return status;
}

} // namespace vnm::process_custody
