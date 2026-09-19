#include <vnm_process_custody/process_channel.h>

#include <vnm_process_custody/process_clock.h>
#include "process_writer_win.h"

#include <windows.h>

#include <cstring>
#include <limits>

namespace vnm::process_custody {

namespace {

constexpr std::size_t k_length_prefix_bytes = 4;

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

std::wstring unique_channel_name(const wchar_t* in_role)
{
    static LONG s_sequence = 0;
    const LONG sequence = InterlockedIncrement(&s_sequence);
    wchar_t name[160];
    swprintf(
        name,
        sizeof(name) / sizeof(name[0]),
        L"\\\\.\\pipe\\logonomic-cli-addon-%lu-%lu-%lld-%ls",
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(sequence),
        static_cast<long long>(GetTickCount64()),
        in_role);
    return name;
}

std::string last_error_text(const char* in_call)
{
    return std::string(in_call) + " failed: error " + std::to_string(GetLastError());
}

// Both ends of a channel are overlapped named-pipe handles: the server end for
// this process and a client end that a child can inherit and still drive with
// its own overlapped reads (unlike an anonymous pipe, which cannot be
// overlapped at all).
enum class Pipe_mode
{
    BYTE_STREAM,
    MESSAGE_RECORD,
};

bool create_overlapped_pair(
    const wchar_t* in_role,
    Pipe_mode      in_mode,
    DWORD          in_buffer_bytes,
    HANDLE*        out_server,
    HANDLE*        out_client,
    std::string*   out_error)
{
    const std::wstring name = unique_channel_name(in_role);
    const DWORD pipe_mode = in_mode == Pipe_mode::MESSAGE_RECORD
        ? PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE
        : PIPE_TYPE_BYTE | PIPE_READMODE_BYTE;
    HANDLE server = CreateNamedPipeW(
        name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        pipe_mode | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        in_buffer_bytes,
        in_buffer_bytes,
        0,
        nullptr);
    if (server == INVALID_HANDLE_VALUE) {
        *out_error = last_error_text("CreateNamedPipeW");
        return false;
    }
    HANDLE client = CreateFileW(
        name.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr);
    if (client == INVALID_HANDLE_VALUE) {
        *out_error = last_error_text("CreateFileW(pipe client)");
        CloseHandle(server);
        return false;
    }
    if (in_mode == Pipe_mode::MESSAGE_RECORD) {
        DWORD read_mode = PIPE_READMODE_MESSAGE;
        if (!SetNamedPipeHandleState(client, &read_mode, nullptr, nullptr)) {
            *out_error = last_error_text("SetNamedPipeHandleState(PIPE_READMODE_MESSAGE)");
            CloseHandle(client);
            CloseHandle(server);
            return false;
        }
    }
    *out_server = server;
    *out_client = client;
    return true;
}

// One overlapped transfer driven to completion or timeout. Returns
// TRANSFERRED with the byte count, CLOSED on a broken pipe, WOULD_BLOCK when
// the timeout passed (the operation is then cancelled), FAILED otherwise.
Io_result transfer_with_timeout(
    HANDLE       in_handle,
    HANDLE       in_event,
    bool         in_reading,
    std::uint8_t* in_out_buffer,
    std::size_t  in_size,
    int          in_timeout_ms)
{
    Io_result result;
    OVERLAPPED overlapped{};
    overlapped.hEvent = in_event;
    ResetEvent(in_event);

    DWORD transferred = 0;
    const BOOL immediate = in_reading
        ? ReadFile(in_handle, in_out_buffer, static_cast<DWORD>(in_size), &transferred, &overlapped)
        : WriteFile(in_handle, in_out_buffer, static_cast<DWORD>(in_size), &transferred, &overlapped);
    if (!immediate) {
        const DWORD code = GetLastError();
        if (code == ERROR_BROKEN_PIPE || code == ERROR_HANDLE_EOF || code == ERROR_NO_DATA) {
            result.status = Io_status::CLOSED;
            return result;
        }
        if (code != ERROR_IO_PENDING) {
            result.error = static_cast<int>(code);
            return result;
        }
        const DWORD timeout = in_timeout_ms < 0 ? INFINITE : static_cast<DWORD>(in_timeout_ms);
        if (WaitForSingleObject(in_event, timeout) == WAIT_TIMEOUT) {
            CancelIoEx(in_handle, &overlapped);
            GetOverlappedResult(in_handle, &overlapped, &transferred, TRUE);
            result.status = Io_status::WOULD_BLOCK;
            return result;
        }
        if (!GetOverlappedResult(in_handle, &overlapped, &transferred, TRUE)) {
            const DWORD completion = GetLastError();
            if (completion == ERROR_BROKEN_PIPE || completion == ERROR_HANDLE_EOF || completion == ERROR_NO_DATA) {
                result.status = Io_status::CLOSED;
                return result;
            }
            result.error = static_cast<int>(completion);
            return result;
        }
    }
    if (in_reading && transferred == 0) {
        result.status = Io_status::CLOSED;
        return result;
    }
    result.status = Io_status::TRANSFERRED;
    result.bytes  = transferred;
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Duplex_channel

struct Duplex_channel::Impl
{
    enum class State
    {
        UNPREPARED,
        READING,
        MESSAGE,
        OVERSIZE,
        CLOSED,
        FAILED,
    };

    HANDLE                    handle            = nullptr;
    HANDLE                    read_event        = nullptr;
    HANDLE                    write_event       = nullptr;
    OVERLAPPED                read_overlapped{};
    std::vector<std::uint8_t> record;
    std::vector<std::uint8_t> discard;
    std::size_t               record_bytes      = 0;
    std::size_t               max_message_bytes = 0;
    std::size_t               first_read_bytes  = 0;
    std::size_t               message_bytes     = 0;
    bool                      read_pending      = false;
    bool                      first_read        = true;
    bool                      discarding        = false;
    State                     state             = State::UNPREPARED;
    int                       error             = 0;

    bool ensure_write_event()
    {
        if (!write_event) {
            write_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        }
        return write_event != nullptr;
    }

    void finish_record()
    {
        if (record_bytes < k_length_prefix_bytes) {
            state = State::FAILED;
            error = static_cast<int>(ERROR_INVALID_DATA);
            return;
        }
        const std::uint32_t declared = decode_length(record.data());
        if (declared > max_message_bytes) {
            state = State::OVERSIZE;
            return;
        }
        message_bytes = record_bytes - k_length_prefix_bytes;
        if (declared != message_bytes) {
            state = State::FAILED;
            error = static_cast<int>(ERROR_INVALID_DATA);
            return;
        }
        state = State::MESSAGE;
    }

    void accept_transfer(DWORD in_transferred, bool in_more_data)
    {
        first_read = false;
        if (discarding) {
            if (!in_more_data) {
                state = State::OVERSIZE;
            }
            return;
        }

        record_bytes += static_cast<std::size_t>(in_transferred);
        if (!in_more_data) {
            finish_record();
            return;
        }
        if (record_bytes == record.size()) {
            discarding = true;
        }
    }

    void start_read()
    {
        while (state == State::READING && !read_pending) {
            ResetEvent(read_event);
            std::memset(&read_overlapped, 0, sizeof(read_overlapped));
            read_overlapped.hEvent = read_event;

            std::uint8_t* destination = nullptr;
            std::size_t capacity = 0;
            if (discarding) {
                destination = discard.data();
                capacity    = discard.size();
            }
            else {
                destination = record.data() + record_bytes;
                capacity    = record.size() - record_bytes;
                if (first_read && first_read_bytes != 0) {
                    capacity = std::min(capacity, first_read_bytes);
                }
            }

            DWORD transferred = 0;
            if (ReadFile(
                    handle,
                    destination,
                    static_cast<DWORD>(capacity),
                    &transferred,
                    &read_overlapped))
            {
                accept_transfer(transferred, false);
                continue;
            }
            const DWORD code = GetLastError();
            if (code == ERROR_IO_PENDING) {
                read_pending = true;
                return;
            }
            if (code == ERROR_MORE_DATA) {
                accept_transfer(transferred, true);
                continue;
            }
            if (code == ERROR_BROKEN_PIPE || code == ERROR_HANDLE_EOF || code == ERROR_NO_DATA) {
                state = State::CLOSED;
            }
            else {
                state = State::FAILED;
                error = static_cast<int>(code);
            }
        }
        if (state != State::READING) {
            SetEvent(read_event);
        }
    }

    void collect_read()
    {
        if (!read_pending) {
            return;
        }
        DWORD transferred = 0;
        if (GetOverlappedResult(handle, &read_overlapped, &transferred, FALSE)) {
            read_pending = false;
            accept_transfer(transferred, false);
            start_read();
            return;
        }
        const DWORD code = GetLastError();
        if (code == ERROR_IO_INCOMPLETE) {
            return;
        }
        read_pending = false;
        if (code == ERROR_MORE_DATA) {
            accept_transfer(transferred, true);
            start_read();
            return;
        }
        if (code == ERROR_BROKEN_PIPE || code == ERROR_HANDLE_EOF || code == ERROR_NO_DATA) {
            state = State::CLOSED;
        }
        else {
            state = State::FAILED;
            error = static_cast<int>(code);
        }
        SetEvent(read_event);
    }

    void next_record()
    {
        record_bytes = 0;
        message_bytes = 0;
        first_read = true;
        discarding = false;
        state = State::READING;
        error = 0;
        start_read();
    }
};

Duplex_channel::Duplex_channel()
:
    m_impl(std::make_unique<Impl>())
{}

Duplex_channel::Duplex_channel(Native_handle in_end, std::size_t in_max_message_bytes)
:
    m_impl(std::make_unique<Impl>())
{
    m_impl->handle            = in_end;
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
    if (in_max_message_bytes > static_cast<std::size_t>(std::numeric_limits<DWORD>::max()) - k_length_prefix_bytes) {
        *out_error = "channel message bound does not fit the native named-pipe buffer";
        return false;
    }
    HANDLE server = nullptr;
    HANDLE client = nullptr;
    if (!create_overlapped_pair(
            L"control",
            Pipe_mode::MESSAGE_RECORD,
            static_cast<DWORD>(in_max_message_bytes + k_length_prefix_bytes),
            &server,
            &client,
            out_error))
    {
        return false;
    }
    *out_first  = Duplex_channel(server, in_max_message_bytes);
    *out_second = Duplex_channel(client, in_max_message_bytes);
    return true;
}

bool Duplex_channel::transfers_descriptors() { return false; }

bool          Duplex_channel::valid()             const { return m_impl->handle != nullptr; }
Native_handle Duplex_channel::native()            const { return m_impl->handle; }
Native_handle Duplex_channel::waitable() const
{
    return m_impl->read_event;
}
std::size_t   Duplex_channel::max_message_bytes() const { return m_impl->max_message_bytes; }

bool Duplex_channel::prepare_for_owner(std::string* out_error)
{
    if (!m_impl->handle || m_impl->state != Impl::State::UNPREPARED) {
        if (out_error) {
            *out_error = "Duplex channel is invalid or already prepared";
        }
        return false;
    }
    m_impl->read_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_impl->read_event) {
        if (out_error) {
            *out_error = last_error_text("CreateEventW");
        }
        return false;
    }
    m_impl->record.resize(k_length_prefix_bytes + m_impl->max_message_bytes);
    m_impl->discard.resize(65536);
    m_impl->next_record();
    return true;
}

bool Duplex_channel::set_receive_staging_bytes_for_tests(std::size_t in_bytes)
{
    if (in_bytes == 0 || m_impl->state != Impl::State::UNPREPARED) {
        return false;
    }
    m_impl->first_read_bytes = in_bytes;
    return true;
}

void Duplex_channel::close()
{
    if (m_impl->handle) {
        if (m_impl->read_pending) {
            CancelIoEx(m_impl->handle, &m_impl->read_overlapped);
            DWORD transferred = 0;
            GetOverlappedResult(m_impl->handle, &m_impl->read_overlapped, &transferred, TRUE);
            m_impl->read_pending = false;
        }
        CloseHandle(m_impl->handle);
        m_impl->handle = nullptr;
    }
    if (m_impl->read_event) {
        CloseHandle(m_impl->read_event);
        m_impl->read_event = nullptr;
    }
    if (m_impl->write_event) {
        CloseHandle(m_impl->write_event);
        m_impl->write_event = nullptr;
    }
    m_impl->state = Impl::State::CLOSED;
}

Io_status Duplex_channel::send(const std::uint8_t* in_payload, std::size_t in_size, int* out_error)
{
    if (in_size > m_impl->max_message_bytes || !m_impl->ensure_write_event()) {
        if (out_error) {
            *out_error = static_cast<int>(ERROR_INVALID_PARAMETER);
        }
        return Io_status::FAILED;
    }
    std::vector<std::uint8_t> message(k_length_prefix_bytes + in_size);
    encode_length(in_size, message.data());
    std::memcpy(message.data() + k_length_prefix_bytes, in_payload, in_size);

    const Io_result written = transfer_with_timeout(
        m_impl->handle, m_impl->write_event, false, message.data(), message.size(), -1);
    if (written.status == Io_status::TRANSFERRED && written.bytes != message.size()) {
        if (out_error) {
            *out_error = static_cast<int>(ERROR_MORE_DATA);
        }
        return Io_status::FAILED;
    }
    if (written.status == Io_status::FAILED && out_error) {
        *out_error = written.error;
    }
    return written.status;
}

Io_status Duplex_channel::send_descriptors(
    const std::uint8_t*               in_payload,
    std::size_t                       in_size,
    const std::vector<Native_handle>& in_descriptors,
    int*                              out_error)
{
    // Handles do not travel over a pipe; callers branch on
    // transfers_descriptors() and only ever reach this with an empty list.
    if (!in_descriptors.empty()) {
        if (out_error) {
            *out_error = static_cast<int>(ERROR_NOT_SUPPORTED);
        }
        return Io_status::FAILED;
    }
    return send(in_payload, in_size, out_error);
}

Receive_result Duplex_channel::receive(std::uint8_t* out_buffer, std::size_t in_capacity, int in_timeout_ms)
{
    Receive_result result;
    if (m_impl->state == Impl::State::UNPREPARED || !m_impl->read_event) {
        result.error = static_cast<int>(ERROR_INVALID_FUNCTION);
        return result;
    }
    const Deadline deadline = in_timeout_ms < 0
        ? Deadline::never()
        : Deadline::after(std::chrono::milliseconds(in_timeout_ms));

    while (m_impl->state == Impl::State::READING) {
        m_impl->collect_read();
        if (m_impl->state != Impl::State::READING) {
            break;
        }
        const int remaining = deadline.remaining_ms();
        const DWORD wait = WaitForSingleObject(
            m_impl->read_event,
            remaining < 0 ? INFINITE : static_cast<DWORD>(remaining));
        if (wait == WAIT_TIMEOUT) {
            result.status = Receive_status::TIMEOUT;
            return result;
        }
        if (wait != WAIT_OBJECT_0) {
            result.error = static_cast<int>(GetLastError());
            return result;
        }
    }
    switch (m_impl->state) {
        case Impl::State::MESSAGE:
            if (m_impl->message_bytes > in_capacity) {
                result.error = static_cast<int>(ERROR_INSUFFICIENT_BUFFER);
                return result;
            }
            std::memcpy(
                out_buffer,
                m_impl->record.data() + k_length_prefix_bytes,
                m_impl->message_bytes);
            result.status = Receive_status::MESSAGE;
            result.size   = m_impl->message_bytes;
            m_impl->next_record();
            return result;
        case Impl::State::OVERSIZE:
            result.status = Receive_status::OVERSIZE;
            m_impl->next_record();
            return result;
        case Impl::State::CLOSED:
            result.status = Receive_status::CLOSED;
            return result;
        case Impl::State::FAILED:
            result.error = m_impl->error;
            return result;
        case Impl::State::UNPREPARED:
        case Impl::State::READING:
            return result;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Stream_channel

struct Stream_channel::Impl
{
    HANDLE                    handle       = nullptr;
    HANDLE                    read_event   = nullptr;
    OVERLAPPED                read_overlapped{};
    std::vector<std::uint8_t> read_buffer;
    std::size_t               ready_bytes  = 0;
    std::size_t               consumed     = 0;
    bool                      read_pending = false;
    detail::Overlapped_writer writer;
    bool                      closed       = false;
    int                       error        = 0;

    void start_read()
    {
        if (read_pending || closed || error != 0 || ready_bytes > consumed) {
            return;
        }
        ResetEvent(read_event);
        std::memset(&read_overlapped, 0, sizeof(read_overlapped));
        read_overlapped.hEvent = read_event;
        DWORD transferred = 0;
        if (ReadFile(handle, read_buffer.data(), static_cast<DWORD>(read_buffer.size()), &transferred, &read_overlapped)) {
            ready_bytes = transferred;
            consumed    = 0;
            closed      = transferred == 0;
            SetEvent(read_event);
            return;
        }
        const DWORD code = GetLastError();
        if (code == ERROR_IO_PENDING) {
            read_pending = true;
            return;
        }
        if (code == ERROR_BROKEN_PIPE || code == ERROR_HANDLE_EOF) {
            closed = true;
        }
        else {
            error = static_cast<int>(code);
        }
        SetEvent(read_event);
    }

    void collect_read()
    {
        if (!read_pending) {
            return;
        }
        DWORD transferred = 0;
        if (GetOverlappedResult(handle, &read_overlapped, &transferred, FALSE)) {
            read_pending = false;
            ready_bytes  = transferred;
            consumed     = 0;
            closed       = transferred == 0;
            return;
        }
        const DWORD code = GetLastError();
        if (code == ERROR_IO_INCOMPLETE) {
            return;
        }
        read_pending = false;
        if (code == ERROR_BROKEN_PIPE || code == ERROR_HANDLE_EOF) {
            closed = true;
        }
        else {
            error = static_cast<int>(code);
        }
    }

};

Stream_channel::Stream_channel()
:
    m_impl(std::make_unique<Impl>())
{}

Stream_channel::Stream_channel(Native_handle in_end)
:
    m_impl(std::make_unique<Impl>())
{
    m_impl->handle = in_end;
    m_impl->writer.handle = in_end;
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
    HANDLE server = nullptr;
    HANDLE client = nullptr;
    if (!create_overlapped_pair(L"link", Pipe_mode::BYTE_STREAM, 65536, &server, &client, out_error)) {
        return false;
    }
    *out_first  = Stream_channel(server);
    *out_second = Stream_channel(client);
    return true;
}

bool          Stream_channel::valid()    const { return m_impl->handle != nullptr; }
Native_handle Stream_channel::native()   const { return m_impl->handle; }
Native_handle Stream_channel::waitable() const { return m_impl->read_event; }
Native_handle Stream_channel::write_waitable() const { return m_impl->writer.waitable(); }

void Stream_channel::close()
{
    if (m_impl->handle) {
        if (m_impl->read_pending) {
            CancelIoEx(m_impl->handle, &m_impl->read_overlapped);
            DWORD transferred = 0;
            GetOverlappedResult(m_impl->handle, &m_impl->read_overlapped, &transferred, TRUE);
            m_impl->read_pending = false;
        }
        m_impl->writer.close();
        m_impl->handle = nullptr;
    }
    if (m_impl->read_event) {
        CloseHandle(m_impl->read_event);
        m_impl->read_event = nullptr;
    }
}

bool Stream_channel::prepare_for_owner(std::string* out_error)
{
    m_impl->read_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_impl->read_event) {
        *out_error = last_error_text("CreateEventW");
        return false;
    }
    if (!m_impl->writer.prepare(out_error)) {
        return false;
    }
    m_impl->read_buffer.resize(65536);
    m_impl->start_read();
    return true;
}

Io_result Stream_channel::read(std::uint8_t* out_buffer, std::size_t in_capacity)
{
    Impl& impl = *m_impl;
    impl.collect_read();
    Io_result result;
    if (impl.ready_bytes > impl.consumed) {
        const std::size_t count = std::min(in_capacity, impl.ready_bytes - impl.consumed);
        std::memcpy(out_buffer, impl.read_buffer.data() + impl.consumed, count);
        impl.consumed += count;
        result.status  = Io_status::TRANSFERRED;
        result.bytes   = count;
        if (impl.consumed == impl.ready_bytes) {
            impl.ready_bytes = 0;
            impl.consumed    = 0;
            impl.start_read();
        }
        return result;
    }
    if (impl.closed) {
        result.status = Io_status::CLOSED;
        return result;
    }
    if (impl.error != 0) {
        result.error = impl.error;
        return result;
    }
    impl.start_read();
    if (impl.ready_bytes > impl.consumed || impl.closed || impl.error != 0) {
        return read(out_buffer, in_capacity);
    }
    result.status = Io_status::WOULD_BLOCK;
    return result;
}

Write_submission_result Stream_channel::begin_write(const std::uint8_t* in_data, std::size_t in_size)
{
    return m_impl->writer.begin_write(in_data, in_size);
}

Io_result Stream_channel::complete_write(Write_ticket in_ticket)
{
    return m_impl->writer.complete_write(in_ticket);
}

} // namespace vnm::process_custody
