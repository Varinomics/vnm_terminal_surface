#pragma once

#include <vnm_process_custody/process_pipe.h>

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

namespace vnm::process_custody::detail {

inline std::string writer_last_error_text(const char* in_call)
{
    return std::string(in_call) + " failed: error " + std::to_string(GetLastError());
}

class Overlapped_writer
{
public:
    HANDLE handle = nullptr;

    bool prepare(std::string* out_error)
    {
        m_event = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        if (!m_event) {
            *out_error = writer_last_error_text("CreateEventW");
            return false;
        }
        return true;
    }

    HANDLE waitable() const { return m_event; }

    Write_submission_result begin_write(const std::uint8_t* in_data, std::size_t in_size)
    {
        collect();
        Write_submission_result result;
        if (m_ticket != 0) {
            result.status = Write_submission_status::BUSY;
            return result;
        }
        if (m_closed) {
            result.status = Write_submission_status::CLOSED;
            return result;
        }
        if (m_error != 0) {
            result.error = m_error;
            return result;
        }

        m_buffer.assign(in_data, in_data + in_size);
        ResetEvent(m_event);
        std::memset(&m_overlapped, 0, sizeof(m_overlapped));
        m_overlapped.hEvent = m_event;
        DWORD transferred = 0;
        if (WriteFile(
                handle,
                m_buffer.data(),
                static_cast<DWORD>(m_buffer.size()),
                &transferred,
                &m_overlapped))
        {
            SetEvent(m_event);
            result.status = Write_submission_status::TRANSFERRED;
            result.bytes  = static_cast<std::size_t>(transferred);
            m_buffer.clear();
            return result;
        }

        const DWORD code = GetLastError();
        if (code == ERROR_IO_PENDING) {
            m_ticket       = next_ticket();
            m_pending      = true;
            result.status  = Write_submission_status::PENDING;
            result.ticket  = m_ticket;
            return result;
        }
        SetEvent(m_event);
        m_buffer.clear();
        if (is_closed_error(code)) {
            m_closed      = true;
            result.status = Write_submission_status::CLOSED;
            return result;
        }
        m_error      = static_cast<int>(code);
        result.error = m_error;
        return result;
    }

    Io_result complete_write(Write_ticket in_ticket)
    {
        collect();
        Io_result result;
        if (in_ticket == 0 || in_ticket != m_ticket) {
            result.error = static_cast<int>(ERROR_INVALID_PARAMETER);
            return result;
        }
        if (m_pending) {
            result.status = Io_status::WOULD_BLOCK;
            return result;
        }
        if (m_closed) {
            result.status = Io_status::CLOSED;
        }
        else
        if (m_error != 0) {
            result.error = m_error;
        }
        else {
            result.status = Io_status::TRANSFERRED;
            result.bytes  = m_completed;
        }
        clear_completion();
        return result;
    }

    void close()
    {
        if (handle) {
            if (m_pending) {
                CancelIoEx(handle, &m_overlapped);
                DWORD transferred = 0;
                GetOverlappedResult(handle, &m_overlapped, &transferred, TRUE);
                m_pending = false;
            }
            CloseHandle(handle);
            handle = nullptr;
        }
        if (m_event) {
            CloseHandle(m_event);
            m_event = nullptr;
        }
        m_buffer.clear();
        m_ticket = 0;
    }

private:
    static bool is_closed_error(DWORD in_code)
    {
        return in_code == ERROR_BROKEN_PIPE ||
            in_code == ERROR_NO_DATA         ||
            in_code == ERROR_HANDLE_EOF;
    }

    Write_ticket next_ticket()
    {
        ++m_next_ticket;
        if (m_next_ticket == 0) {
            ++m_next_ticket;
        }
        return m_next_ticket;
    }

    void collect()
    {
        if (!m_pending) {
            return;
        }
        DWORD transferred = 0;
        if (GetOverlappedResult(handle, &m_overlapped, &transferred, FALSE)) {
            m_pending   = false;
            m_completed = static_cast<std::size_t>(transferred);
            SetEvent(m_event);
            return;
        }
        const DWORD code = GetLastError();
        if (code == ERROR_IO_INCOMPLETE) {
            return;
        }
        m_pending = false;
        if (is_closed_error(code)) {
            m_closed = true;
        }
        else {
            m_error = static_cast<int>(code);
        }
        SetEvent(m_event);
    }

    void clear_completion()
    {
        m_buffer.clear();
        m_completed = 0;
        m_ticket    = 0;
    }

    HANDLE                    m_event = nullptr;
    OVERLAPPED                m_overlapped{};
    std::vector<std::uint8_t> m_buffer;
    std::size_t               m_completed   = 0;
    Write_ticket              m_ticket      = 0;
    Write_ticket              m_next_ticket = 0;
    bool                      m_pending      = false;
    bool                      m_closed       = false;
    int                       m_error        = 0;
};

} // namespace vnm::process_custody::detail
