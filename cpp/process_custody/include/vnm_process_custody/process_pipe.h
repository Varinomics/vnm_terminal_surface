#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace vnm::process_custody {

// The one platform-dependent declaration the library's headers carry: the OS
// handle type. Linux code passes descriptors, Windows code passes HANDLEs, and
// every platform-neutral caller (the two executables, the desktop client, the
// tests) only ever moves the value between library calls.
#if defined(_WIN32)
using Native_handle = void*;
inline constexpr Native_handle k_invalid_handle = nullptr;
#else
using Native_handle = int;
inline constexpr Native_handle k_invalid_handle = -1;
#endif

#if !defined(_WIN32)
// The pidfd syscalls, called directly rather than through <sys/pidfd.h>:
// that glibc header (2.39) is missing its extern-"C" guard, so its wrappers
// do not link from C++. The runtime-owner service reaches pidfd the same way
// (logonomic_runtime_owner_service.cpp), and the raw syscall is stable across
// glibc versions.
int posix_pidfd_open(int in_pid);
int posix_pidfd_send_signal(int in_pidfd, int in_signal);
#endif

/// Closes a handle owned by the caller. Nothing to report: the only failure
/// of close() a caller could act on is passing a handle it does not own.
void close_native_handle(Native_handle in_handle);

/// Whether the handle names an open object in this process; how a child
/// checks that a handle it was told about was actually inherited.
bool native_handle_is_open(Native_handle in_handle);

/// Text form of a handle for a child's command line and its parse. The child
/// receives what the parent chose for it in Spawn_request::inherited (a
/// descriptor number on Linux; the unchanged handle value on Windows, which
/// PROC_THREAD_ATTRIBUTE_HANDLE_LIST preserves across process creation).
std::string native_handle_argument(Native_handle in_handle);
bool        parse_native_handle_argument(const std::string& in_text, Native_handle* out_handle);

enum class Io_status
{
    TRANSFERRED,   // bytes moved (possibly fewer than requested)
    WOULD_BLOCK,   // nothing available / no room right now; wait for readiness
    CLOSED,        // the peer closed its end
    FAILED,        // error holds the platform error code
};

struct Io_result
{
    Io_status   status = Io_status::FAILED;
    std::size_t bytes  = 0;
    int         error  = 0;
};

using Write_ticket = std::uint64_t;

enum class Write_submission_status
{
    TRANSFERRED,   // the owned copy completed synchronously; bytes is valid
    PENDING,       // the owned copy is in flight; ticket is valid
    BUSY,          // another owned copy is in flight; no input was accepted
    CLOSED,        // the peer closed its end
    FAILED,        // error holds the platform error code
};

struct Write_submission_result
{
    Write_submission_status status = Write_submission_status::FAILED;
    std::size_t             bytes  = 0;
    Write_ticket            ticket = 0;
    int                     error  = 0;
};

/// Read end of an anonymous pipe. The owning process reads without blocking:
/// WOULD_BLOCK means "wait on waitable() first". The end handed to a child
/// stays a plain blocking handle so the child's stdio behaves as it expects.
class Readable_pipe
{
public:
    Readable_pipe();
    explicit Readable_pipe(Native_handle in_handle);
    ~Readable_pipe();

    Readable_pipe(Readable_pipe&& other) noexcept;
    Readable_pipe& operator=(Readable_pipe&& other) noexcept;
    Readable_pipe(const Readable_pipe&)            = delete;
    Readable_pipe& operator=(const Readable_pipe&) = delete;

    bool          valid() const;
    Native_handle native() const;      // the pipe handle itself (inherit this)
    Native_handle waitable() const;    // what wait_ready() takes for this end
    void          close();

    /// Makes this end non-blocking for the owning process (call before the
    /// first read; the other end of the pipe is unaffected).
    bool prepare_for_owner(std::string* out_error);

    Io_result read(std::uint8_t* out_buffer, std::size_t in_capacity);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// Write end of an anonymous pipe, same ownership rules as Readable_pipe.
class Writable_pipe
{
public:
    Writable_pipe();
    explicit Writable_pipe(Native_handle in_handle);
    ~Writable_pipe();

    Writable_pipe(Writable_pipe&& other) noexcept;
    Writable_pipe& operator=(Writable_pipe&& other) noexcept;
    Writable_pipe(const Writable_pipe&)            = delete;
    Writable_pipe& operator=(const Writable_pipe&) = delete;

    bool          valid() const;
    Native_handle native() const;
    Native_handle waitable() const;
    void          close();

    bool prepare_for_owner(std::string* out_error);

    /// Copies and commits one write, or accepts nothing when BUSY. A pending
    /// copy belongs to this pipe until complete_write() consumes its ticket;
    /// the completion call takes no caller buffer and a wrong ticket fails.
    Write_submission_result begin_write(const std::uint8_t* in_data, std::size_t in_size);
    Io_result               complete_write(Write_ticket in_ticket);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// Creates an owner-only pipe whose two ends are asynchronous-ready and
/// close-on-exec / non-inheritable. Neither end is child stdio; use one of the
/// child-oriented constructors below for inheritance.
bool create_pipe(Readable_pipe* out_read_end, Writable_pipe* out_write_end, std::string* out_error);

/// Creates a stdio pipe whose owner end supports asynchronous readiness and
/// whose child end is a synchronous handle suitable for inherited stdio.
bool create_child_pipe_owner_reads(
    Readable_pipe* out_owner_read,
    Writable_pipe* out_child_write,
    std::string* out_error);
bool create_child_pipe_owner_writes(
    Writable_pipe* out_owner_write,
    Readable_pipe* out_child_read,
    std::string* out_error);

/// One readiness query for wait_ready(): which handle, which directions the
/// caller wants, and what the wait observed.
struct Wait_entry
{
    Native_handle handle     = k_invalid_handle;
    bool          want_read  = false;
    bool          want_write = false;

    bool readable = false;
    bool writable = false;
    bool closed   = false;   // peer gone (POLLHUP); a final read may still return buffered bytes
    bool failed   = false;   // POLLERR / POLLNVAL
};

/// Blocks until at least one entry is ready or timeout_ms (-1: no timeout)
/// passes. Returns the number of entries with an observation, 0 on timeout,
/// -1 on failure (platform error in *out_error when given).
int wait_ready(Wait_entry* in_out_entries, std::size_t in_count, int in_timeout_ms, int* out_error = nullptr);

} // namespace vnm::process_custody
