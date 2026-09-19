#include <vnm_process_custody/process_pipe.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <limits>
#include <vector>

namespace vnm::process_custody {

namespace {

bool set_nonblocking(int fd, std::string* out_error)
{
    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        *out_error = std::string("fcntl(O_NONBLOCK): ") + std::strerror(errno);
        return false;
    }
    return true;
}

Io_result io_result_from_errno()
{
    Io_result result;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        result.status = Io_status::WOULD_BLOCK;
    }
    else
    if (errno == EPIPE || errno == ECONNRESET) {
        result.status = Io_status::CLOSED;
    }
    else {
        result.status = Io_status::FAILED;
        result.error  = errno;
    }
    return result;
}

} // namespace

int posix_pidfd_open(int in_pid)
{
    return static_cast<int>(syscall(SYS_pidfd_open, in_pid, 0u));
}

int posix_pidfd_send_signal(int in_pidfd, int in_signal)
{
    return static_cast<int>(syscall(SYS_pidfd_send_signal, in_pidfd, in_signal, nullptr, 0u));
}

void close_native_handle(Native_handle in_handle)
{
    if (in_handle >= 0) {
        (void)close(in_handle);
    }
}

bool native_handle_is_open(Native_handle in_handle)
{
    return in_handle >= 0 && fcntl(in_handle, F_GETFD) != -1;
}

std::string native_handle_argument(Native_handle in_handle)
{
    return std::to_string(in_handle);
}

