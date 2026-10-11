#include <vnm_process_custody/process_channel.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>

namespace vnm::process_custody {
namespace {

constexpr std::size_t k_prefix_bytes = 4;
constexpr std::size_t k_max_descriptors = 64;
using Clock = std::chrono::steady_clock;

int remaining_ms(Clock::time_point deadline)
{
    if (deadline == Clock::time_point::max()) {
        return -1;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return (int)std::clamp<std::int64_t>(remaining, 0, std::numeric_limits<int>::max());
}

int wait_for(int fd, short events, Clock::time_point deadline)
{
    pollfd descriptor{fd, events, 0};
    int result;
    do {
        result = poll(&descriptor, 1, remaining_ms(deadline));
    }
    while (result < 0 && errno == EINTR);
    return result;
}

bool set_nonblocking(int fd, std::string* error)
{
    const int flags = fcntl(fd, F_GETFL);
    if (flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0) {
        return true;
    }
    *error = std::string("fcntl(O_NONBLOCK): ") + std::strerror(errno);
    return false;
}

bool prepare_socket(int fd, std::string* error)
{
    const int enabled = 1;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 &&
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0)
    {
        return true;
    }
    *error = std::string("socket descriptor setup: ") + std::strerror(errno);
    return false;
}

bool inspect_control(
    const msghdr& message, std::size_t capacity, bool first_byte,
    std::vector<Native_handle>* received_descriptors)
{
    bool valid = (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) == 0 && message.msg_controllen <= capacity;
    const std::size_t size = std::min<std::size_t>(message.msg_controllen, capacity);
    const auto* control = (const std::uint8_t*)message.msg_control;
    std::size_t position = 0;
    std::size_t descriptor_count = 0;
    while (position < size) {
        const std::size_t remaining = size - position;
        if (remaining < CMSG_LEN(0)) {
            return false;
        }
        cmsghdr header;
        std::memcpy(&header, control + position, sizeof(header));
        if (header.cmsg_len < CMSG_LEN(0)) {
            return false;
        }
        const std::size_t descriptor_bytes = std::min<std::size_t>(header.cmsg_len, remaining) - CMSG_LEN(0);
        const bool rights = header.cmsg_level == SOL_SOCKET && header.cmsg_type == SCM_RIGHTS;
        valid &= rights && first_byte && header.cmsg_len <= remaining && descriptor_bytes % sizeof(int) == 0;
        if (rights) {
            descriptor_count += descriptor_bytes / sizeof(int);
            valid &= descriptor_count <= k_max_descriptors;
            // Darwin's peek returns zero placeholders, not installed FDs.
            // Only a consuming receive may transfer descriptor ownership.
            if (received_descriptors) {
                for (std::size_t offset = 0; offset + sizeof(int) <= descriptor_bytes; offset += sizeof(int)) {
                    int descriptor;
                    std::memcpy(&descriptor, control + position + CMSG_LEN(0) + offset, sizeof(descriptor));
                    received_descriptors->push_back(descriptor);
                    valid &= fcntl(descriptor, F_SETFD, FD_CLOEXEC) == 0;
                }
            }
        }
        if (header.cmsg_len > remaining) {
            return false;
        }
        position += CMSG_SPACE(header.cmsg_len - CMSG_LEN(0));
    }
    return valid;
}

} // namespace

struct Duplex_channel::Impl
{
    int fd = -1;
    std::size_t max_message_bytes = 0;
    std::array<std::uint8_t, k_prefix_bytes> prefix{};
    std::size_t prefix_received = 0;
    std::vector<std::uint8_t> payload;
    std::size_t payload_received = 0;
    std::vector<Native_handle> descriptors;
};

Duplex_channel::Duplex_channel()
:
    m_impl(std::make_unique<Impl>())
{}

Duplex_channel::Duplex_channel(Native_handle in_end, std::size_t in_max_message_bytes)
:
    m_impl(std::make_unique<Impl>())
{
    m_impl->fd = in_end;
    m_impl->max_message_bytes = in_max_message_bytes;
}

Duplex_channel::~Duplex_channel() { close(); }

Duplex_channel::Duplex_channel(Duplex_channel&& other) noexcept
:
    m_impl(std::move(other.m_impl))
{
    other.m_impl = std::make_unique<Impl>();
}

Duplex_channel& Duplex_channel::operator=(Duplex_channel&& other) noexcept
{
    if (this != &other) {
        close();
        m_impl = std::move(other.m_impl);
        other.m_impl = std::make_unique<Impl>();
    }
    return *this;
}

bool Duplex_channel::create_pair(
    std::size_t in_max_message_bytes, Duplex_channel* out_first,
    Duplex_channel* out_second, std::string* out_error)
{
    if (in_max_message_bytes > std::numeric_limits<std::uint32_t>::max()) {
        *out_error = "channel message bound exceeds the wire length";
        return false;
    }
    int ends[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, ends) != 0) {
        *out_error = std::string("socketpair(SOCK_STREAM): ") + std::strerror(errno);
        return false;
    }
    if (!prepare_socket(ends[0], out_error) || !prepare_socket(ends[1], out_error)) {
        ::close(ends[0]);
        ::close(ends[1]);
        return false;
    }
    *out_first  = Duplex_channel(ends[0], in_max_message_bytes);
    *out_second = Duplex_channel(ends[1], in_max_message_bytes);
    return true;
}

