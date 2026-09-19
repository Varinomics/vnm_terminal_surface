#include <vnm_process_custody/process_pipe.h>

#include "process_writer_win.h"

#include <windows.h>

#include <cstring>
#include <vector>

namespace vnm::process_custody {

namespace {

// Anonymous pipes cannot be overlapped, so every pipe is one instance of a
// uniquely named pipe: the owner's end is opened FILE_FLAG_OVERLAPPED and the
// child's end is a plain synchronous handle, which keeps the child's stdio
// blocking as it expects.
std::wstring unique_pipe_name()
{
    static LONG s_sequence = 0;
    const LONG sequence = InterlockedIncrement(&s_sequence);
    wchar_t name[128];
    swprintf(
        name,
        sizeof(name) / sizeof(name[0]),
        L"\\\\.\\pipe\\logonomic-cli-addon-%lu-%lu-%lld",
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(sequence),
        static_cast<long long>(GetTickCount64()));
    return name;
}

std::string last_error_text(const char* in_call)
{
    return std::string(in_call) + " failed: error " + std::to_string(GetLastError());
}

// Overlapped read-ahead: one pending ReadFile at a time into `buffer`; the
// event is what wait_ready() observes.
struct Overlapped_reader
{
    HANDLE                    handle  = nullptr;
    HANDLE                    event   = nullptr;
    OVERLAPPED                overlapped{};
    std::vector<std::uint8_t> buffer;
    std::size_t               ready_bytes = 0;   // completed bytes not yet handed out
    std::size_t               consumed    = 0;
    bool                      pending     = false;
    bool                      closed      = false;
    int                       error       = 0;

    bool prepare(std::string* out_error)
    {
        event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) {
            *out_error = last_error_text("CreateEventW");
            return false;
        }
        buffer.resize(65536);
        return true;
    }

    void start_read()
    {
        if (pending || closed || ready_bytes > consumed) {
            return;
        }
        ResetEvent(event);
        std::memset(&overlapped, 0, sizeof(overlapped));
        overlapped.hEvent = event;
        DWORD transferred = 0;
        if (ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &transferred, &overlapped)) {
            ready_bytes = transferred;
            consumed    = 0;
            if (transferred == 0) {
                closed = true;
            }
            SetEvent(event);
            return;
        }
        const DWORD code = GetLastError();
        if (code == ERROR_IO_PENDING) {
            pending = true;
            return;
        }
        if (code == ERROR_BROKEN_PIPE || code == ERROR_HANDLE_EOF) {
            closed = true;
        }
        else {
            error = static_cast<int>(code);
        }
        SetEvent(event);
    }

    // Collects a completed read without blocking.
    void collect()
    {
        if (!pending) {
            return;
        }
        DWORD transferred = 0;
        if (GetOverlappedResult(handle, &overlapped, &transferred, FALSE)) {
            pending     = false;
            ready_bytes = transferred;
            consumed    = 0;
            if (transferred == 0) {
                closed = true;
            }
            return;
        }
        const DWORD code = GetLastError();
        if (code == ERROR_IO_INCOMPLETE) {
            return;
        }
        pending = false;
        if (code == ERROR_BROKEN_PIPE || code == ERROR_HANDLE_EOF) {
            closed = true;
        }
        else {
            error = static_cast<int>(code);
        }
    }

    Io_result read(std::uint8_t* out_buffer, std::size_t in_capacity)
    {
        collect();
        Io_result result;
        if (ready_bytes > consumed) {
            const std::size_t count = std::min(in_capacity, ready_bytes - consumed);
            std::memcpy(out_buffer, buffer.data() + consumed, count);
            consumed     += count;
            result.status = Io_status::TRANSFERRED;
            result.bytes  = count;
            if (consumed == ready_bytes) {
                ready_bytes = 0;
                consumed    = 0;
                start_read();
            }
            return result;
        }
        if (closed) {
            result.status = Io_status::CLOSED;
            return result;
        }
        if (error != 0) {
            result.status = Io_status::FAILED;
            result.error  = error;
            return result;
        }
        start_read();
        if (ready_bytes > consumed || closed || error != 0) {
            return read(out_buffer, in_capacity);
        }
        result.status = Io_status::WOULD_BLOCK;
        return result;
    }

