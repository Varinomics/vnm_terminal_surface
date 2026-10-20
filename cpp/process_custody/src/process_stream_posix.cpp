#include "vnm_process_custody/process_channel.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

namespace vnm::process_custody {
namespace {

#if defined(__APPLE__)
constexpr int k_send_flags = MSG_DONTWAIT;
#else
constexpr int k_send_flags = MSG_DONTWAIT | MSG_NOSIGNAL;
#endif

} // namespace

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
    const int flags = fcntl(m_impl->fd, F_GETFL);
    if (flags < 0 || fcntl(m_impl->fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        *out_error = std::string("fcntl(O_NONBLOCK): ") + std::strerror(errno);
        return false;
    }
    return true;
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
        count = ::send(m_impl->fd, in_data, in_size, k_send_flags);
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