bool Duplex_channel::transfers_descriptors() { return true; }
bool Duplex_channel::valid() const { return m_impl->fd >= 0; }
Native_handle Duplex_channel::native() const { return m_impl->fd; }
Native_handle Duplex_channel::waitable() const { return m_impl->fd; }
std::size_t Duplex_channel::max_message_bytes() const { return m_impl->max_message_bytes; }
bool Duplex_channel::prepare_for_owner(std::string* out_error)
{
    return prepare_socket(m_impl->fd, out_error);
}
bool Duplex_channel::set_receive_staging_bytes_for_tests(std::size_t in_bytes) { return in_bytes != 0; }

void Duplex_channel::close()
{
    if (m_impl->fd >= 0) {
        ::close(m_impl->fd);
        m_impl->fd = -1;
    }
    for (const int descriptor : m_impl->descriptors) {
        ::close(descriptor);
    }
    m_impl->descriptors.clear();
    m_impl->prefix_received = 0;
    m_impl->payload.clear();
    m_impl->payload_received = 0;
}

Io_status Duplex_channel::send(const std::uint8_t* in_payload, std::size_t in_size, int* out_error)
{
    return send_descriptors(in_payload, in_size, {}, out_error);
}

Io_status Duplex_channel::send_descriptors(
    const std::uint8_t* in_payload, std::size_t in_size,
    const std::vector<Native_handle>& in_descriptors, int* out_error)
{
    return send_descriptors_until(in_payload, in_size, in_descriptors, Clock::time_point::max(), out_error);
}