    void close()
    {
        if (handle) {
            if (pending) {
                CancelIoEx(handle, &overlapped);
                DWORD transferred = 0;
                GetOverlappedResult(handle, &overlapped, &transferred, TRUE);
                pending = false;
            }
            CloseHandle(handle);
            handle = nullptr;
        }
        if (event) {
            CloseHandle(event);
            event = nullptr;
        }
    }
};

} // namespace

void close_native_handle(Native_handle in_handle)
{
    if (in_handle && in_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(in_handle);
    }
}

bool native_handle_is_open(Native_handle in_handle)
{
    DWORD flags = 0;
    return in_handle && in_handle != INVALID_HANDLE_VALUE && GetHandleInformation(in_handle, &flags);
}

std::string native_handle_argument(Native_handle in_handle)
{
    return std::to_string(reinterpret_cast<std::uintptr_t>(in_handle));
}

bool parse_native_handle_argument(const std::string& in_text, Native_handle* out_handle)
{
    if (in_text.empty() || in_text.size() > 20) {
        return false;
    }
    std::uintptr_t value = 0;
    for (const char c : in_text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<std::uintptr_t>(c - '0');
    }
    *out_handle = reinterpret_cast<Native_handle>(value);
    return *out_handle != nullptr;
}

// ---------------------------------------------------------------------------
// Readable_pipe

struct Readable_pipe::Impl
{
    Overlapped_reader reader;
    bool              prepared = false;
};

Readable_pipe::Readable_pipe()
:
    m_impl(std::make_unique<Impl>())
{}