bool parse_native_handle_argument(const std::string& in_text, Native_handle* out_handle)
{
    if (in_text.empty() || in_text.size() > 9) {
        return false;
    }
    int value = 0;
    for (const char c : in_text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    *out_handle = value;
    return true;
}

// ---------------------------------------------------------------------------
// Readable_pipe

struct Readable_pipe::Impl
{
    int fd = -1;
};

Readable_pipe::Readable_pipe()
:
    m_impl(std::make_unique<Impl>())
{}

Readable_pipe::Readable_pipe(Native_handle in_handle)
:
    m_impl(std::make_unique<Impl>())
{
    m_impl->fd = in_handle;
}

Readable_pipe::~Readable_pipe() { close(); }

Readable_pipe::Readable_pipe(Readable_pipe&& other) noexcept
:
    m_impl(std::move(other.m_impl))
{
    other.m_impl = std::make_unique<Impl>();
}

Readable_pipe& Readable_pipe::operator=(Readable_pipe&& other) noexcept
{
    if (this != &other) {
        close();
        m_impl       = std::move(other.m_impl);
        other.m_impl = std::make_unique<Impl>();
    }
    return *this;
}

bool          Readable_pipe::valid()    const { return m_impl->fd >= 0; }
Native_handle Readable_pipe::native()   const { return m_impl->fd; }
Native_handle Readable_pipe::waitable() const { return m_impl->fd; }

void Readable_pipe::close()
{
    if (m_impl->fd >= 0) {
        (void)::close(m_impl->fd);
        m_impl->fd = -1;
    }
}

bool Readable_pipe::prepare_for_owner(std::string* out_error)
{
    return set_nonblocking(m_impl->fd, out_error);
}

Io_result Readable_pipe::read(std::uint8_t* out_buffer, std::size_t in_capacity)
{
    const ssize_t count = ::read(m_impl->fd, out_buffer, in_capacity);
    if (count < 0) {
        return io_result_from_errno();
    }
    Io_result result;
    result.status = count == 0 ? Io_status::CLOSED : Io_status::TRANSFERRED;
    result.bytes  = static_cast<std::size_t>(count);
    return result;
}

// ---------------------------------------------------------------------------
// Writable_pipe

struct Writable_pipe::Impl
{
    int fd = -1;
};

Writable_pipe::Writable_pipe()
:
    m_impl(std::make_unique<Impl>())
{}

Writable_pipe::Writable_pipe(Native_handle in_handle)
:
    m_impl(std::make_unique<Impl>())
{
    m_impl->fd = in_handle;
}

Writable_pipe::~Writable_pipe() { close(); }

Writable_pipe::Writable_pipe(Writable_pipe&& other) noexcept
:
    m_impl(std::move(other.m_impl))
{
    other.m_impl = std::make_unique<Impl>();
}

Writable_pipe& Writable_pipe::operator=(Writable_pipe&& other) noexcept
{
    if (this != &other) {
        close();
        m_impl       = std::move(other.m_impl);
        other.m_impl = std::make_unique<Impl>();
    }
    return *this;
}

bool          Writable_pipe::valid()    const { return m_impl->fd >= 0; }
Native_handle Writable_pipe::native()   const { return m_impl->fd; }
Native_handle Writable_pipe::waitable() const { return m_impl->fd; }

void Writable_pipe::close()
{
    if (m_impl->fd >= 0) {
        (void)::close(m_impl->fd);
        m_impl->fd = -1;
    }
}

bool Writable_pipe::prepare_for_owner(std::string* out_error)
{
    return set_nonblocking(m_impl->fd, out_error);
}

Write_submission_result Writable_pipe::begin_write(const std::uint8_t* in_data, std::size_t in_size)
{
    // A closed reader surfaces as EPIPE because the owning process ignores
    // SIGPIPE (prepare_parent_process in spawn.h); spawn() restores the
    // default disposition in every child before exec.
    const ssize_t count = ::write(m_impl->fd, in_data, in_size);
    Write_submission_result result;
    if (count < 0) {
        const Io_result io = io_result_from_errno();
        switch (io.status) {
            case Io_status::WOULD_BLOCK: result.status = Write_submission_status::BUSY;   break;
            case Io_status::CLOSED:      result.status = Write_submission_status::CLOSED; break;
            case Io_status::FAILED:
            default:
                result.status = Write_submission_status::FAILED;
                result.error  = io.error;
                break;
        }
        return result;
    }
    result.status = Write_submission_status::TRANSFERRED;
    result.bytes  = static_cast<std::size_t>(count);
    return result;
}

Io_result Writable_pipe::complete_write(Write_ticket /*in_ticket*/)
{
    Io_result result;
    result.error = EINVAL;
    return result;
}

// ---------------------------------------------------------------------------

bool create_pipe(Readable_pipe* out_read_end, Writable_pipe* out_write_end, std::string* out_error)
{
    int fds[2] = {-1, -1};
    if (pipe2(fds, O_CLOEXEC) != 0) {
        *out_error = std::string("pipe2: ") + std::strerror(errno);
        return false;
    }
    *out_read_end  = Readable_pipe(fds[0]);
    *out_write_end = Writable_pipe(fds[1]);
    return true;
}

bool create_child_pipe_owner_reads(
    Readable_pipe* out_owner_read,
    Writable_pipe* out_child_write,
    std::string* out_error)
{
    return create_pipe(out_owner_read, out_child_write, out_error);
}

bool create_child_pipe_owner_writes(
    Writable_pipe* out_owner_write,
    Readable_pipe* out_child_read,
    std::string* out_error)
{
    return create_pipe(out_child_read, out_owner_write, out_error);
}

int wait_ready(Wait_entry* in_out_entries, std::size_t in_count, int in_timeout_ms, int* out_error)
{
    std::vector<pollfd> descriptors(in_count);
    for (std::size_t i = 0; i < in_count; ++i) {
        Wait_entry& entry = in_out_entries[i];
        entry.readable = false;
        entry.writable = false;
        entry.closed   = false;
        entry.failed   = false;

        descriptors[i].fd      = entry.handle;
        descriptors[i].events  = 0;
        descriptors[i].revents = 0;
        if (entry.want_read)  { descriptors[i].events |= POLLIN;  }
        if (entry.want_write) { descriptors[i].events |= POLLOUT; }
    }

    int ready = 0;
    do {
        ready = poll(descriptors.data(), static_cast<nfds_t>(in_count), in_timeout_ms);
    }
    while (ready < 0 && errno == EINTR);

    if (ready < 0) {
        if (out_error) {
            *out_error = errno;
        }
        return -1;
    }

    int observed = 0;
    for (std::size_t i = 0; i < in_count; ++i) {
        const short revents = descriptors[i].revents;
        if (revents == 0) {
            continue;
        }
        Wait_entry& entry = in_out_entries[i];
        entry.readable = (revents & POLLIN)  != 0;
        entry.writable = (revents & POLLOUT) != 0;
        entry.closed   = (revents & POLLHUP) != 0;
        entry.failed   = (revents & (POLLERR | POLLNVAL)) != 0;
        ++observed;
    }
    return observed;
}

} // namespace vnm::process_custody