Io_status Duplex_channel::send_descriptors_until(
    const std::uint8_t* in_payload, std::size_t in_size,
    const std::vector<Native_handle>& in_descriptors, Clock::time_point in_deadline, int* out_error)
{
    if (in_deadline != Clock::time_point::max()) {
        // Darwin's MSG_DONTWAIT does not bound every send-buffer wait on a
        // blocking socket. Refuse before any bytes or descriptor ownership move.
        const int flags = fcntl(m_impl->fd, F_GETFL);
        if (flags < 0 || (flags & O_NONBLOCK) == 0) {
            if (out_error) {
                *out_error = flags < 0 ? errno : EINVAL;
            }
            return Io_status::FAILED;
        }
    }
    if (in_size > m_impl->max_message_bytes || in_descriptors.size() > k_max_descriptors) {
        if (out_error) {
            *out_error = EMSGSIZE;
        }
        return Io_status::FAILED;
    }
    std::vector<std::uint8_t> record(k_prefix_bytes + in_size);
    for (std::size_t index = 0; index != k_prefix_bytes; ++index) {
        record[index] = (std::uint8_t)(in_size >> (24 - 8 * index));
    }
    if (in_size != 0) {
        std::memcpy(record.data() + k_prefix_bytes, in_payload, in_size);
    }
    std::size_t sent = 0;
    while (sent != record.size()) {
        iovec part{record.data() + sent, record.size() - sent};
        alignas(cmsghdr) std::uint8_t control[CMSG_SPACE(sizeof(int) * k_max_descriptors)]{};
        msghdr message{};
        message.msg_iov = &part;
        message.msg_iovlen = 1;
        // SCM_RIGHTS belongs to the first byte only. Reattaching it after a
        // partial stream write would duplicate the transferred ownership.
        if (sent == 0 && !in_descriptors.empty()) {
            message.msg_control = control;
            message.msg_controllen = CMSG_SPACE(sizeof(int) * in_descriptors.size());
            auto* header = CMSG_FIRSTHDR(&message);
            header->cmsg_level = SOL_SOCKET;
            header->cmsg_type = SCM_RIGHTS;
            header->cmsg_len = CMSG_LEN(sizeof(int) * in_descriptors.size());
            std::memcpy(CMSG_DATA(header), in_descriptors.data(), sizeof(int) * in_descriptors.size());
        }
        const ssize_t count = sendmsg(m_impl->fd, &message, MSG_DONTWAIT);
        if (count > 0) {
            sent += (std::size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        int error = count == 0 ? EPIPE : errno;
        if (error == EAGAIN || error == EWOULDBLOCK) {
            const int ready = wait_for(m_impl->fd, POLLOUT, in_deadline);
            if (ready > 0) {
                continue;
            }
            error = ready == 0 ? ETIMEDOUT : errno;
        }
        if (sent != 0) {
            // A failed partial record has no replay boundary on a stream.
            close();
        }
        if (out_error) {
            *out_error = error;
        }
        return error == EPIPE || error == ECONNRESET || error == ENOTCONN ?
            Io_status::CLOSED : Io_status::FAILED;
    }
    return Io_status::TRANSFERRED;
}

Receive_result Duplex_channel::receive(std::uint8_t* out_buffer, std::size_t in_capacity, int in_timeout_ms)
{
    Receive_result result;
    const auto deadline = in_timeout_ms < 0 ? Clock::time_point::max() :
        Clock::now() + std::chrono::milliseconds(in_timeout_ms);
    while (true) {
        const bool reading_prefix = m_impl->prefix_received < k_prefix_bytes;
        if (!reading_prefix && m_impl->payload.size() > in_capacity) {
            result.status = Receive_status::OVERSIZE;
            close();
            return result;
        }
        if (!reading_prefix && m_impl->payload_received == m_impl->payload.size()) {
            result.status = Receive_status::MESSAGE;
            result.size = m_impl->payload.size();
            result.descriptors = std::move(m_impl->descriptors);
            if (result.size != 0) {
                std::memcpy(out_buffer, m_impl->payload.data(), result.size);
            }
            m_impl->prefix_received = 0;
            m_impl->payload.clear();
            m_impl->payload_received = 0;
            return result;
        }
        const int ready = wait_for(m_impl->fd, POLLIN, deadline);
        if (ready == 0) {
            result.status = Receive_status::TIMEOUT;
            return result;
        }
        if (ready < 0) {
            result.error = errno;
            close();
            return result;
        }
        const std::size_t already_read = m_impl->prefix_received + m_impl->payload_received;
        iovec part{};
        part.iov_base = reading_prefix ? m_impl->prefix.data() + m_impl->prefix_received :
            m_impl->payload.data() + m_impl->payload_received;
        part.iov_len = reading_prefix ? k_prefix_bytes - m_impl->prefix_received :
            m_impl->payload.size() - m_impl->payload_received;
        alignas(cmsghdr) std::uint8_t control[CMSG_SPACE(sizeof(int) * k_max_descriptors)]{};
        msghdr message{};
        message.msg_iov = &part;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        // Darwin installs all rights before truncating their returned numbers.
        // Peek rejects excess or misplaced rights while they remain socket-owned.
        const ssize_t peeked = recvmsg(m_impl->fd, &message, MSG_PEEK | MSG_DONTWAIT);
        if (peeked < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        const bool valid_peek = peeked >= 0 && inspect_control(message, sizeof(control), already_read == 0, nullptr);
        if (peeked <= 0 || !valid_peek) {
            result.status = peeked == 0 && already_read == 0 && valid_peek ?
                Receive_status::CLOSED : Receive_status::FAILED;
            result.error = peeked < 0 ? errno : EPROTO;
            close();
            return result;
        }
        // This is the sole reader. A peer may append after the peek, so consume
        // only the inspected bytes, excluding any newly appended ancillary data.
        part.iov_len = (std::size_t)peeked;
        message.msg_controllen = sizeof(control);
        message.msg_flags = 0;
        const ssize_t count = recvmsg(m_impl->fd, &message, MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        const bool malformed = count >= 0 &&
            !inspect_control(message, sizeof(control), already_read == 0, &m_impl->descriptors);
        if (count <= 0 || malformed) {
            result.status = count == 0 && already_read == 0 && !malformed ?
                Receive_status::CLOSED : Receive_status::FAILED;
            result.error = count < 0 ? errno : EPROTO;
            close();
            return result;
        }
        if (reading_prefix) {
            m_impl->prefix_received += (std::size_t)count;
            if (m_impl->prefix_received == k_prefix_bytes) {
                std::uint32_t length = 0;
                for (const auto byte : m_impl->prefix) {
                    length = (length << 8) | byte;
                }
                if (length > m_impl->max_message_bytes || length > in_capacity) {
                    result.status = Receive_status::OVERSIZE;
                    close();
                    return result;
                }
                m_impl->payload.resize(length);
            }
        }
        else {
            m_impl->payload_received += (std::size_t)count;
        }
    }
}

// Stream_channel

struct Stream_channel::Impl
{
    int fd = -1;
};

Stream_channel::Stream_channel()
:
    m_impl(std::make_unique<Impl>())
{}

Stream_channel::Stream_channel(Native_handle in_end)
:
    m_impl(std::make_unique<Impl>())
{
    m_impl->fd = in_end;
}

Stream_channel::~Stream_channel() { close(); }

Stream_channel::Stream_channel(Stream_channel&& other) noexcept
:
    m_impl(std::move(other.m_impl))
{
    other.m_impl = std::make_unique<Impl>();
}

Stream_channel& Stream_channel::operator=(Stream_channel&& other) noexcept
{
    if (this != &other) {
        close();
        m_impl       = std::move(other.m_impl);
        other.m_impl = std::make_unique<Impl>();
    }
    return *this;
}

bool Stream_channel::create_pair(Stream_channel* out_first, Stream_channel* out_second, std::string* out_error)
{
    int ends[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, ends) != 0) {
        *out_error = std::string("socketpair(SOCK_STREAM): ") + std::strerror(errno);
        return false;
    }
    if (!prepare_socket(ends[0], out_error) || !prepare_socket(ends[1], out_error)) {
        ::close(ends[0]);
        ::close(ends[1]);
        return false;
    }
    *out_first  = Stream_channel(ends[0]);
    *out_second = Stream_channel(ends[1]);
    return true;
}

bool          Stream_channel::valid()    const { return m_impl->fd >= 0; }
Native_handle Stream_channel::native()   const { return m_impl->fd; }
Native_handle Stream_channel::waitable() const { return m_impl->fd; }
Native_handle Stream_channel::write_waitable() const { return m_impl->fd; }

void Stream_channel::close()
{
    if (m_impl->fd >= 0) {
        (void)::close(m_impl->fd);
        m_impl->fd = -1;
    }
}

bool Stream_channel::prepare_for_owner(std::string* out_error)
{
    return set_nonblocking(m_impl->fd, out_error);
}

Io_result Stream_channel::read(std::uint8_t* out_buffer, std::size_t in_capacity)
{
    Io_result result;
    ssize_t count = 0;
    do {
        count = recv(m_impl->fd, out_buffer, in_capacity, MSG_DONTWAIT);
    }
    while (count < 0 && errno == EINTR);

    if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            result.status = Io_status::WOULD_BLOCK;
        }
        else
        if (errno == ECONNRESET) {
            result.status = Io_status::CLOSED;
        }
        else {
            result.error = errno;
        }
        return result;
    }
    result.status = count == 0 ? Io_status::CLOSED : Io_status::TRANSFERRED;
    result.bytes  = static_cast<std::size_t>(count);
    return result;
}

Write_submission_result Stream_channel::begin_write(const std::uint8_t* in_data, std::size_t in_size)
{
    Write_submission_result result;
    ssize_t count = 0;
    do {
        count = ::send(m_impl->fd, in_data, in_size, MSG_DONTWAIT);
    }
    while (count < 0 && errno == EINTR);
    if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            result.status = Write_submission_status::BUSY;
            return result;
        }
        if (errno == EPIPE || errno == ECONNRESET) {
            result.status = Write_submission_status::CLOSED;
            return result;
        }
        result.error = errno;
        return result;
    }
    result.status = Write_submission_status::TRANSFERRED;
    result.bytes  = static_cast<std::size_t>(count);
    return result;
}

Io_result Stream_channel::complete_write(Write_ticket /*in_ticket*/)
{
    Io_result result;
    result.error = EINVAL;
    return result;
}

} // namespace vnm::process_custody
