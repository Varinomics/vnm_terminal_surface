#pragma once

#include <vnm_process_custody/process_pipe.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vnm::process_custody {

enum class Receive_status
{
    MESSAGE,    // one complete message is in the caller's buffer
    TIMEOUT,    // nothing arrived within the timeout
    CLOSED,     // the peer closed its end
    OVERSIZE,   // the peer sent more than this end's max_message_bytes; the message was discarded
    FAILED,     // error holds the platform error code, or 0 for a malformed length prefix
};

struct Receive_result
{
    Receive_status             status = Receive_status::FAILED;
    std::size_t                size   = 0;   // payload bytes in the caller's buffer
    std::vector<Native_handle> descriptors;  // handles that travelled with the message (Linux only); the caller owns them
    int                        error  = 0;
};

/// The message channel of the control protocol (desktop <-> custodian) and of
/// the custodian <-> supervisor status report. Each message is sent and
/// received whole as [length:4 big-endian][payload] with payload bounded by
/// this end's max_message_bytes; Linux carries it as one AF_UNIX
/// SOCK_SEQPACKET datagram (whose boundaries make the prefix redundant but
/// verifiable), Windows as one write on a named pipe. Descriptors travel with
/// a message as SCM_RIGHTS on Linux; Windows transfers none, and
/// transfers_descriptors() lets a caller branch without a platform test.
class Duplex_channel
{
public:
    Duplex_channel();
    Duplex_channel(Native_handle in_end, std::size_t in_max_message_bytes);
    ~Duplex_channel();

    Duplex_channel(Duplex_channel&& other) noexcept;
    Duplex_channel& operator=(Duplex_channel&& other) noexcept;
    Duplex_channel(const Duplex_channel&)            = delete;
    Duplex_channel& operator=(const Duplex_channel&) = delete;

    /// Two connected ends, both close-on-exec / non-inheritable until one is
    /// named in a Spawn_request.
    static bool create_pair(
        std::size_t     in_max_message_bytes,
        Duplex_channel* out_first,
        Duplex_channel* out_second,
        std::string*    out_error);

    static bool transfers_descriptors();

    bool          valid() const;
    Native_handle native() const;
    Native_handle waitable() const;
    std::size_t   max_message_bytes() const;
    void          close();

    /// Arms one channel-owned record read. waitable() is a pure query and is
    /// invalid until this succeeds on Windows; POSIX needs no OS preparation.
    bool prepare_for_owner(std::string* out_error);

    /// Narrows the first Windows read of a record to force ERROR_MORE_DATA in
    /// the focused primitive test. This is accepted only before preparation;
    /// production callers never set it. POSIX accepts and ignores the value.
    bool set_receive_staging_bytes_for_tests(std::size_t in_bytes);

    /// Blocking send of one whole message. CLOSED when the peer is gone.
    Io_status send(const std::uint8_t* in_payload, std::size_t in_size, int* out_error = nullptr);
    Io_status send_descriptors(
        const std::uint8_t*               in_payload,
        std::size_t                       in_size,
        const std::vector<Native_handle>& in_descriptors,
        int*                              out_error = nullptr);

    /// Waits up to timeout_ms (-1: no timeout) for one message; in_capacity
    /// must be at least max_message_bytes() so a message the peer was allowed
    /// to send always fits.
    Receive_result receive(std::uint8_t* out_buffer, std::size_t in_capacity, int in_timeout_ms);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// The byte stream of the supervisor link (desktop <-> supervisor). Framing
/// belongs to logonomic_cli_addon_supervisor_link.h; this class only moves
/// bytes. The owning process reads without blocking after
/// prepare_for_owner(); writes are non-blocking and expose a separate write
/// waitable so a duplex owner can keep draining the peer while its own
/// outbound queue is back-pressured.
class Stream_channel
{
public:
    Stream_channel();
    explicit Stream_channel(Native_handle in_end);
    ~Stream_channel();

    Stream_channel(Stream_channel&& other) noexcept;
    Stream_channel& operator=(Stream_channel&& other) noexcept;
    Stream_channel(const Stream_channel&)            = delete;
    Stream_channel& operator=(const Stream_channel&) = delete;

    static bool create_pair(Stream_channel* out_first, Stream_channel* out_second, std::string* out_error);

    bool          valid() const;
    Native_handle native() const;
    Native_handle waitable() const;
    Native_handle write_waitable() const;
    void          close();

    bool prepare_for_owner(std::string* out_error);

    Io_result read(std::uint8_t* out_buffer, std::size_t in_capacity);
    Write_submission_result begin_write(const std::uint8_t* in_data, std::size_t in_size);
    Io_result               complete_write(Write_ticket in_ticket);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vnm::process_custody