Readable_pipe::Readable_pipe(Native_handle in_handle)
:
    m_impl(std::make_unique<Impl>())
{
    m_impl->reader.handle = in_handle;
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

bool          Readable_pipe::valid()    const { return m_impl->reader.handle != nullptr; }
Native_handle Readable_pipe::native()   const { return m_impl->reader.handle; }
Native_handle Readable_pipe::waitable() const { return m_impl->reader.event; }

void Readable_pipe::close() { m_impl->reader.close(); }

bool Readable_pipe::prepare_for_owner(std::string* out_error)
{
    if (!m_impl->reader.prepare(out_error)) {
        return false;
    }
    m_impl->prepared = true;
    m_impl->reader.start_read();
    return true;
}

Io_result Readable_pipe::read(std::uint8_t* out_buffer, std::size_t in_capacity)
{
    return m_impl->reader.read(out_buffer, in_capacity);
}

// ---------------------------------------------------------------------------
// Writable_pipe

struct Writable_pipe::Impl
{
    detail::Overlapped_writer writer;
};

Writable_pipe::Writable_pipe()
:
    m_impl(std::make_unique<Impl>())
{}

Writable_pipe::Writable_pipe(Native_handle in_handle)
:
    m_impl(std::make_unique<Impl>())
{
    m_impl->writer.handle = in_handle;
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

bool          Writable_pipe::valid()    const { return m_impl->writer.handle != nullptr; }
Native_handle Writable_pipe::native()   const { return m_impl->writer.handle; }
Native_handle Writable_pipe::waitable() const { return m_impl->writer.waitable(); }

void Writable_pipe::close() { m_impl->writer.close(); }

bool Writable_pipe::prepare_for_owner(std::string* out_error)
{
    return m_impl->writer.prepare(out_error);
}

Write_submission_result Writable_pipe::begin_write(const std::uint8_t* in_data, std::size_t in_size)
{
    return m_impl->writer.begin_write(in_data, in_size);
}

Io_result Writable_pipe::complete_write(Write_ticket in_ticket)
{
    return m_impl->writer.complete_write(in_ticket);
}

// ---------------------------------------------------------------------------

bool create_pipe(Readable_pipe* out_read_end, Writable_pipe* out_write_end, std::string* out_error)
{
    // Both ends are overlapped owner-side handles. prepare_for_owner creates
    // the readiness event and never changes the handle. This constructor is
    // for in-process wake/data pipes; the child-oriented constructors below
    // create a distinct synchronous end for inherited stdio.
    const std::wstring name = unique_pipe_name();
    HANDLE server = CreateNamedPipeW(
        name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        65536,
        65536,
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
    *out_read_end  = Readable_pipe(server);
    *out_write_end = Writable_pipe(client);
    return true;
}

bool create_child_pipe_owner_reads(
    Readable_pipe* out_owner_read,
    Writable_pipe* out_child_write,
    std::string* out_error)
{
    const std::wstring name = unique_pipe_name();
    HANDLE owner = CreateNamedPipeW(
        name.c_str(),
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1,
        65536,
        65536,
        0,
        nullptr);
    if (owner == INVALID_HANDLE_VALUE) {
        *out_error = last_error_text("CreateNamedPipeW(owner read)");
        return false;
    }
    HANDLE child = CreateFileW(
        name.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (child == INVALID_HANDLE_VALUE) {
        *out_error = last_error_text("CreateFileW(child write)");
        CloseHandle(owner);
        return false;
    }
    *out_owner_read = Readable_pipe(owner);
    *out_child_write = Writable_pipe(child);
    return true;
}

bool create_child_pipe_owner_writes(
    Writable_pipe* out_owner_write,
    Readable_pipe* out_child_read,
    std::string* out_error)
{
    const std::wstring name = unique_pipe_name();
    HANDLE owner = CreateNamedPipeW(
        name.c_str(),
        PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1,
        65536,
        65536,
        0,
        nullptr);
    if (owner == INVALID_HANDLE_VALUE) {
        *out_error = last_error_text("CreateNamedPipeW(owner write)");
        return false;
    }
    HANDLE child = CreateFileW(
        name.c_str(),
        GENERIC_READ,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (child == INVALID_HANDLE_VALUE) {
        *out_error = last_error_text("CreateFileW(child read)");
        CloseHandle(owner);
        return false;
    }
    *out_owner_write = Writable_pipe(owner);
    *out_child_read = Readable_pipe(child);
    return true;
}

int wait_ready(Wait_entry* in_out_entries, std::size_t in_count, int in_timeout_ms, int* out_error)
{
    if (in_count == 0 || in_count > MAXIMUM_WAIT_OBJECTS) {
        if (out_error) {
            *out_error = static_cast<int>(ERROR_INVALID_PARAMETER);
        }
        return -1;
    }
    std::vector<HANDLE> handles(in_count);
    for (std::size_t i = 0; i < in_count; ++i) {
        Wait_entry& entry = in_out_entries[i];
        entry.readable = false;
        entry.writable = false;
        entry.closed   = false;
        entry.failed   = false;
        handles[i]     = entry.handle;
    }
    const DWORD timeout = in_timeout_ms < 0 ? INFINITE : static_cast<DWORD>(in_timeout_ms);
    const DWORD wait    = WaitForMultipleObjects(static_cast<DWORD>(in_count), handles.data(), FALSE, timeout);
    if (wait == WAIT_TIMEOUT) {
        return 0;
    }
    if (wait == WAIT_FAILED) {
        if (out_error) {
            *out_error = static_cast<int>(GetLastError());
        }
        return -1;
    }

    // WaitForMultipleObjects reports one index; every other signalled handle
    // is probed without waiting so the caller sees all ready entries at once.
    int observed = 0;
    for (std::size_t i = 0; i < in_count; ++i) {
        const bool signalled =
            (wait - WAIT_OBJECT_0) == i ||
            WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0;
        if (!signalled) {
            continue;
        }
        Wait_entry& entry = in_out_entries[i];
        entry.readable = entry.want_read;
        entry.writable = entry.want_write;
        ++observed;
    }
    return observed;
}

} // namespace vnm::process_custody
