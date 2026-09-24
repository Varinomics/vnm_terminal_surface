#include <vnm_process_custody/process_channel.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>

namespace vnm::process_custody {

namespace {

constexpr std::size_t k_length_prefix_bytes          = 4;
constexpr std::size_t k_max_transferred_descriptors  = 64; // matches the native spawn descriptor-map bound

void encode_length(std::size_t in_size, std::uint8_t out_prefix[k_length_prefix_bytes])
{
    const auto value = static_cast<std::uint32_t>(in_size);
    out_prefix[0] = static_cast<std::uint8_t>(value >> 24);
    out_prefix[1] = static_cast<std::uint8_t>(value >> 16);
    out_prefix[2] = static_cast<std::uint8_t>(value >> 8);
    out_prefix[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t decode_length(const std::uint8_t in_prefix[k_length_prefix_bytes])
{
    return
        (static_cast<std::uint32_t>(in_prefix[0]) << 24) |
        (static_cast<std::uint32_t>(in_prefix[1]) << 16) |
        (static_cast<std::uint32_t>(in_prefix[2]) << 8)  |
        static_cast<std::uint32_t>(in_prefix[3]);
}

Io_status status_from_send_errno(int in_errno, int* out_error)
{
    if (in_errno == EPIPE || in_errno == ECONNRESET || in_errno == ENOTCONN) {
        return Io_status::CLOSED;
    }
    if (out_error) {
        *out_error = in_errno;
    }
    return Io_status::FAILED;
}

int wait_for(int fd, short events, int timeout_ms)
{
    pollfd descriptor{};
    descriptor.fd     = fd;
    descriptor.events = events;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    int remaining_ms = timeout_ms;
    for (;;) {
        const int ready = poll(&descriptor, 1, remaining_ms);
        if (ready >= 0 || errno != EINTR) {
            return ready;
        }
        if (timeout_ms < 0) {
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return 0;
        }
        remaining_ms = static_cast<int>(
            std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count());
    }
}

bool set_nonblocking(int fd, std::string* out_error)
{
    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        *out_error = std::string("fcntl(O_NONBLOCK): ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool set_message_buffer(int fd, std::size_t message_bytes, std::string* out_error)
{
    // Linux reports SO_SNDBUF/SO_RCVBUF in accounting units which can be
    // doubled and silently clamped. Reserve a page beyond the wire record so
    // the configured value is an intentionally conservative capacity probe,
    // not a claim about a kernel-private skb allocation size.
    constexpr std::size_t k_seqpacket_record_headroom_bytes = 4096;
    if (message_bytes >
        static_cast<std::size_t>(std::numeric_limits<int>::max()) - k_seqpacket_record_headroom_bytes)
    {
        *out_error = "channel message bound does not fit the native socket buffer option";
        return false;
    }
    const int bytes = static_cast<int>(message_bytes + k_seqpacket_record_headroom_bytes);
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) != 0)
    {
        *out_error = std::string("setsockopt(channel message buffers): ") + std::strerror(errno);
        return false;
    }
    int effective_send = 0;
    int effective_receive = 0;
    socklen_t effective_send_size = sizeof(effective_send);
    socklen_t effective_receive_size = sizeof(effective_receive);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &effective_send, &effective_send_size) != 0 ||
        getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &effective_receive, &effective_receive_size) != 0)
    {
        *out_error = std::string("getsockopt(channel message buffers): ") + std::strerror(errno);
        return false;
    }
    if (effective_send < bytes || effective_receive < bytes)
    {
        *out_error = "effective channel message buffers cannot carry the configured complete record";
        return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Duplex_channel

struct Duplex_channel::Impl
{
    int         fd                = -1;
    std::size_t max_message_bytes = 0;
};

Duplex_channel::Duplex_channel()
:
    m_impl(std::make_unique<Impl>())
{}

Duplex_channel::Duplex_channel(Native_handle in_end, std::size_t in_max_message_bytes)
:
    m_impl(std::make_unique<Impl>())
{
    m_impl->fd                = in_end;
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
        m_impl       = std::move(other.m_impl);
        other.m_impl = std::make_unique<Impl>();
    }
    return *this;
}

bool Duplex_channel::create_pair(
    std::size_t     in_max_message_bytes,
    Duplex_channel* out_first,
    Duplex_channel* out_second,
    std::string*    out_error)
{
    int ends[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, ends) != 0) {
        *out_error = std::string("socketpair(SOCK_SEQPACKET): ") + std::strerror(errno);
        return false;
    }
    if (in_max_message_bytes > std::numeric_limits<std::size_t>::max() - k_length_prefix_bytes ||
        !set_message_buffer(ends[0], in_max_message_bytes + k_length_prefix_bytes, out_error) ||
        !set_message_buffer(ends[1], in_max_message_bytes + k_length_prefix_bytes, out_error))
    {
        ::close(ends[0]);
        ::close(ends[1]);
        return false;
    }
    *out_first  = Duplex_channel(ends[0], in_max_message_bytes);
    *out_second = Duplex_channel(ends[1], in_max_message_bytes);
    return true;
}

bool Duplex_channel::transfers_descriptors() { return true; }

bool          Duplex_channel::valid()             const { return m_impl->fd >= 0; }
Native_handle Duplex_channel::native()            const { return m_impl->fd; }
Native_handle Duplex_channel::waitable()          const { return m_impl->fd; }
std::size_t   Duplex_channel::max_message_bytes() const { return m_impl->max_message_bytes; }

bool Duplex_channel::prepare_for_owner(std::string* /*out_error*/)
{
    return m_impl->fd >= 0;
}

bool Duplex_channel::set_receive_staging_bytes_for_tests(std::size_t in_bytes)
{
    return in_bytes != 0;
}

void Duplex_channel::close()
{
    if (m_impl->fd >= 0) {
        (void)::close(m_impl->fd);
        m_impl->fd = -1;
    }
}

Io_status Duplex_channel::send(const std::uint8_t* in_payload, std::size_t in_size, int* out_error)
{
    return send_descriptors(in_payload, in_size, {}, out_error);
}

Io_status Duplex_channel::send_descriptors(
    const std::uint8_t*               in_payload,
    std::size_t                       in_size,
    const std::vector<Native_handle>& in_descriptors,
    int*                              out_error)
{
    if (in_size > m_impl->max_message_bytes || in_descriptors.size() > k_max_transferred_descriptors) {
        if (out_error) {
            *out_error = EMSGSIZE;
        }
        return Io_status::FAILED;
    }

    std::uint8_t prefix[k_length_prefix_bytes];
    encode_length(in_size, prefix);

    iovec parts[2];
    parts[0].iov_base = prefix;
    parts[0].iov_len  = k_length_prefix_bytes;
    parts[1].iov_base = const_cast<std::uint8_t*>(in_payload);
    parts[1].iov_len  = in_size;

    alignas(cmsghdr) std::uint8_t control[CMSG_SPACE(sizeof(int) * k_max_transferred_descriptors)];
    std::memset(control, 0, sizeof(control));

    msghdr message{};
    message.msg_iov    = parts;
    message.msg_iovlen = in_size > 0 ? 2 : 1;
    if (!in_descriptors.empty()) {
        message.msg_control    = control;
        message.msg_controllen = CMSG_SPACE(sizeof(int) * in_descriptors.size());
        cmsghdr* header        = CMSG_FIRSTHDR(&message);
        header->cmsg_level     = SOL_SOCKET;
        header->cmsg_type      = SCM_RIGHTS;
        header->cmsg_len       = CMSG_LEN(sizeof(int) * in_descriptors.size());
        std::memcpy(CMSG_DATA(header), in_descriptors.data(), sizeof(int) * in_descriptors.size());
    }

    ssize_t sent = 0;
    do {
        sent = sendmsg(m_impl->fd, &message, MSG_NOSIGNAL);
    }
    while (sent < 0 && errno == EINTR);

    if (sent < 0) {
        return status_from_send_errno(errno, out_error);
    }
    if (static_cast<std::size_t>(sent) != k_length_prefix_bytes + in_size) {
        // SOCK_SEQPACKET delivers a datagram whole or not at all; a short
        // count means the socket is not the SEQPACKET pair this class owns.
        if (out_error) {
            *out_error = EPROTO;
        }
        return Io_status::FAILED;
    }
    return Io_status::TRANSFERRED;
}

Receive_result Duplex_channel::receive(std::uint8_t* out_buffer, std::size_t in_capacity, int in_timeout_ms)
{
    Receive_result result;

    const int ready = wait_for(m_impl->fd, POLLIN, in_timeout_ms);
    if (ready == 0) {
        result.status = Receive_status::TIMEOUT;
        return result;
    }
    if (ready < 0) {
        result.error = errno;
        return result;
    }

    std::uint8_t prefix[k_length_prefix_bytes];
    iovec parts[2];
    parts[0].iov_base = prefix;
    parts[0].iov_len  = k_length_prefix_bytes;
    parts[1].iov_base = out_buffer;
    parts[1].iov_len  = in_capacity;

    alignas(cmsghdr) std::uint8_t control[CMSG_SPACE(sizeof(int) * k_max_transferred_descriptors)];

    msghdr message{};
    message.msg_iov        = parts;
    message.msg_iovlen     = 2;
    message.msg_control    = control;
    message.msg_controllen = sizeof(control);

    ssize_t received = 0;
    do {
        received = recvmsg(m_impl->fd, &message, MSG_CMSG_CLOEXEC);
    }
    while (received < 0 && errno == EINTR);

    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            result.status = Receive_status::TIMEOUT;
            return result;
        }
        result.error = errno;
        return result;
    }

    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) {
            continue;
        }
        const std::size_t count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (std::size_t i = 0; i < count; ++i) {
            int descriptor = -1;
            std::memcpy(&descriptor, CMSG_DATA(header) + i * sizeof(int), sizeof(int));
            result.descriptors.push_back(descriptor);
        }
    }

    const auto discard_descriptors = [&result]() {
        for (const int descriptor : result.descriptors) {
            (void)::close(descriptor);
        }
        result.descriptors.clear();
    };

    if (received == 0) {
        // Every message carries its prefix, so an empty read is the peer's
        // end closing, not an empty datagram.
        discard_descriptors();
        result.status = Receive_status::CLOSED;
        return result;
    }
    if ((message.msg_flags & MSG_TRUNC) != 0) {
        discard_descriptors();
        result.status = Receive_status::OVERSIZE;
        return result;
    }
    if ((message.msg_flags & MSG_CTRUNC) != 0 || static_cast<std::size_t>(received) < k_length_prefix_bytes) {
        discard_descriptors();
        result.error = EPROTO;
        return result;
    }

    const std::size_t payload = static_cast<std::size_t>(received) - k_length_prefix_bytes;
    const std::uint32_t declared = decode_length(prefix);
    if (declared > m_impl->max_message_bytes) {
        discard_descriptors();
        result.status = Receive_status::OVERSIZE;
        return result;
    }
    if (declared != payload) {
        discard_descriptors();
        result.error = EPROTO;
        return result;
    }

    result.status = Receive_status::MESSAGE;
    result.size   = payload;
    return result;
}

// ---------------------------------------------------------------------------
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
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ends) != 0) {
        *out_error = std::string("socketpair(SOCK_STREAM): ") + std::strerror(errno);
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
        count = ::send(m_impl->fd, in_data, in_size, MSG_DONTWAIT | MSG_NOSIGNAL);
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
