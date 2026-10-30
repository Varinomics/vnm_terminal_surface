#include "helpers/decode_hex.h"
#include "helpers/test_check.h"
#include "../posix_pty_backend/callback_lifetime_checks.h"
#include "../posix_pty_backend/native_session_checks.h"
#include "../../src/native_backend_io_core.h"
#include "../../src/native_backend_cleanup_owner.h"
#include "vnm_terminal/internal/terminal_canvas_fixture_contract.h"
#include "vnm_terminal/internal/terminal_content_tail_projection.h"
#include "vnm_terminal/internal/terminal_input_encoder.h"
#include "vnm_terminal/internal/terminal_screen_model.h"
#include "vnm_terminal/internal/windows_conpty_backend.h"
#include "vnm_terminal/process_cpu_time.h"
#include "vnm_terminal/process_snapshot.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QKeyEvent>
#include <QProcess>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>
#include <cstddef>
#include <cwchar>

namespace term = vnm_terminal::internal;

namespace {

// Bounds how long a scenario waits for the backend to deliver expected output.
// It is a liveness bound, not a latency assertion: a healthy round trip takes
// milliseconds, and a genuine product hang never completes, so the value only
// has to exceed the worst scheduling delay a loaded host can impose. Ten
// seconds was not enough under CPU saturation with ASan.
constexpr std::chrono::milliseconds k_wait_timeout(20000);
// Filesystem markers are written by a freshly started PowerShell fixture, so
// this covers process cold start rather than backend responsiveness. Ten
// seconds proved too tight on loaded ASan CI runners. Only five call sites use
// it, and each returns as soon as its marker appears, so the extra tolerance
// costs nothing on a healthy run and stays inside the 120 second CTest budget
// even if several fixtures genuinely fail.
constexpr std::chrono::milliseconds k_file_marker_wait_timeout(20000);
constexpr std::chrono::milliseconds k_console_host_exit_timeout(1000);
constexpr std::uint64_t k_cpu_accounting_work_usec = 100'000U;
constexpr int k_cpu_accounting_failure_resample_count = 20;
constexpr std::chrono::milliseconds k_cpu_accounting_failure_resample_interval(10);
// The slowest recorded cold ASan completion of this entire test executable was
// 57.73 seconds, and it reached 80 seconds under full CPU saturation. Bound the
// first scenario's progressing-output phase at 60 seconds, comfortably inside
// the 300-second CTest budget even when the rest of the suite runs slowly.
constexpr std::chrono::milliseconds k_progressing_output_absolute_timeout(60000);

using vnm_terminal::test_helpers::check;
using vnm_terminal::test_helpers::decode_hex;

std::size_t count_occurrences(const QByteArray& haystack, const QByteArray& needle)
{
    std::size_t count  = 0U;
    qsizetype   offset = 0;
    for (;;) {
        offset = haystack.indexOf(needle, offset);
        if (offset < 0) {
            return count;
        }

        ++count;
        offset += needle.size();
    }
}

bool output_contains_in_order(
    const QByteArray&              output,
    const std::vector<QByteArray>& needles,
    const char*                    message)
{
    qsizetype offset = 0;
    for (const QByteArray& needle : needles) {
        offset = output.indexOf(needle, offset);
        if (offset < 0) {
            std::cerr << "missing ordered output payload: "
                << needle.toHex(' ').constData() << '\n';
            return check(false, message);
        }

        offset += needle.size();
    }

    return true;
}

int parser_failure_count(const term::Terminal_screen_model_result& result)
{
    int count = 0;
    for (const term::Parser_action& action : result.actions) {
        const auto* diagnostic = std::get_if<term::Parser_payload_diagnostic>(&action.payload);
        if (diagnostic != nullptr && diagnostic->code != term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE) {
            ++count;
        }
    }

    return count;
}

bool wait_for_file(const QString& path)
{
    const auto deadline =
        std::chrono::steady_clock::now() + k_file_marker_wait_timeout;
    do {
        if (QFileInfo::exists(path)) {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    while (std::chrono::steady_clock::now() < deadline);

    return false;
}

bool write_gate_file(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::cerr << "failed to open gate file: "
            << file.errorString().toLocal8Bit().constData() << '\n';
        return false;
    }

    return file.write(QByteArrayLiteral("go\n")) == 3;
}

std::vector<DWORD> current_process_console_host_children()
{
    std::vector<DWORD> pids;
    const DWORD        current_pid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return pids;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot, &entry)) {
        CloseHandle(snapshot);
        return pids;
    }

    do {
        if (entry.th32ParentProcessID == current_pid &&
            std::wcscmp(entry.szExeFile, L"OpenConsole.exe") == 0)
        {
            pids.push_back(entry.th32ProcessID);
        }
    }
    while (Process32NextW(snapshot, &entry));

    CloseHandle(snapshot);
    return pids;
}

bool wait_for_console_host_children_to_exit(std::string_view test_name)
{
    const auto deadline = std::chrono::steady_clock::now() + k_console_host_exit_timeout;
    std::vector<DWORD> pids;
    do {
        pids = current_process_console_host_children();
        if (pids.empty()) {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    while (std::chrono::steady_clock::now() < deadline);

    std::cerr << "ConPTY test left OpenConsole.exe child processes after "
        << test_name << ':';
    for (const DWORD pid : pids) {
        std::cerr << ' ' << pid;
    }
    std::cerr << '\n';
    return check(false, "ConPTY backend releases console host child processes");
}

std::optional<DWORD> parse_pid_after_prefix(
    const QByteArray&  output,
    const QByteArray&  prefix)
{
    const qsizetype prefix_offset = output.indexOf(prefix);
    if (prefix_offset < 0) {
        return std::nullopt;
    }

    const qsizetype pid_offset = prefix_offset + prefix.size();
    const qsizetype line_end   = output.indexOf('\n', pid_offset);
    if (line_end < 0) {
        return std::nullopt;
    }

    qsizetype pid_end = line_end;
    if (pid_end > pid_offset && output[pid_end - 1] == '\r') {
        --pid_end;
    }

    if (pid_end <= pid_offset) {
        return std::nullopt;
    }

    unsigned long long pid = 0U;
    for (qsizetype offset = pid_offset; offset < pid_end; ++offset) {
        const char ch = output[offset];
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }

        pid = pid * 10U + static_cast<unsigned long long>(ch - '0');
        if (pid > std::numeric_limits<DWORD>::max()) {
            return std::nullopt;
        }
    }

    if (pid == 0U) {
        return std::nullopt;
    }

    return static_cast<DWORD>(pid);
}

bool test_pid_parser_requires_line_delimiter()
{
    bool ok = true;

    const QByteArray prefix = QByteArrayLiteral("hold-open-pid-no-read ");
    ok &= check(!parse_pid_after_prefix(
        QByteArrayLiteral("hold-open-pid-no-read 1"),
        prefix).has_value(),
        "PID parser waits for a complete line before accepting digits");
    ok &= check(!parse_pid_after_prefix(
        QByteArrayLiteral("hold-open-pid-no-read 123x\n"),
        prefix).has_value(),
        "PID parser rejects non-digit PID line content");

    const std::optional<DWORD> pid = parse_pid_after_prefix(
        QByteArrayLiteral("hold-open-pid-no-read 12345\r\n"),
        prefix);
    ok &= check(pid.has_value() && *pid == 12345U,
        "PID parser accepts a complete CRLF-delimited PID line");

    return ok;
}

class Win32_process_handle
{
public:
    Win32_process_handle() = default;

    explicit Win32_process_handle(HANDLE handle)
    :
        m_handle(handle)
    {}

    ~Win32_process_handle()
    {
        reset();
    }

    Win32_process_handle(const Win32_process_handle&)            = delete;
    Win32_process_handle& operator=(const Win32_process_handle&) = delete;

    Win32_process_handle(Win32_process_handle&& other) noexcept
    :
        m_handle(other.m_handle)
    {
        other.m_handle = nullptr;
    }

    Win32_process_handle& operator=(Win32_process_handle&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_handle = other.m_handle;
            other.m_handle = nullptr;
        }

        return *this;
    }

    void reset(HANDLE handle = nullptr)
    {
        if (m_handle != nullptr) {
            CloseHandle(m_handle);
        }

        m_handle = handle;
    }

    bool   is_valid() const { return m_handle != nullptr; }
    HANDLE get()      const { return m_handle;            }

private:
    HANDLE m_handle = nullptr;
};

Win32_process_handle open_process_handle(DWORD pid)
{
    return Win32_process_handle(
        OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
            FALSE,
            pid));
}

std::optional<bool> process_handle_is_running(HANDLE process)
{
    const DWORD wait_result = WaitForSingleObject(process, 0U);
    if (wait_result == WAIT_TIMEOUT) {
        return true;
    }

    if (wait_result == WAIT_OBJECT_0) {
        return false;
    }

    std::cerr << "WaitForSingleObject(process, 0) failed: " << GetLastError() << '\n';
    return std::nullopt;
}

bool wait_for_process_exit(HANDLE process)
{
    const auto deadline = std::chrono::steady_clock::now() + k_wait_timeout;
    do {
        const DWORD wait_result = WaitForSingleObject(process, 10U);
        if (wait_result == WAIT_OBJECT_0) {
            return true;
        }

        if (wait_result == WAIT_FAILED) {
            std::cerr << "WaitForSingleObject(process, 10) failed: " << GetLastError() << '\n';
            return false;
        }
    }
    while (std::chrono::steady_clock::now() < deadline);

    const DWORD wait_result = WaitForSingleObject(process, 0U);
    if (wait_result == WAIT_FAILED) {
        std::cerr << "WaitForSingleObject(process, 0) failed: " << GetLastError() << '\n';
        return false;
    }

    return wait_result == WAIT_OBJECT_0;
}

std::optional<DWORD> process_exit_code(HANDLE process)
{
    DWORD exit_code = 0U;
    if (!GetExitCodeProcess(process, &exit_code)) {
        std::cerr << "GetExitCodeProcess failed: " << GetLastError() << '\n';
        return std::nullopt;
    }

    return exit_code;
}

void terminate_process_if_running(HANDLE process)
{
    DWORD exit_code = 0U;
    if (GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE) {
        (void)TerminateProcess(process, 1U);
        (void)WaitForSingleObject(process, 5000U);
    }
}

std::optional<QByteArray> conpty_observable_output_payload(
    const term::terminal_canvas_fixture_record_t& record)
{
    if (record.label == std::string_view("enter-alternate-screen") ||
        record.label == std::string_view("leave-alternate-screen"))
    {
        return std::nullopt;
    }

    if (record.label == std::string_view("prompt")) {
        return QByteArrayLiteral("term>");
    }

    if (record.label == std::string_view("prompt-editing-keys-ack")) {
        return QByteArrayLiteral("input prompt-editing-keys ok");
    }

    if (record.label == std::string_view("resize-report")) {
        return QByteArrayLiteral("resize 33x120");
    }

    if (record.label == std::string_view("bracketed-paste-ack")) {
        return QByteArrayLiteral("input bracketed-paste ok");
    }

    if (record.label == std::string_view("focus-reporting-ack")) {
        return QByteArrayLiteral("input focus-reporting ok");
    }

    if (record.label == std::string_view("mouse-sgr-1006-ack")) {
        return QByteArrayLiteral("input mouse-sgr-1006 ok");
    }

    if (record.label == term::k_terminal_canvas_fixture_enable_input_modes_label) {
        return std::nullopt;
    }

    if (record.label == std::string_view("reply-handling") &&
        record.kind  == term::Terminal_canvas_fixture_record_kind::OUTPUT)
    {
        return std::nullopt;
    }

    if (record.label == std::string_view("high-volume-streaming")) {
        return QByteArrayLiteral("stream-row");
    }

    if (record.kind == term::Terminal_canvas_fixture_record_kind::OUTPUT ||
        record.kind == term::Terminal_canvas_fixture_record_kind::REPEAT_OUTPUT)
    {
        return decode_hex(record.payload_hex);
    }

    return std::nullopt;
}

QByteArray fixture_record_payload(
    term::Terminal_canvas_fixture_record_kind kind,
    std::string_view                          label)
{
    for (const term::terminal_canvas_fixture_record_t& record :
        term::terminal_canvas_fixture_contract_script())
    {
        if (record.kind  == kind &&
            record.label == label)
        {
            return decode_hex(record.payload_hex);
        }
    }

    return {};
}

// The first CSI report in a scripted reply that ends in final_byte, such as
// the device attributes report ('c') or the cursor position report ('R').
QByteArray scripted_csi_report(const QByteArray& reply, char final_byte)
{
    for (qsizetype start = reply.indexOf("\x1b[");
         start >= 0;
         start = reply.indexOf("\x1b[", start + 2))
    {
        qsizetype end = start + 2;
        while (end < reply.size() &&
               (reply[end] == '?' || reply[end] == ';' ||
                (reply[end] >= '0' && reply[end] <= '9')))
        {
            ++end;
        }
        if (end < reply.size() && reply[end] == final_byte) {
            return reply.mid(start, end - start + 1);
        }
    }

    return {};
}

bool scripted_output_is_ordered(const QByteArray& output)
{
    qsizetype offset = 0;
    for (const term::terminal_canvas_fixture_record_t& record :
        term::terminal_canvas_fixture_contract_script())
    {
        const std::optional<QByteArray> payload =
            conpty_observable_output_payload(record);
        if (!payload.has_value()) {
            continue;
        }

        if (record.kind == term::Terminal_canvas_fixture_record_kind::OUTPUT) {
            offset = output.indexOf(*payload, offset);
            if (offset < 0) {
                std::cerr << "missing ordered output record: " << record.label << '\n';
                return false;
            }

            offset += payload->size();
        }
        else
        if (record.kind == term::Terminal_canvas_fixture_record_kind::REPEAT_OUTPUT) {
            for (int i = 0; i < record.repeat_count; ++i) {
                offset = output.indexOf(*payload, offset);
                if (offset < 0) {
                    std::cerr << "missing repeated output record: "
                        << record.label << " at repeat " << i << '\n';
                    return false;
                }

                offset += payload->size();
            }
        }
    }

    return true;
}

class Backend_capture
{
public:
    term::Terminal_backend_callbacks callbacks(
        std::function<void(const QByteArray&)> output_observer = {})
    {
        term::Terminal_backend_callbacks callbacks;
        callbacks.output_received = [this, output_observer](QByteArray bytes) {
            const QByteArray observed_bytes = bytes;

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_output.append(bytes);
                m_output_events.push_back({bytes, m_next_event_sequence++});
            }

            if (output_observer) {
                output_observer(observed_bytes);
            }
            m_cv.notify_all();
        };
        callbacks.process_exited = [this](term::Terminal_backend_exit exit) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_exit = exit;
            ++m_exit_count;
            m_exit_event_sequence = m_next_event_sequence++;
            m_cv.notify_all();
        };
        callbacks.error_reported = [this](term::Terminal_backend_error error) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_errors.push_back(std::move(error));
            m_cv.notify_all();
        };
        callbacks.resize_completed = [this](term::Terminal_backend_resize_completion completion) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_resize_completions.push_back(std::move(completion));
            m_cv.notify_all();
        };
        return callbacks;
    }

    bool wait_until(const std::function<bool()>& predicate)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, k_wait_timeout, predicate);
    }

    bool wait_for_output(const QByteArray& needle)
    {
        return wait_until([&] {
            return m_output.contains(needle);
        });
    }

    bool wait_for_output_within(
        const QByteArray&          needle,
        std::chrono::milliseconds  interval)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, interval, [&] {
            return m_output.contains(needle);
        });
    }

    bool wait_for_output_matching_within(
        const std::function<bool(const QByteArray&)>&
                                   predicate,
        std::chrono::milliseconds  interval)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, interval, [&] {
            return predicate(m_output);
        });
    }

    std::optional<DWORD> wait_for_pid_output(const QByteArray& prefix)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        const bool ready = m_cv.wait_for(lock, k_wait_timeout, [&] {
            return parse_pid_after_prefix(m_output, prefix).has_value();
        });
        if (!ready) {
            return std::nullopt;
        }

        return parse_pid_after_prefix(m_output, prefix);
    }

    bool wait_for_exit()
    {
        return wait_until([&] {
            return m_exit.has_value();
        });
    }

    bool wait_for_exit_while_output_progresses(
        std::chrono::milliseconds absolute_timeout =
            k_progressing_output_absolute_timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        std::size_t observed_output_events = m_output_events.size();
        const auto wait_started      = std::chrono::steady_clock::now();
        const auto absolute_deadline = wait_started + absolute_timeout;
        const auto report_timeout = [&](
            bool                                  absolute_limit_reached,
            std::chrono::steady_clock::time_point wait_ended)
        {
            if (absolute_limit_reached) {
                std::cerr << "exit wait exceeded absolute limit: absolute_limit_ms="
                    << absolute_timeout.count();
            }
            else {
                std::cerr << "exit wait made no output progress: inactivity_limit_ms="
                    << k_wait_timeout.count();
            }
            std::cerr
                << " elapsed_ms="
                << std::chrono::duration_cast<std::chrono::milliseconds>(
                    wait_ended - wait_started).count()
                << " output_bytes=" << m_output.size()
                << " output_events=" << m_output_events.size()
                << " backend_errors=" << m_errors.size()
                << '\n';
        };
        while (!m_exit.has_value()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= absolute_deadline) {
                report_timeout(true, now);
                return false;
            }

            const auto inactivity_deadline = now + k_wait_timeout;
            const auto next_deadline = std::min(inactivity_deadline, absolute_deadline);
            const bool state_changed = m_cv.wait_until(lock, next_deadline, [&] {
                return
                    m_exit.has_value() ||
                    m_output_events.size() != observed_output_events;
            });
            if (!state_changed) {
                const auto wait_ended = std::chrono::steady_clock::now();
                report_timeout(wait_ended >= absolute_deadline, wait_ended);
                return false;
            }

            observed_output_events = m_output_events.size();
        }

        return true;
    }

    bool wait_for_exit_within(std::chrono::milliseconds interval)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, interval, [&] {
            return m_exit.has_value();
        });
    }

    bool wait_for_resize_completion_count(
        std::size_t               count,
        std::chrono::milliseconds timeout = k_wait_timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, timeout, [&] {
            return m_resize_completions.size() >= count;
        });
    }

    QByteArray output_snapshot()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_output;
    }

    std::optional<term::Terminal_backend_exit> exit_snapshot()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_exit;
    }

    int exit_count_snapshot()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_exit_count;
    }

    std::vector<term::Terminal_backend_error> errors_snapshot()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_errors;
    }

    std::vector<term::Terminal_backend_resize_completion>
    resize_completions_snapshot()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_resize_completions;
    }

    std::vector<QByteArray> output_events_snapshot()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::vector<QByteArray> events;
        events.reserve(m_output_events.size());
        for (const Output_event& event : m_output_events) {
            events.push_back(event.bytes);
        }
        return events;
    }

    std::size_t output_event_count_snapshot()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_output_events.size();
    }

    // Blocks until at least `minimum` output events have been observed. Lets a
    // test take producer start-up latency out of a bounded measurement window
    // instead of assuming a background thread gets scheduled in time.
    bool wait_for_output_event_count(
        std::size_t                minimum,
        std::chrono::milliseconds  timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, timeout, [&] {
            return m_output_events.size() >= minimum;
        });
    }

    bool output_precedes_exit(const QByteArray& needle)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_exit_event_sequence.has_value()) {
            return false;
        }

        QByteArray output_prefix;
        for (const Output_event& event : m_output_events) {
            output_prefix += event.bytes;
            if (output_prefix.contains(needle) &&
                event.sequence < *m_exit_event_sequence)
            {
                return true;
            }
        }

        return false;
    }

private:
    struct Output_event
    {
        QByteArray     bytes;
        std::size_t    sequence = 0U;
    };

    std::mutex                 m_mutex;
    std::condition_variable    m_cv;
    QByteArray                 m_output;
    std::vector<Output_event>  m_output_events;
    std::optional<term::Terminal_backend_exit>
                               m_exit;
    std::optional<std::size_t> m_exit_event_sequence;
    std::vector<term::Terminal_backend_error>
                               m_errors;
    std::vector<term::Terminal_backend_resize_completion>
                               m_resize_completions;
    std::size_t                m_next_event_sequence = 0U;
    int                        m_exit_count = 0;
};

const char* backend_error_code_name(term::Terminal_backend_error_code code)
{
    switch (code) {
        case term::Terminal_backend_error_code::INVALID_LAUNCH_CONFIG:         return "INVALID_LAUNCH_CONFIG";
        case term::Terminal_backend_error_code::INVALID_INITIAL_GRID_SIZE:     return "INVALID_INITIAL_GRID_SIZE";
        case term::Terminal_backend_error_code::WORKING_DIRECTORY_UNAVAILABLE: return "WORKING_DIRECTORY_UNAVAILABLE";
        case term::Terminal_backend_error_code::START_FAILED:                  return "START_FAILED";
        case term::Terminal_backend_error_code::WRITE_FAILED:                  return "WRITE_FAILED";
        case term::Terminal_backend_error_code::RESIZE_FAILED:                 return "RESIZE_FAILED";
        case term::Terminal_backend_error_code::INTERRUPT_FAILED:              return "INTERRUPT_FAILED";
        case term::Terminal_backend_error_code::TERMINATE_FAILED:              return "TERMINATE_FAILED";
        case term::Terminal_backend_error_code::OUTPUT_OVERFLOW:               return "OUTPUT_OVERFLOW";
        case term::Terminal_backend_error_code::CALLBACK_MISSING:              return "CALLBACK_MISSING";
        case term::Terminal_backend_error_code::READ_FAILED:                   return "READ_FAILED";
    }

    return "UNKNOWN";
}

bool check_no_backend_errors(Backend_capture& capture, const char* message)
{
    const std::vector<term::Terminal_backend_error> errors =
        capture.errors_snapshot();
    if (errors.empty()) {
        return check(true, message);
    }

    std::cerr << message << ": " << errors.size()
        << " backend error(s)\n";
    for (const term::Terminal_backend_error& error : errors) {
        const QByteArray error_message = error.message.toLocal8Bit();
        std::cerr << "  "
            << backend_error_code_name(error.code)
            << ": "
            << error_message.constData()
            << '\n';
    }

    return check(false, message);
}

// `required` is false for a caller the in-flight write is a precondition of
// rather than the subject of: the state dump is still printed, but the caller
// decides whether not reaching it is a failure or a run that cannot exercise
// what it set out to.
bool wait_for_in_flight_write(
    term::Windows_conpty_backend& backend,
    std::size_t                   minimum_bytes,
    const char*                   message,
    bool                          required = true)
{
    term::Windows_conpty_backend_write_state_for_testing last_state;
    const auto deadline = std::chrono::steady_clock::now() + k_wait_timeout;
    do {
        last_state = backend.write_state_for_testing();
        if (last_state.in_flight_write_bytes >= minimum_bytes) {
            return check(true, message);
        }

        if (last_state.stopping || last_state.writer_failed) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    while (std::chrono::steady_clock::now() < deadline);

    std::cerr << message << ": write state queued_bytes="
        << last_state.queued_write_bytes
        << " queued_count=" << last_state.queued_write_count
        << " in_flight_bytes=" << last_state.in_flight_write_bytes
        << " running=" << last_state.running
        << " stopping=" << last_state.stopping
        << " writer_failed=" << last_state.writer_failed
        << '\n';
    return required ? check(false, message) : false;
}

// Waits until an in-flight write has reached a terminal outcome, without
// requiring which one. Use this when the child provably never consumes the
// bytes and only the host decides whether the pending write ends up failing or
// completing at teardown.
bool wait_for_write_settled(
    term::Windows_conpty_backend&                               backend,
    const term::Windows_conpty_backend_write_state_for_testing& before,
    const char*                                                 message)
{
    term::Windows_conpty_backend_write_state_for_testing last_state;
    const auto deadline = std::chrono::steady_clock::now() + k_wait_timeout;
    do {
        last_state = backend.write_state_for_testing();
        if (last_state.failed_write_count     != before.failed_write_count ||
            last_state.successful_write_count != before.successful_write_count)
        {
            return check(true, message);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    while (std::chrono::steady_clock::now() < deadline);

    std::cerr << message
        << ": successful_before=" << before.successful_write_count
        << " successful_after=" << last_state.successful_write_count
        << " failed_before=" << before.failed_write_count
        << " failed_after=" << last_state.failed_write_count
        << " in_flight_bytes=" << last_state.in_flight_write_bytes
        << " stopping=" << last_state.stopping
        << " writer_failed=" << last_state.writer_failed
        << '\n';
    return check(false, message);
}

term::Terminal_launch_config launch_config(
    const QString& fixture_path,
    QStringList    arguments)
{
    term::Terminal_launch_config config;
    config.argv = {fixture_path};
    config.argv.append(arguments);
    config.working_directory  = QFileInfo(fixture_path).absolutePath();
    config.initial_grid_size  = term::terminal_grid_size_t{24, 80};
    config.identity.term      = QStringLiteral("xterm-256color");
    config.identity.colorterm = QStringLiteral("truecolor");
    return config;
}

struct cpu_accounting_process_times_t
{
    std::uint64_t kernel_ticks   = 0;
    std::uint64_t user_ticks     = 0;
    std::uint64_t creation_ticks = 0;
    std::uint64_t exit_ticks     = 0;
    std::int64_t  before_qpc     = 0;
    std::int64_t  after_qpc      = 0;
    DWORD         pid            = 0;
    DWORD         signal         = WAIT_FAILED;
    DWORD         error          = 0;
};

std::optional<std::uint64_t> process_cpu_time_usec(
    HANDLE                          process,
    cpu_accounting_process_times_t* out_times = nullptr,
    bool                            report_error = true)
{
    FILETIME creation{}, exit{}, kernel{}, user{};
    LARGE_INTEGER before{}, after{};
    if (out_times) {
        QueryPerformanceCounter(&before);
    }
    const bool queried = GetProcessTimes(process, &creation, &exit, &kernel, &user);
    const DWORD error = queried ? ERROR_SUCCESS : GetLastError();
    if (out_times) {
        QueryPerformanceCounter(&after);
    }
    if (!queried) {
        if (out_times) {
            out_times->error = error;
        }
        if (report_error) {
            std::cerr << "GetProcessTimes failed: " << error << '\n';
        }
        return std::nullopt;
    }
    const auto ticks = [](FILETIME time) {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    if (out_times) {
        *out_times = {ticks(kernel), ticks(user), ticks(creation), ticks(exit),
            before.QuadPart, after.QuadPart, GetProcessId(process), WaitForSingleObject(process, 0)};
    }
    return (ticks(kernel) + ticks(user)) / 10;
}

void write_cpu_process_times(std::ostream& output, const cpu_accounting_process_times_t& times)
{
    output << " pid=" << times.pid << " kernel_ticks=" << times.kernel_ticks
        << " user_ticks=" << times.user_ticks << " creation_ticks=" << times.creation_ticks
        << " exit_ticks=" << times.exit_ticks << " signal=" << times.signal
        << " qpc=[" << times.before_qpc << ',' << times.after_qpc << "]\n";
}

class Cpu_accounting_diagnostics
{
public:
    void observe(
        const char* event,
        HANDLE job,
        HANDLE root,
        const term::windows_conpty_cpu_query_for_testing_t& query)
    {
        std::ostringstream output;
        output << "Native CPU event=" << event << " qpc=[" << query.before_qpc << ',' << query.after_qpc
            << "] error=" << query.error;
        if (std::string_view(event) == "sample" || std::string_view(event) == "settlement") {
            output << " kernel_ticks=" << query.kernel_ticks << " user_ticks=" << query.user_ticks
                << " active=" << query.active_processes << " total=" << query.total_processes << '\n';
        }
        else {
            // Assignment records bracket admission before the suspended root is resumed.
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            LARGE_INTEGER before{}, after{};
            QueryPerformanceCounter(&before);
            const bool queried = QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                &accounting, sizeof(accounting), nullptr);
            const DWORD error = queried ? ERROR_SUCCESS : GetLastError();
            QueryPerformanceCounter(&after);
            output << "\n  assignment Job qpc=[" << before.QuadPart << ',' << after.QuadPart
                << "] error=" << error << " kernel_ticks=" << accounting.TotalKernelTime.QuadPart
                << " user_ticks=" << accounting.TotalUserTime.QuadPart
                << " active=" << accounting.ActiveProcesses << " total=" << accounting.TotalProcesses << '\n';
        }
        cpu_accounting_process_times_t root_times{};
        output << "  retained root";
        if (process_cpu_time_usec(root, &root_times, false)) {
            write_cpu_process_times(output, root_times);
        }
        else {
            output << " unavailable error=" << root_times.error << '\n';
        }

        // A bounded live snapshot identifies auxiliary members without retaining
        // handles or claiming to reconstruct members that exited between samples.
        struct member_list_t
        {
            DWORD assigned = 0;
            DWORD count = 0;
            ULONG_PTR ids[16]{};
        } members;
        LARGE_INTEGER members_before{}, members_after{};
        QueryPerformanceCounter(&members_before);
        const bool listed = QueryInformationJobObject(job, JobObjectBasicProcessIdList,
            &members, sizeof(members), nullptr);
        const DWORD list_error = listed ? ERROR_SUCCESS : GetLastError();
        QueryPerformanceCounter(&members_after);
        output << "  member snapshot qpc=[" << members_before.QuadPart << ',' << members_after.QuadPart
            << "] error=" << list_error << " assigned=" << members.assigned << " returned=" << members.count
            << " capacity=" << std::size(members.ids) << '\n';
        if (listed || list_error == ERROR_MORE_DATA) {
            for (DWORD index = 0; index < std::min<DWORD>(members.count, std::size(members.ids)); ++index) {
                const DWORD pid = (DWORD)members.ids[index];
                Win32_process_handle member(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
                const DWORD open_error = member.is_valid() ? ERROR_SUCCESS : GetLastError();
                output << "  member listed_pid=" << pid << " open_error=" << open_error;
                if (!member.is_valid()) {
                    output << '\n';
                    continue;
                }
                BOOL in_job = FALSE;
                const bool checked = IsProcessInJob(member.get(), job, &in_job);
                const DWORD membership_error = checked ? ERROR_SUCCESS : GetLastError();
                std::array<wchar_t, 1024> image{};
                DWORD image_size = (DWORD)image.size();
                const bool named = QueryFullProcessImageNameW(member.get(), 0, image.data(), &image_size);
                const DWORD image_error = named ? ERROR_SUCCESS : GetLastError();
                output << " in_job=" << in_job << " membership_error=" << membership_error
                    << " image_error=" << image_error << " image="
                    << (named ? QString::fromWCharArray(image.data(), image_size).toStdString() : std::string{});
                cpu_accounting_process_times_t member_times{};
                if (process_cpu_time_usec(member.get(), &member_times, false)) {
                    write_cpu_process_times(output, member_times);
                }
                else {
                    output << " counters unavailable error=" << member_times.error << '\n';
                }
            }
        }
        const std::lock_guard lock(m_mutex);
        if (std::string_view(event) == "settlement") {
            m_settlement = output.str();
        }
        else
        if (m_records.size() < 32) {
            m_records.push_back(output.str());
        }
        else {
            ++m_dropped_records;
        }
    }

    void dump() const
    {
        const std::lock_guard lock(m_mutex);
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        std::cerr << "Native CPU diagnostics: ticks=100ns qpc_frequency=" << frequency.QuadPart
            << " dropped_records=" << m_dropped_records << '\n';
        for (const auto& record : m_records) {
            std::cerr << record;
        }
        std::cerr << "Last settlement observation:\n" << m_settlement;
    }

private:
    mutable std::mutex       m_mutex;
    std::vector<std::string> m_records;
    std::string              m_settlement;
    unsigned                 m_dropped_records = 0;
};

int run_cpu_accounting_child(const QString& directory)
{
    const QDir gates(directory);
    if (!wait_for_file(gates.filePath(QStringLiteral("start-burn")))) {
        return 1;
    }
    const auto initial_cpu = process_cpu_time_usec(GetCurrentProcess());
    if (!initial_cpu) {
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + k_wait_timeout;
    std::atomic<std::uint64_t> work{0};
    do {
        for (std::uint64_t i = 0; i < 4096; ++i) {
            work.fetch_add(i, std::memory_order_relaxed);
        }
        const auto cpu = process_cpu_time_usec(GetCurrentProcess());
        if (!cpu) {
            return 1;
        }
        // A CPU-time threshold makes the workload independent of host scheduling.
        if (*cpu - *initial_cpu >= k_cpu_accounting_work_usec) {
            if (!write_gate_file(gates.filePath(QStringLiteral("burn-finished")))) {
                return 1;
            }
            return wait_for_file(gates.filePath(QStringLiteral("child-exit"))) ? 0 : 1;
        }
    }
    while (std::chrono::steady_clock::now() < deadline);
    return 1;
}

int run_cpu_accounting_root(const QString& executable_path, const QString& directory)
{
    const std::wstring application = QDir::toNativeSeparators(executable_path).toStdWString();
    std::wstring command = QStringLiteral("\"%1\" --cpu-accounting-child \"%2\"")
        .arg(executable_path, directory).toStdWString();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    // Console detachment must preserve the root's inherited Job membership.
    if (!CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, FALSE,
            DETACHED_PROCESS, nullptr, nullptr, &startup, &process))
    {
        std::cerr << "CPU child CreateProcessW failed: " << GetLastError() << '\n';
        return 1;
    }
    Win32_process_handle child(process.hProcess);
    CloseHandle(process.hThread);
    std::cout << "cpu-root-pid=" << GetCurrentProcessId() << '\n'
        << "cpu-child-pid=" << process.dwProcessId << '\n' << std::flush;
    if (!wait_for_process_exit(child.get()) || process_exit_code(child.get()) != 0U) {
        return 1;
    }
    child.reset();
    std::cout << "cpu-child-exited\n" << std::flush;
    return wait_for_file(QDir(directory).filePath(QStringLiteral("root-exit"))) ? 0 : 1;
}

bool check_cpu_accounting_sample(
    term::Windows_conpty_backend&         backend,
    const Cpu_accounting_diagnostics&     diagnostics,
    std::uint64_t                         minimum_cpu_usec,
    std::string_view                      phase,
    vnm_terminal::process_cpu_time_t*      out_sample,
    const cpu_accounting_process_times_t* root_times = nullptr,
    const cpu_accounting_process_times_t* child_times = nullptr)
{
    const auto before = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto sample = backend.process_cpu_time();
    const auto after = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const std::string label(phase);
    if (!check(sample.has_value(), label + ": retained Job supplies a CPU sample")) {
        return false;
    }
    *out_sample = *sample;
    bool ok = check(sample->cpu_time_usec >= minimum_cpu_usec,
        label + ": Job CPU includes independent root and child process totals");
    ok &= check(sample->scope == vnm_terminal::Process_cpu_scope::WORKLOAD_FAMILY,
        label + ": Job sample declares native family accounting");
    if (!ok) {
        std::cerr << "Job CPU usec=" << sample->cpu_time_usec
            << ", root and child CPU usec=" << minimum_cpu_usec << '\n';
        if (root_times && child_times) {
            std::cerr << "Independent root";
            write_cpu_process_times(std::cerr, *root_times);
            std::cerr << "Independent child";
            write_cpu_process_times(std::cerr, *child_times);
        }
        // Preserve the failed sample and verdict while exposing whether the native total settles later.
        for (int index = 0; index < k_cpu_accounting_failure_resample_count; ++index) {
            std::this_thread::sleep_for(k_cpu_accounting_failure_resample_interval);
            const auto later = backend.process_cpu_time();
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count() - after;
            std::cerr << "Job CPU failure resample=" << index + 1 << " elapsed_usec=" << elapsed;
            if (later) {
                std::cerr << " cpu_usec=" << later->cpu_time_usec
                    << " sampled_at_usec=" << later->sampled_at_usec << '\n';
            }
            else {
                std::cerr << " unavailable\n";
            }
        }
        diagnostics.dump();
    }
    ok &= check(sample->sampled_at_usec >= static_cast<std::uint64_t>(before) &&
            sample->sampled_at_usec <= static_cast<std::uint64_t>(after),
        label + ": sample timestamp uses the native query's monotonic capture interval");
    ok &= check(sample->logical_processor_count > 0 &&
            sample->logical_processor_count == GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
        label + ": normalization counts every active processor group");
    return ok;
}

bool test_cpu_accounting_retains_exited_descendants(const QString& executable_path)
{
    QTemporaryDir directory;
    if (!check(directory.isValid(), "CPU accounting gate directory is available")) {
        return false;
    }
    const QDir gates(directory.path());
    Backend_capture capture;
    auto diagnostics = std::make_shared<Cpu_accounting_diagnostics>();
    term::Windows_conpty_backend backend;
    if (!check(backend.set_cpu_observer_for_testing(
            [diagnostics](const char* event, void* job, void* root,
                const term::windows_conpty_cpu_query_for_testing_t& query)
            {
                diagnostics->observe(event, job, root, query);
            }), "CPU diagnostic observer is installed before admission"))
    {
        return false;
    }
    if (!check(!backend.process_cpu_time(), "CPU is unavailable before Job admission")) {
        return false;
    }
    const auto result = backend.start(launch_config(executable_path,
        {QStringLiteral("--cpu-accounting-root"), directory.path()}), capture.callbacks());
    if (!check(result.code == term::Terminal_backend_result_code::ACCEPTED,
            "CPU accounting fixture starts in the native ConPTY Job"))
    {
        return false;
    }
    const auto root_pid  = capture.wait_for_pid_output(QByteArrayLiteral("cpu-root-pid="));
    const auto child_pid = capture.wait_for_pid_output(QByteArrayLiteral("cpu-child-pid="));
    if (!check(root_pid && child_pid, "CPU accounting root reports both live process identities")) {
        return false;
    }
    auto root  = open_process_handle(*root_pid);
    auto child = open_process_handle(*child_pid);
    if (!check(root.is_valid() && child.is_valid(), "CPU accounting holds both exact process handles")) {
        return false;
    }
    vnm_terminal::process_cpu_time_t initial{}, live{}, child_exited{}, root_exited{};
    if (!check_cpu_accounting_sample(backend, *diagnostics, 0, "before workload", &initial) ||
        !check(write_gate_file(gates.filePath(QStringLiteral("start-burn"))), "CPU child is released") ||
        !check(wait_for_file(gates.filePath(QStringLiteral("burn-finished"))), "CPU child finishes its work"))
    {
        return false;
    }
    cpu_accounting_process_times_t live_root_times{}, live_child_times{};
    const auto live_root_cpu  = process_cpu_time_usec(root.get(), &live_root_times);
    const auto live_child_cpu = process_cpu_time_usec(child.get(), &live_child_times);
    if (!check(live_root_cpu && live_child_cpu && *live_child_cpu >= k_cpu_accounting_work_usec,
            "independent process accounting confirms substantial child CPU") ||
        !check_cpu_accounting_sample(backend, *diagnostics, *live_root_cpu + *live_child_cpu, "live child", &live,
            &live_root_times, &live_child_times))
    {
        return false;
    }
    bool ok = check(live.cpu_time_usec >= initial.cpu_time_usec + k_cpu_accounting_work_usec,
        "the Job accumulates the detached child's work after the baseline sample");
    if (!check(write_gate_file(gates.filePath(QStringLiteral("child-exit"))), "CPU child may exit") ||
        !check(wait_for_process_exit(child.get()), "CPU child actually exits") ||
        !check(capture.wait_for_output(QByteArrayLiteral("cpu-child-exited")), "root observes child exit"))
    {
        return false;
    }
    cpu_accounting_process_times_t final_child_times{}, waiting_root_times{};
    const auto final_child_cpu  = process_cpu_time_usec(child.get(), &final_child_times);
    const auto waiting_root_cpu = process_cpu_time_usec(root.get(), &waiting_root_times);
    child.reset();
    if (!check(final_child_cpu && waiting_root_cpu, "exited child CPU remains independently readable") ||
        !check_cpu_accounting_sample(backend, *diagnostics, *final_child_cpu + *waiting_root_cpu,
            "child handles closed", &child_exited, &waiting_root_times, &final_child_times))
    {
        return false;
    }
    ok &= check(child_exited.cpu_time_usec >= live.cpu_time_usec,
        "closing every exited-child handle preserves cumulative Job CPU");
    if (!check(write_gate_file(gates.filePath(QStringLiteral("root-exit"))), "CPU root may exit") ||
        !check(capture.wait_for_exit(), "CPU root actually exits"))
    {
        return false;
    }
    const auto exit = capture.exit_snapshot();
    ok &= check(exit && exit->reason == term::Terminal_exit_reason::EXITED && exit->exit_code == 0,
        "CPU accounting fixture exits successfully");
    cpu_accounting_process_times_t final_root_times{};
    const auto final_root_cpu = process_cpu_time_usec(root.get(), &final_root_times);
    root.reset();
    if (!check(final_root_cpu.has_value(), "root final CPU is available") ||
        !check_cpu_accounting_sample(backend, *diagnostics, *final_root_cpu + *final_child_cpu,
            "root exited", &root_exited, &final_root_times, &final_child_times))
    {
        return false;
    }
    ok &= check(root_exited.cpu_time_usec >= child_exited.cpu_time_usec,
        "root exit does not hide the retained Job's final CPU total");
    return ok;
}

bool test_process_snapshot_observes_native_job(const QString& executable_path)
{
    QTemporaryDir directory;
    if (!check(directory.isValid(), "process snapshot gate directory is available")) {
        return false;
    }
    const QDir gates(directory.path());
    Backend_capture capture;
    term::Windows_conpty_backend backend;
    const term::Terminal_backend& observer = backend;
    if (!check(!observer.process_snapshot(), "process snapshot is unavailable before Job admission")) {
        return false;
    }
    const auto result = backend.start(launch_config(executable_path,
        {QStringLiteral("--cpu-accounting-root"), directory.path()}), capture.callbacks());
    if (!check(result.code == term::Terminal_backend_result_code::ACCEPTED,
            "process snapshot fixture starts in the native ConPTY Job"))
    {
        return false;
    }
    const auto root_pid  = capture.wait_for_pid_output(QByteArrayLiteral("cpu-root-pid="));
    const auto child_pid = capture.wait_for_pid_output(QByteArrayLiteral("cpu-child-pid="));
    if (!check(root_pid && child_pid, "process snapshot fixture reports the root and detached child")) {
        return false;
    }
    auto root  = open_process_handle(*root_pid);
    auto child = open_process_handle(*child_pid);
    if (!check(root.is_valid() && child.is_valid(), "process snapshot holds exact fixture identities")) {
        return false;
    }
    cpu_accounting_process_times_t root_times{}, child_times{};
    const auto root_cpu  = process_cpu_time_usec(root.get(), &root_times);
    const auto child_cpu = process_cpu_time_usec(child.get(), &child_times);
    if (!check(root_cpu && child_cpu, "independent process times are available for both members")) {
        return false;
    }
    const auto find_member = [](const vnm_terminal::Process_snapshot& snapshot, DWORD pid)
        -> const vnm_terminal::Process_snapshot_entry*
    {
        const auto found = std::find_if(snapshot.processes.begin(), snapshot.processes.end(),
            [pid](const auto& entry) { return entry.process_id == pid; });
        return found == snapshot.processes.end() ? nullptr : &*found;
    };
    const auto before_sample = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto initial = observer.process_snapshot();
    const auto after_sample = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!check(initial.has_value(), "the native backend supplies a process snapshot through its contract")) {
        return false;
    }
    const auto root_cpu_after  = process_cpu_time_usec(root.get());
    const auto child_cpu_after = process_cpu_time_usec(child.get());
    if (!check(root_cpu_after && child_cpu_after, "independent CPU readings bracket the snapshot")) {
        return false;
    }
    bool ok = check(initial->sampled_at_usec >= (std::uint64_t)before_sample &&
        initial->sampled_at_usec <= (std::uint64_t)after_sample &&
        initial->logical_processor_count == GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
        "process snapshot includes sample time and processor normalization");
    ok &= check(initial->processes.size() == 2U && !find_member(*initial, GetCurrentProcessId()),
        "process snapshot contains only the two admitted Job members");
    const auto check_member = [
            &initial, &find_member, &executable_path
        ](
            DWORD                                 pid,
            const cpu_accounting_process_times_t& times,
            std::uint64_t                         minimum_cpu,
            std::uint64_t                         maximum_cpu)
        {
            const auto* entry = find_member(*initial, pid);
            if (!check(entry != nullptr, "process snapshot includes the expected Job member")) {
                return false;
            }
            bool member_ok = check(entry->birth_token != 0U && entry->birth_token == times.creation_ticks,
                "process snapshot birth token identifies the independently observed process");
            member_ok &= check(QString::fromStdString(entry->name).compare(
                QFileInfo(executable_path).fileName(), Qt::CaseInsensitive) == 0,
                "process snapshot includes the executable name");
            member_ok &= check(entry->cpu_time_usec && *entry->cpu_time_usec >= minimum_cpu &&
                *entry->cpu_time_usec <= maximum_cpu,
                "process snapshot CPU lies within independent readings before and after sampling");
            member_ok &= check(entry->resident_memory_bytes && *entry->resident_memory_bytes > 0U,
                "process snapshot includes resident memory for an observable member");
            return member_ok;
        };
    ok &= check_member(*root_pid, root_times, *root_cpu, *root_cpu_after);
    ok &= check_member(*child_pid, child_times, *child_cpu, *child_cpu_after);
    if (!ok ||
        !check(write_gate_file(gates.filePath(QStringLiteral("start-burn"))), "snapshot child may do CPU work") ||
        !check(wait_for_file(gates.filePath(QStringLiteral("burn-finished"))), "snapshot child completes CPU work"))
    {
        return false;
    }
    const auto live = observer.process_snapshot();
    const auto* initial_child = find_member(*initial, *child_pid);
    const auto* live_child = live ? find_member(*live, *child_pid) : nullptr;
    if (!check(live_child && live_child->birth_token == initial_child->birth_token &&
            live_child->cpu_time_usec &&
            *live_child->cpu_time_usec >= *initial_child->cpu_time_usec + k_cpu_accounting_work_usec &&
            live->sampled_at_usec > initial->sampled_at_usec,
            "successive native snapshots retain identity and observe the detached child's CPU work"))
    {
        return false;
    }
    if (!check(write_gate_file(gates.filePath(QStringLiteral("child-exit"))), "snapshot child may exit") ||
        !check(wait_for_process_exit(child.get()), "snapshot child actually exits") ||
        !check(capture.wait_for_output(QByteArrayLiteral("cpu-child-exited")), "root observes snapshot child exit"))
    {
        return false;
    }
    const auto child_exited = observer.process_snapshot();
    ok &= check(child_exited && find_member(*child_exited, *root_pid) &&
        !find_member(*child_exited, *child_pid), "snapshot retires the exited child while retaining the live root");
    if (!check(write_gate_file(gates.filePath(QStringLiteral("root-exit"))), "snapshot root may exit") ||
        !check(capture.wait_for_exit(), "snapshot root actually exits"))
    {
        return false;
    }
    const auto exit = capture.exit_snapshot();
    ok &= check(exit && exit->reason == term::Terminal_exit_reason::EXITED && exit->exit_code == 0,
        "process snapshot fixture exits successfully");
    const auto root_exited = observer.process_snapshot();
    ok &= check(!root_exited || root_exited->processes.empty(), "snapshot does not retain exited process rows");
    return ok;
}

const std::vector<term::terminal_grid_size_t>& resize_storm_grid_sizes()
{
    static const std::vector<term::terminal_grid_size_t> sizes = {
        { 20, 70  },
        { 35, 132 },
        { 12, 40  },
        { 48, 160 },
        { 24, 80  },
        { 30, 100 },
        { 18, 90  },
        { 42, 120 },
        { 25, 81  },
        { 33, 111 },
        { 16, 132 },
        { 28, 96  },
        { 21, 73  },
        { 37, 140 },
        { 19, 88  },
        { 31, 99  },
    };

    return sizes;
}

int resize_storm_max_rows()
{
    int max_rows = 0;
    for (term::terminal_grid_size_t grid_size : resize_storm_grid_sizes()) {
        if (grid_size.rows > max_rows) {
            max_rows = grid_size.rows;
        }
    }

    return max_rows;
}

int gated_stream_resize_pad_rows()
{
    return resize_storm_max_rows() + 32;
}

QByteArray shell_fixture_command(std::string_view command)
{
    QByteArray bytes(
        command.data(),
        static_cast<qsizetype>(command.size()));
    bytes.append('\r');
    return bytes;
}

QByteArray shell_fixture_prompt()
{
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();
    QByteArray prompt(
        contract.prompt.data(),
        static_cast<qsizetype>(contract.prompt.size()));
    while (prompt.endsWith(' ')) {
        prompt.chop(1);
    }

    return prompt;
}

QByteArray shell_fixture_size_marker(term::terminal_grid_size_t grid_size)
{
    QByteArray marker = QByteArrayLiteral("size ");
    marker += QByteArray::number(grid_size.rows);
    marker += 'x';
    marker += QByteArray::number(grid_size.columns);
    return marker;
}

bool is_shell_line_delimiter(char ch)
{
    return ch == '\r' || ch == '\n';
}

bool output_contains_shell_size_marker(
    const QByteArray&          output,
    term::terminal_grid_size_t grid_size)
{
    const QByteArray marker = shell_fixture_size_marker(grid_size);
    qsizetype        offset = 0;
    for (;;) {
        offset = output.indexOf(marker, offset);
        if (offset < 0) {
            return false;
        }

        const qsizetype delimiter_offset = offset + marker.size();
        if (delimiter_offset < output.size() &&
            is_shell_line_delimiter(output[delimiter_offset]))
        {
            return true;
        }

        ++offset;
    }
}

std::size_t shell_size_report_count(const QByteArray& output)
{
    return count_occurrences(output, QByteArrayLiteral("size "));
}

QByteArray shell_fixture_stream_command(int count)
{
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();
    QByteArray bytes(
        contract.stream_command.data(),
        static_cast<qsizetype>(contract.stream_command.size()));
    bytes += ' ';
    bytes += QByteArray::number(count);
    bytes += '\r';
    return bytes;
}

QByteArray shell_fixture_gated_stream_command(
    int                        count,
    term::terminal_grid_size_t resize_gate,
    int                        resize_pad_rows)
{
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();
    QByteArray bytes(
        contract.gated_stream_command.data(),
        static_cast<qsizetype>(contract.gated_stream_command.size()));
    bytes += ' ';
    bytes += QByteArray::number(count);
    bytes += ' ';
    bytes += QByteArray::number(resize_gate.rows);
    bytes += ' ';
    bytes += QByteArray::number(resize_gate.columns);
    bytes += ' ';
    bytes += QByteArray::number(resize_pad_rows);
    bytes += '\r';
    return bytes;
}

QByteArray shell_fixture_gated_stream_ready_marker()
{
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();
    return
        QByteArray(
            contract.gated_stream_ready_output.data(),
            static_cast<qsizetype>(contract.gated_stream_ready_output.size()));
}

QByteArray shell_fixture_gated_stream_resized_marker(
    term::terminal_grid_size_t grid_size)
{
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();
    QByteArray marker(
        contract.gated_stream_resized_prefix.data(),
        static_cast<qsizetype>(contract.gated_stream_resized_prefix.size()));
    marker += QByteArray::number(grid_size.rows);
    marker += 'x';
    marker += QByteArray::number(grid_size.columns);
    return marker;
}

QByteArray shell_fixture_gated_stream_continue_command()
{
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();
    return shell_fixture_command(contract.gated_stream_continue_command);
}

QByteArray shell_fixture_stream_line(int row)
{
    QByteArray line = QByteArrayLiteral("stream-row-");
    if (row < 100) { line += '0'; }
    if (row < 10)  { line += '0'; }
    line += QByteArray::number(row);
    return line;
}

QByteArray conpty_utf8_payload()
{
    return decode_hex("636f6e7074792d7574663820636166c3a920cea920e4b8ad20e29c930d0a");
}

QByteArray conpty_sync_enter_sequence()
{
    return decode_hex("1b5b3f3230323668");
}

QByteArray conpty_sync_hidden_payload()
{
    return QByteArrayLiteral("sync-hidden-before-resize\r\n");
}

QByteArray conpty_sync_release_sequence()
{
    return decode_hex("1b5b3f323032366c");
}

QByteArray conpty_sync_final_payload()
{
    return QByteArrayLiteral("sync-final-after-resize\r\n");
}

QByteArray conpty_quick_exit_payload()
{
    return QByteArrayLiteral("quick-exit\r\n");
}

term::Terminal_launch_config powershell_script_launch_config(const QString& script_path)
{
    term::Terminal_launch_config config;
    config.argv = {
        QStringLiteral("powershell.exe"),
        QStringLiteral("-NoLogo"),
        QStringLiteral("-NoProfile"),
        QStringLiteral("-ExecutionPolicy"),
        QStringLiteral("Bypass"),
        QStringLiteral("-File"),
        script_path,
    };
    config.working_directory  = QFileInfo(script_path).absolutePath();
    config.initial_grid_size  = term::terminal_grid_size_t{24, 80};
    config.identity.term      = QStringLiteral("xterm-256color");
    config.identity.colorterm = QStringLiteral("truecolor");
    return config;
}

QString powershell_single_quoted_string(QString text)
{
    text.replace(QStringLiteral("'"), QStringLiteral("''"));
    return QStringLiteral("'") + text + QStringLiteral("'");
}

bool write_tight_paused_output_powershell_script(
    const QString& script_path,
    const QString& ready_path,
    const QString& done_path,
    const QString& exit_gate_path)
{
    const QByteArray script = QStringLiteral(
        "$ErrorActionPreference = 'Stop'\r\n"
        "[System.IO.File]::WriteAllText(%1, 'ready')\r\n"
        "$key = [Console]::ReadKey($true)\r\n"
        "if ($key.KeyChar -ne [char]'g') {\r\n"
        "    [Console]::Out.WriteLine('tight-paused-unexpected-input')\r\n"
        "    exit 3\r\n"
        "}\r\n"
        "[Console]::Out.Write('~~~~~~~~~~~~~~')\r\n"
        "[Console]::Out.Flush()\r\n"
        "[System.IO.File]::WriteAllText(%2, 'done')\r\n"
        "while (-not [System.IO.File]::Exists(%3)) {\r\n"
        "    Start-Sleep -Milliseconds 10\r\n"
        "}\r\n"
        "exit 0\r\n").arg(
            powershell_single_quoted_string(ready_path),
            powershell_single_quoted_string(done_path),
            powershell_single_quoted_string(exit_gate_path)).toUtf8();

    QFile file(script_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::cerr << "failed to open tight paused-output script: "
            << file.errorString().toLocal8Bit().constData() << '\n';
        return false;
    }

    if (file.write(script) != script.size()) {
        std::cerr << "failed to write tight paused-output script: "
            << file.errorString().toLocal8Bit().constData() << '\n';
        return false;
    }

    return true;
}

bool test_progressing_output_exit_wait_has_absolute_bound()
{
    // The bound under test is the absolute one, so the window has to be wide
    // enough that a 5 ms producer cannot be starved out of it. An earlier 75 ms
    // window failed intermittently on loaded ASan CI runners, once with zero
    // output events, because the producer thread had not been scheduled at all
    // before the window closed.
    constexpr auto k_producer_interval = std::chrono::milliseconds(5);
    constexpr auto k_absolute_bound    = std::chrono::milliseconds(500);

    Backend_capture capture;
    term::Terminal_backend_callbacks callbacks = capture.callbacks();
    std::atomic_bool keep_producing = true;
    std::thread producer([&] {
        while (keep_producing.load()) {
            callbacks.output_received(QByteArrayLiteral("progress"));
            std::this_thread::sleep_for(k_producer_interval);
        }
    });

    // Take thread start-up out of the measured window: only once output is
    // genuinely flowing does the bounded wait mean what the test claims.
    const bool producing = capture.wait_for_output_event_count(1U, k_wait_timeout);
    const std::size_t events_before_wait = capture.output_event_count_snapshot();

    const auto wait_started = std::chrono::steady_clock::now();
    const bool exited = capture.wait_for_exit_while_output_progresses(k_absolute_bound);
    const auto wait_elapsed = std::chrono::steady_clock::now() - wait_started;
    const std::size_t events_after_wait = capture.output_event_count_snapshot();
    keep_producing.store(false);
    producer.join();

    bool ok  = check(producing,
        "progressing output fixture produces output before the bounded wait");
    ok      &= check(!exited,
        "progressing output without exit reaches its absolute wait bound");
    ok      &= check(events_after_wait > events_before_wait,
        "absolute exit wait bound is exercised while output keeps progressing");
    ok      &= check(
        wait_elapsed >= k_absolute_bound / 2 &&
        wait_elapsed <  k_absolute_bound + std::chrono::seconds(2),
        "absolute exit wait bound returns near its configured deadline");
    return ok;
}

bool write_ignored_interrupt_powershell_script(const QString& script_path)
{
    const QByteArray script = QByteArrayLiteral(
        "$ErrorActionPreference = 'Stop'\r\n"
        "[Console]::TreatControlCAsInput = $true\r\n"
        "[Console]::Out.WriteLine('ignore-int')\r\n"
        "$deadline = [DateTime]::UtcNow.AddSeconds(5)\r\n"
        "$caught = $false\r\n"
        "while ([DateTime]::UtcNow -lt $deadline) {\r\n"
        "    if ([Console]::KeyAvailable) {\r\n"
        "        $key = [Console]::ReadKey($true)\r\n"
        "        if ([int][char]$key.KeyChar -eq 3) {\r\n"
        "            [Console]::Out.WriteLine('caught-int')\r\n"
        "            $caught = $true\r\n"
        "            break\r\n"
        "        }\r\n"
        "    }\r\n"
        "    Start-Sleep -Milliseconds 10\r\n"
        "}\r\n"
        "if (-not $caught) {\r\n"
        "    [Console]::Out.WriteLine('missed-int')\r\n"
        "    exit 3\r\n"
        "}\r\n"
        "[Console]::Out.WriteLine('post-int-ready')\r\n"
        "$key = [Console]::ReadKey($true)\r\n"
        "if ($key.KeyChar -ne [char]'q') {\r\n"
        "    [Console]::Out.WriteLine('unexpected-post-int-input')\r\n"
        "    exit 4\r\n"
        "}\r\n"
        "[Console]::Out.WriteLine('natural-exit')\r\n"
        "exit 130\r\n");

    QFile file(script_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::cerr << "failed to open ignored-interrupt script: "
            << file.errorString().toLocal8Bit().constData() << '\n';
        return false;
    }

    if (file.write(script) != script.size()) {
        std::cerr << "failed to write ignored-interrupt script: "
            << file.errorString().toLocal8Bit().constData() << '\n';
        return false;
    }

    return true;
}

bool shell_fixture_stream_rows_are_exactly_ordered(
    const QByteArray&  output,
    int                stream_count,
    const char*        message)
{
    const std::size_t observed_row_count =
        count_occurrences(output, QByteArrayLiteral("stream-row-"));
    bool rows_are_exact = true;
    if (observed_row_count != static_cast<std::size_t>(stream_count)) {
        std::cerr << "expected " << stream_count
            << " stream rows, saw " << observed_row_count << '\n';
        rows_are_exact = false;
    }

    qsizetype search_offset = 0;
    for (int row = 1; row <= stream_count; ++row) {
        const QByteArray  line      = shell_fixture_stream_line(row);
        const std::size_t row_count = count_occurrences(output, line);
        if (row_count != 1U) {
            std::cerr << "expected one occurrence of "
                << line.constData()
                << ", saw " << row_count << '\n';
            rows_are_exact = false;
        }

        const qsizetype row_offset = output.indexOf(line, search_offset);
        if (row_offset < 0) {
            std::cerr << "stream row out of order: " << line.constData() << '\n';
            rows_are_exact = false;
            continue;
        }

        search_offset = row_offset + line.size();
    }

    return rows_are_exact ? true : check(false, message);
}

std::size_t shell_fixture_authored_stream_row_count(const QByteArray& output)
{
    return count_occurrences(output, QByteArrayLiteral("stream-row-"));
}

bool apply_resize_storm(
    term::Terminal_backend&    backend,
    std::uint64_t&             resize_id,
    const char*                message)
{
    for (term::terminal_grid_size_t grid_size : resize_storm_grid_sizes()) {
        const term::Terminal_backend_result resize_result = backend.resize({
            resize_id++,
            grid_size,
        });
        if (!check(resize_result.code == term::Terminal_backend_result_code::ACCEPTED,
            message))
        {
            std::cerr << "resize storm rejected "
                << grid_size.rows << "x" << grid_size.columns << '\n';
            return false;
        }
    }

    return true;
}

bool stop_shell_like_fixture(
    term::Terminal_backend&    backend,
    Backend_capture&           capture)
{
    bool ok = true;

    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();
    const term::Terminal_backend_result exit_write_result =
        backend.write(shell_fixture_command(contract.exit_command));
    ok &= check(exit_write_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "shell-like fixture accepts exit command");
    ok &= check(capture.wait_for_exit(), "shell-like fixture exits after exit command");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::EXITED &&
        exit->exit_code == 0,
        "shell-like fixture reports clean exit");
    ok &= check_no_backend_errors(capture,
        "shell-like fixture produces no backend errors");

    return ok;
}

bool wait_for_shell_size_report(
    term::Terminal_backend&    backend,
    Backend_capture&           capture,
    term::terminal_grid_size_t grid_size,
    const char*                message)
{
    const QByteArray marker = shell_fixture_size_marker(grid_size);
    if (output_contains_shell_size_marker(capture.output_snapshot(), grid_size)) {
        return true;
    }

    std::size_t observed_size_reports =
        shell_size_report_count(capture.output_snapshot());
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();
    const auto deadline = std::chrono::steady_clock::now() + k_wait_timeout;
    do {
        const term::Terminal_backend_result write_result =
            backend.write(shell_fixture_command(contract.size_command));
        if (!check(write_result.code == term::Terminal_backend_result_code::ACCEPTED,
            "shell-like fixture accepts size command"))
        {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;
        }

        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        bool expected_size_reported = false;
        const bool observed_size_response = capture.wait_for_output_matching_within(
            [&](const QByteArray& output) {
                expected_size_reported =
                    output_contains_shell_size_marker(output, grid_size);
                return
                    expected_size_reported ||
                    shell_size_report_count(output) > observed_size_reports;
            },
            remaining);
        if (expected_size_reported) {
            return true;
        }

        const QByteArray output = capture.output_snapshot();
        if (output_contains_shell_size_marker(output, grid_size)) {
            return true;
        }

        observed_size_reports = shell_size_report_count(output);
        if (!observed_size_response) {
            break;
        }
    }
    while (std::chrono::steady_clock::now() < deadline);

    std::cerr << "expected shell size marker: " << marker.constData() << '\n';
    std::cerr << "shell size output hex: "
        << capture.output_snapshot().toHex(' ').constData()
        << '\n';
    return check(false, message);
}

// After a resize, ConPTY 1.25 asks its host for the cursor the first time the
// child reads its screen buffer, and takes the next cursor report it reads as
// the answer, however late. The interactive scenario keeps output paused across
// its resize, so the request is answered once the fixture's reply-handling
// queries arrive: by then the fixture has moved its cursor and asks for it, so
// the scripted report is the true answer to both. Earlier releases ask nothing.
bool answer_conpty_cursor_request(
    term::Terminal_backend& backend,
    Backend_capture&        capture,
    const QByteArray&       scripted_reply)
{
    const QByteArray queries = fixture_record_payload(
        term::Terminal_canvas_fixture_record_kind::OUTPUT,
        "reply-handling");
    bool ok = check(capture.wait_for_output(queries.mid(queries.lastIndexOf('\x1b'))),
        "interactive fixture sends its reply-handling queries");

    const QByteArray cursor_request = QByteArrayLiteral("\x1b[6n");
    if (count_occurrences(capture.output_snapshot(), cursor_request) >
        count_occurrences(queries, cursor_request))
    {
        ok &= check(backend.write(scripted_csi_report(scripted_reply, 'R')).code ==
                term::Terminal_backend_result_code::ACCEPTED,
            "ConPTY backend accepts the host's answer to ConPTY's cursor request");
    }

    return ok;
}

bool test_interactive_canvas_fixture(const QString& fixture_path)
{
    bool ok = true;

    QTemporaryDir checkpoint_dir;
    ok &= check(checkpoint_dir.isValid(), "temporary checkpoint directory is available");
    if (!checkpoint_dir.isValid()) {
        return false;
    }

    const QString checkpoint_path =
        checkpoint_dir.filePath(QStringLiteral("enable-input-modes.checkpoint"));

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    term::Terminal_launch_config config = launch_config(
        fixture_path,
        {
            QStringLiteral("--interactive-scenario"),
            QString::fromLatin1(
                term::terminal_canvas_fixture_scenario_name().data(),
                static_cast<qsizetype>(
                    term::terminal_canvas_fixture_scenario_name().size())),
            QStringLiteral("--expect-initial-size"),
            QStringLiteral("24"),
            QStringLiteral("80"),
            QStringLiteral("--checkpoint-after-enable-input-modes"),
            checkpoint_path,
            QStringLiteral("--wait-for-host-ready"),
        });

    const term::Terminal_backend_result start_result =
        backend->start(config, capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend starts interactive fixture");

    // This test is the fixture's terminal, and a terminal answers ConPTY's own
    // queries as well as the child's, which ConPTY passes through. ConPTY asks
    // for device attributes as it starts and takes the first DA1 report it
    // reads as its answer; left unanswered, 1.25 takes the child's scripted
    // report instead. 1.24 passes a DA1 report to a child that already reads VT
    // input, and lets the child start after at most 3 s without an answer, so
    // the fixture enables VT input only after the ready character that follows
    // the answer. By then ConPTY has taken the answer on either version.
    ok &= check(capture.wait_for_output(QByteArrayLiteral("\x1b[c")),
        "ConPTY asks its host for device attributes");
    const QByteArray scripted_reply = fixture_record_payload(
        term::Terminal_canvas_fixture_record_kind::EXPECT_INPUT,
        "reply-handling");
    QByteArray host_answer = scripted_csi_report(scripted_reply, 'c');
    ok &= check(!host_answer.isEmpty(), "the scripted reply carries a DA1 report");
    host_answer += term::k_terminal_canvas_fixture_host_ready_input;
    ok &= check(backend->write(host_answer).code ==
            term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend accepts the host's device attributes");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("term>")),
        "interactive fixture prompt reaches backend output");

    const term::Terminal_backend_result pause_result = backend->set_output_paused(true);
    ok &= check(pause_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend accepts output pause");

    std::uint64_t resize_id             = 1U;
    bool          output_pause_released = false;
    for (const term::terminal_canvas_fixture_record_t& record :
        term::terminal_canvas_fixture_contract_script())
    {
        if (record.kind == term::Terminal_canvas_fixture_record_kind::EXPECT_INPUT) {
            const QByteArray input = decode_hex(record.payload_hex);
            if (record.label == std::string_view("reply-handling")) {
                ok &= answer_conpty_cursor_request(*backend, capture, input);
            }
            const term::Terminal_backend_result write_result = backend->write(input);
            ok &= check(write_result.code == term::Terminal_backend_result_code::ACCEPTED,
                "ConPTY backend accepts scripted input write");
            if (write_result.code != term::Terminal_backend_result_code::ACCEPTED) {
                return false;
            }
        }
        else
        if (record.kind == term::Terminal_canvas_fixture_record_kind::RESIZE) {
            const term::Terminal_backend_result resize_result = backend->resize({
                resize_id++,
                term::terminal_grid_size_t{record.rows, record.columns},
            });
            ok &= check(resize_result.code == term::Terminal_backend_result_code::ACCEPTED,
                "ConPTY backend accepts scripted resize");
            if (resize_result.code != term::Terminal_backend_result_code::ACCEPTED) {
                return false;
            }

            ok &= check(wait_for_file(checkpoint_path),
                "fixture produced post-resize output while backend output was paused");
            const QByteArray enable_modes_payload = fixture_record_payload(
                term::Terminal_canvas_fixture_record_kind::OUTPUT,
                term::k_terminal_canvas_fixture_enable_input_modes_label);
            ok &= check(!enable_modes_payload.isEmpty(),
                "enable-input-modes payload is available");
            ok &= check(!capture.output_snapshot().contains(enable_modes_payload),
                "paused ConPTY output holds post-resize fixture output");

            const term::Terminal_backend_result resume_result =
                backend->set_output_paused(false);
            ok                    &= check(resume_result.code == term::Terminal_backend_result_code::ACCEPTED,
                "ConPTY backend accepts output resume");
            ok                    &= check(capture.wait_for_output(decode_hex("1b5b3f3230303468")),
                "resumed ConPTY output delivers buffered fixture output");
            output_pause_released  = true;
        }
    }

    ok &= check(output_pause_released, "interactive scenario exercised output pause");
    ok &= check(capture.wait_for_exit_while_output_progresses(),
        "interactive fixture exits without stalling its output stream");
    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::EXITED &&
        exit->exit_code == 0,
        "interactive fixture reports clean exit");

    const QByteArray output = capture.output_snapshot();
    ok &= check(output.contains(decode_hex("1b5b3f3230303468")),
        "ConPTY output includes bracketed paste enable");
    ok &= check(output.contains(decode_hex("1b5b3f323030342470")),
        "ConPTY output includes bracketed paste query");
    ok &= check(count_occurrences(output, QByteArrayLiteral("stream-row")) == 4096U,
        "ConPTY output preserves high-volume stream rows");
    ok &= check(scripted_output_is_ordered(output),
        "ConPTY output contains the scripted fixture output in order");
    ok &= check_no_backend_errors(capture,
        "interactive fixture produces no backend errors");

    return ok;
}

// The packaged ConPTY forwards CSI 16 t to the host instead of answering it, and
// the host's reply reaches the child's VT input. The reply carries the fixed
// 10x20 cell OpenConsole places sixel images on, whatever the display's cell is,
// so a ConPTY update that starts answering or reshapes the reply fails here.
bool test_cell_pixel_size_query_round_trips_through_conpty(const QString& fixture_path)
{
    bool ok = true;

    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();
    term::Terminal_session_config config;
    config.trace_output_chunk_limit = 4096U;
    config.backend_event_notifier   = [] {};
    term::Terminal_session session(term::make_windows_conpty_backend(), config);
    session.set_cell_pixel_size({9, 18});
    ok &= check(session.start(
            launch_config(fixture_path, {QStringLiteral("--shell-like-smoke")})).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "ConPTY session starts the shell-like fixture");

    const auto wait_for_output = [&](const QByteArray& needle) {
        const auto deadline = std::chrono::steady_clock::now() + k_wait_timeout;
        do {
            session.process_backend_callback_events();
            QByteArray received;
            for (const QByteArray& chunk : session.output_chunks()) {
                received += chunk;
            }
            if (received.contains(needle)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        while (std::chrono::steady_clock::now() < deadline);
        return false;
    };

    ok &= check(wait_for_output(shell_fixture_prompt()),
        "ConPTY session shows the shell-like fixture prompt");
    ok &= check(session.write_user_bytes(
            shell_fixture_command(contract.cell_size_query_command)).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "ConPTY session sends the cell size query command");
    QByteArray expected_report(
        contract.cell_size_reply_prefix.data(),
        static_cast<qsizetype>(contract.cell_size_reply_prefix.size()));
    expected_report += QByteArrayLiteral("\x1b[6;20;10t").toHex();
    ok &= check(wait_for_output(expected_report),
        "the child reads the fixed 10x20 cell back through ConPTY");

    ok &= check(session.write_user_bytes(shell_fixture_command(contract.exit_command)).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "ConPTY session sends exit to the shell-like fixture");
    const auto deadline = std::chrono::steady_clock::now() + k_wait_timeout;
    while (!session.exit_status().has_value() && std::chrono::steady_clock::now() < deadline) {
        session.process_backend_callback_events();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ok &= check(session.exit_status().has_value() && session.exit_status()->exit_code == 0,
        "shell-like fixture exits cleanly after the cell size query");

    return ok;
}

// Cursor movements that ConPTY itself wrote after the image started; with none
// the model had to reach the child's cursor on its own.
int cursor_moves_after_image(const QByteArray& output)
{
    const qsizetype image_start = output.lastIndexOf(QByteArrayLiteral("\x1bP"));
    if (image_start < 0) {
        return 0;
    }

    int moves = 0;
    for (qsizetype index = output.indexOf("\x1b[", image_start);
         index >= 0;
         index = output.indexOf("\x1b[", index + 2))
    {
        qsizetype final_index = index + 2;
        while (final_index < output.size() &&
               ((output[final_index] >= '0' && output[final_index] <= '9') ||
                output[final_index] == ';' || output[final_index] == '?'))
        {
            ++final_index;
        }
        if (final_index < output.size() &&
            std::string_view("HfABCDdG").find(output[final_index]) != std::string_view::npos)
        {
            ++moves;
        }
    }
    return moves;
}

// The sixel cursor-sync gate. OpenConsole moves its own cursor past a sixel
// image, while ConPTY forwards the image unanswered, so the model must reach
// the same cell by itself or later cursor-relative output lands elsewhere.
// Each case runs in a fresh session: the child writes it through the packaged
// ConPTY followed by a cell size query, reports its console cursor once the
// reply is back and waits, and the model cursor is sampled then. Observation
// cases are only recorded.
bool test_sixel_cursor_stays_in_sync_with_openconsole(const QString& fixture_path)
{
    bool ok = true;
    QTemporaryDir report_dir;
    if (!check(report_dir.isValid(), "sixel cursor-sync report directory is created")) {
        return false;
    }

    int case_index = 0;
    for (const term::Terminal_canvas_fixture_sixel_cursor_case& sixel_case :
        term::terminal_canvas_fixture_sixel_cursor_cases())
    {
        const QString name = QString::fromStdString(sixel_case.name);
        const QString report_path =
            report_dir.filePath(QStringLiteral("sixel-cursor-%1.txt").arg(case_index++));
        const std::string label = "sixel cursor sync, " + sixel_case.name;

        term::Terminal_session_config config;
        config.trace_output_chunk_limit = 4096U;
        config.backend_event_notifier   = [] {};
        if (sixel_case.retained_history_capacity_bytes != 0U) {
            config.retained_history_capacity_bytes = sixel_case.retained_history_capacity_bytes;
        }
        term::Terminal_session session(term::make_windows_conpty_backend(), config);
        if (!check(session.start(launch_config(
                    fixture_path,
                    {QStringLiteral("--sixel-cursor-sync"), name, report_path})).code ==
                term::Terminal_session_result_code::ACCEPTED,
                label + ": session starts"))
        {
            ok = false;
            continue;
        }

        const auto output = [&] {
            QByteArray bytes;
            for (const QByteArray& chunk : session.output_chunks()) {
                bytes += chunk;
            }
            return bytes;
        };

        // The child reports only after the model has answered the query that
        // follows the image, so by then the model has applied the whole case.
        const auto deadline = std::chrono::steady_clock::now() + k_wait_timeout;
        bool       reported = false;
        while (!reported && std::chrono::steady_clock::now() < deadline) {
            session.process_backend_callback_events();
            reported = QFileInfo::exists(report_path);
            if (!reported) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        ok &= check(reported, label + ": the child reports after the model answers its query");

        int console_row    = -1;
        int console_column = -1;
        QFile report(report_path);
        if (report.open(QIODevice::ReadOnly)) {
            const QList<QByteArray> fields = report.readAll().trimmed().split(' ');
            if (fields.size() == 2) {
                console_row    = fields[0].toInt();
                console_column = fields[1].toInt();
            }
        }
        const std::optional<term::Terminal_render_snapshot> snapshot =
            session.latest_render_snapshot();
        const int model_row    = snapshot.has_value() ? snapshot->cursor.position.row    : -1;
        const int model_column = snapshot.has_value() ? snapshot->cursor.position.column : -1;
        const bool match =
            console_row >= 0 && model_row == console_row && model_column == console_column;

        const QByteArray received = output();
        std::cout << "sixel cursor sync | " << sixel_case.name
            << " | console " << console_row << ',' << console_column
            << " | model " << model_row << ',' << model_column
            << " | " << (match ? "match" : "MISMATCH")
            << " | image forwarded: " << (received.contains("\x1bP") ? "yes" : "no")
            << " | ConPTY cursor moves after the image: " << cursor_moves_after_image(received)
            << '\n';
        ok &= check(match, label + ": the model cursor matches OpenConsole's");

        ok &= check(session.write_user_bytes("q").code ==
                term::Terminal_session_result_code::ACCEPTED,
            label + ": the child is released");
        const auto exit_deadline = std::chrono::steady_clock::now() + k_wait_timeout;
        while (!session.exit_status().has_value() &&
               std::chrono::steady_clock::now() < exit_deadline)
        {
            session.process_backend_callback_events();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ok &= check(session.exit_status().has_value() && session.exit_status()->exit_code == 0,
            label + ": the child exits cleanly");
    }

    return ok;
}

int run_paste_input_reader(const QString& output_path)
{
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (!SetConsoleMode(input, ENABLE_VIRTUAL_TERMINAL_INPUT)) {
        return 1;
    }

    std::cout << "paste-reader-ready\n" << std::flush;
    QString received;
    for (;;) {
        INPUT_RECORD records[128];
        DWORD count = 0;
        if (!ReadConsoleInputW(input, records, 128, &count)) {
            return 1;
        }
        for (DWORD index = 0; index < count; ++index) {
            const INPUT_RECORD& record = records[index];
            if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) {
                continue;
            }
            const KEY_EVENT_RECORD& key = record.Event.KeyEvent;
            if (key.uChar.UnicodeChar == L'!') {
                QFile output(output_path);
                const QByteArray bytes = received.toUtf8();
                return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() ? 0 : 1;
            }
            if (key.uChar.UnicodeChar != 0) {
                received.append(QString(key.wRepeatCount, QChar(key.uChar.UnicodeChar)));
            }
        }
    }
}

bool flush_escape_reader_and_ack(
    QFile& observation, std::string_view reader_name, std::size_t& ack_count)
{
    if (!observation.flush()) {
        return false;
    }

    ++ack_count;
    std::cout << reader_name << "-ack[" << ack_count << "]\n" << std::flush;
    return std::cout.good();
}

int run_escape_input_reader(
    const QString& observation_path,
    bool           hold_after_completion_ack = false)
{
    HANDLE input      = GetStdHandle(STD_INPUT_HANDLE);
    DWORD  input_mode = 0U;
    if (input == INVALID_HANDLE_VALUE ||
        !GetConsoleMode(input, &input_mode))
    {
        return 1;
    }

    input_mode &= ~ENABLE_VIRTUAL_TERMINAL_INPUT;
    input_mode &= ~ENABLE_LINE_INPUT;
    input_mode &= ~ENABLE_ECHO_INPUT;
    input_mode &= ~ENABLE_PROCESSED_INPUT;
    if (!SetConsoleMode(input, input_mode)) {
        return 1;
    }

    QFile observation(observation_path);
    if (!observation.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return 1;
    }
    std::size_t ack_count = 0U;
    bool pending_ack_key_up = false;
    bool pending_ack_completion = false;
    DWORD pending_ack_control_state = 0U;
    const WORD fallback_f12_scan = static_cast<WORD>(
        MapVirtualKeyW(VK_F12, MAPVK_VK_TO_VSC) & 0xffU);
    std::cout << "escape-input-reader-ready\n" << std::flush;
    for (;;) {
        INPUT_RECORD records[64];
        DWORD        count = 0U;
        if (!ReadConsoleInputW(input, records, 64U, &count)) {
            return 1;
        }

        for (DWORD index = 0U; index < count; ++index) {
            const INPUT_RECORD& record = records[index];
            if (record.EventType != KEY_EVENT) {
                continue;
            }

            const KEY_EVENT_RECORD& key = record.Event.KeyEvent;
            const bool ack_key =
                key.wVirtualKeyCode == VK_F12 &&
                key.wVirtualScanCode == fallback_f12_scan &&
                key.wRepeatCount == 1U &&
                key.uChar.UnicodeChar == 0 &&
                (key.dwControlKeyState == SHIFT_PRESSED ||
                    key.dwControlKeyState == (SHIFT_PRESSED | LEFT_CTRL_PRESSED));
            if (ack_key && key.bKeyDown)
            {
                if (pending_ack_key_up) {
                    return 1;
                }
                pending_ack_key_up = true;
                pending_ack_completion =
                    key.dwControlKeyState == (SHIFT_PRESSED | LEFT_CTRL_PRESSED);
                pending_ack_control_state = key.dwControlKeyState;
                continue;
            }
            if (pending_ack_key_up)
            {
                if (!ack_key || key.bKeyDown ||
                    key.dwControlKeyState != pending_ack_control_state)
                {
                    return 1;
                }
                const bool completion = pending_ack_completion;
                pending_ack_key_up = false;
                pending_ack_completion = false;
                if (!flush_escape_reader_and_ack(
                        observation, "escape-input-reader", ack_count))
                {
                    return 1;
                }
                if (completion) {
                    if (hold_after_completion_ack) {
                        std::cout << "escape-input-reader-holding-after-ack\n" << std::flush;
                        for (;;) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        }
                    }
                    return 0;
                }
                continue;
            }

            std::ostringstream line;
            line << (key.bKeyDown ? 'D' : 'U')
                << " vk=" << key.wVirtualKeyCode
                << " scan=" << key.wVirtualScanCode
                << " repeat=" << key.wRepeatCount
                << " unicode=" << static_cast<unsigned int>(key.uChar.UnicodeChar)
                << " state=" << key.dwControlKeyState;
            const QByteArray bytes = QByteArray::fromStdString(line.str()) + '\n';
            if (observation.write(bytes) != bytes.size() || !observation.flush()) {
                return 1;
            }
        }
    }
}

// A VT-input reader. By default it reads console bytes, converted to UTF-8 by
// the console. With utf16_reads it reads UTF-16 with ReadConsoleW and converts
// the text itself, so it observes the units ConPTY produced for the reader
// rather than the console's conversion of them.
int run_escape_vt_input_reader(const QString& observation_path, bool utf16_reads = false)
{
    HANDLE input      = GetStdHandle(STD_INPUT_HANDLE);
    DWORD  input_mode = 0U;
    if (input == INVALID_HANDLE_VALUE ||
        !GetConsoleMode(input, &input_mode) ||
        !SetConsoleCP(CP_UTF8))
    {
        return 1;
    }

    input_mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    input_mode &= ~ENABLE_LINE_INPUT;
    input_mode &= ~ENABLE_ECHO_INPUT;
    input_mode &= ~ENABLE_PROCESSED_INPUT;
    if (!SetConsoleMode(input, input_mode)) {
        return 1;
    }

    QFile observation(observation_path);
    if (!observation.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return 1;
    }
    const QByteArray barrier_sentinel = decode_hex("1b5b32343b327e");
    const QByteArray completion_sentinel = decode_hex("1b5b32343b367e");
    const qsizetype sentinel_size = barrier_sentinel.size();
    QByteArray pending;
    // A leading surrogate whose trailing unit has not been read yet.
    QString pending_utf16;
    std::size_t ack_count = 0U;
    std::cout << "escape-vt-input-reader-ready\n" << std::flush;
    for (;;) {
        if (utf16_reads) {
            wchar_t units[64];
            DWORD   count = 0U;
            if (!ReadConsoleW(input, units, static_cast<DWORD>(std::size(units)), &count, nullptr)) {
                return 1;
            }

            pending_utf16 += QString::fromWCharArray(units, static_cast<qsizetype>(count));
            qsizetype complete = pending_utf16.size();
            if (complete > 0 && pending_utf16.at(complete - 1).isHighSurrogate()) {
                --complete;
            }
            pending += pending_utf16.left(complete).toUtf8();
            pending_utf16.remove(0, complete);
        }
        else {
            char  bytes[64];
            DWORD count = 0U;
            if (!ReadFile(input, bytes, sizeof(bytes), &count, nullptr)) {
                return 1;
            }

            pending.append(bytes, static_cast<qsizetype>(count));
        }
        const qsizetype barrier_offset = pending.indexOf(barrier_sentinel);
        const qsizetype completion_offset = pending.indexOf(completion_sentinel);
        const bool completion = completion_offset >= 0 &&
            (barrier_offset < 0 || completion_offset < barrier_offset);
        const qsizetype sentinel_offset = completion
            ? completion_offset
            : barrier_offset;
        if (sentinel_offset >= 0) {
            const QByteArray& sentinel = completion
                ? completion_sentinel
                : barrier_sentinel;
            if (sentinel_offset + sentinel.size() != pending.size()) {
                return 1;
            }
            const QByteArray observed = pending.left(sentinel_offset);
            if ((!observed.isEmpty() && observation.write(observed) != observed.size()) ||
                !flush_escape_reader_and_ack(
                    observation, "escape-vt-input-reader", ack_count))
            {
                return 1;
            }
            pending.clear();
            if (completion) {
                return 0;
            }
            continue;
        }

        const qsizetype safe_byte_count = pending.size() - sentinel_size + 1;
        if (safe_byte_count > 0) {
            if (observation.write(pending.constData(), safe_byte_count) != safe_byte_count) {
                return 1;
            }
            pending.remove(0, safe_byte_count);
        }
        if (observation.error() != QFileDevice::NoError) {
            return 1;
        }
    }
}

QByteArray encoded_key_event(
    int key,
    Qt::KeyboardModifiers modifiers,
    const QString& text = {},
    term::Terminal_input_mode_state modes = {})
{
    QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
    return term::encode_terminal_key_event(event, modes);
}

QByteArray encoded_native_key_event(
    int key,
    Qt::KeyboardModifiers modifiers,
    quint32 native_scan_code,
    quint32 native_virtual_key,
    quint32 native_modifiers,
    const QString& text,
    bool auto_repeat = false,
    quint16 count = 1,
    term::Terminal_input_mode_state modes = {})
{
    const QKeyEvent event(
        QEvent::KeyPress,
        key,
        modifiers,
        native_scan_code,
        native_virtual_key,
        native_modifiers,
        text,
        auto_repeat,
        count);
    return term::encode_terminal_key_event(event, modes);
}

QByteArray native_key_record(
    char action, int virtual_key, int scan_code, int repeat, int unicode, int state)
{
    return QByteArray(1, action) + " vk=" + QByteArray::number(virtual_key) +
        " scan=" + QByteArray::number(scan_code) +
        " repeat=" + QByteArray::number(repeat) +
        " unicode=" + QByteArray::number(unicode) +
        " state=" + QByteArray::number(state) + '\n';
}

QByteArray native_key_frame(
    int virtual_key, int scan_code, int unicode, int key_down, int state)
{
    return QByteArrayLiteral("\x1b[") + QByteArray::number(virtual_key) + ';' +
        QByteArray::number(scan_code) + ';' + QByteArray::number(unicode) + ';' +
        QByteArray::number(key_down) + ';' + QByteArray::number(state) + ";1_";
}

// A VK_PACKET stroke carries its UTF-16 unit on key-up as well, as ConPTY's
// own records for typed text do; other keys release with none.
int packet_key_up_unicode(int virtual_key, int unicode)
{
    return virtual_key == VK_PACKET ? unicode : 0;
}

QByteArray native_key_stroke_bytes(
    int virtual_key, int scan_code, int unicode, int state)
{
    return native_key_frame(virtual_key, scan_code, unicode, 1, state) +
        native_key_frame(
            virtual_key, scan_code, packet_key_up_unicode(virtual_key, unicode), 0, state);
}

QByteArray native_key_stroke_records(
    int                virtual_key,
    int                scan_code,
    int                unicode,
    int                state,
    std::optional<int> key_up_unicode = std::nullopt)
{
    return native_key_record('D', virtual_key, scan_code, 1, unicode, state) +
        native_key_record('U', virtual_key, scan_code, 1,
            key_up_unicode.value_or(packet_key_up_unicode(virtual_key, unicode)), state);
}

QByteArray packet_key_stroke_records(const QByteArray& text)
{
    QByteArray records;
    for (const char character : text) {
        records += native_key_stroke_records(
            VK_PACKET, 0, static_cast<unsigned char>(character), 0);
    }
    return records;
}

bool reconstruct_native_record_text(
    const QByteArray& journal, QString& text, int& down_record_count)
{
    text.clear();
    down_record_count = 0;
    qsizetype line_start = 0;
    while (line_start < journal.size()) {
        const qsizetype line_end = journal.indexOf('\n', line_start);
        if (line_end < 0) {
            return false;
        }
        const QList<QByteArray> fields =
            journal.mid(line_start, line_end - line_start).split(' ');
        if (fields.size() != 6 ||
            (fields[0] != QByteArrayLiteral("D") &&
                fields[0] != QByteArrayLiteral("U")) ||
            !fields[3].startsWith(QByteArrayLiteral("repeat=")) ||
            !fields[4].startsWith(QByteArrayLiteral("unicode=")))
        {
            return false;
        }

        bool repeat_ok = false;
        bool unicode_ok = false;
        const int repeat = fields[3].mid(7).toInt(&repeat_ok);
        const int unicode = fields[4].mid(8).toInt(&unicode_ok);
        if (!repeat_ok || !unicode_ok || repeat < 1 ||
            unicode < 0 || unicode > 0xffff)
        {
            return false;
        }

        if (fields[0] == QByteArrayLiteral("D")) {
            ++down_record_count;
            for (int index = 0; index < repeat && unicode != 0; ++index) {
                text.append(QChar(static_cast<ushort>(unicode)));
            }
        }
        line_start = line_end + 1;
    }
    return true;
}

bool test_escape_transport_after_native_shift_return(const QString& executable_path)
{
    const QByteArray shift_return = encoded_key_event(Qt::Key_Return, Qt::ShiftModifier);
    const QByteArray escape       = encoded_key_event(Qt::Key_Escape, Qt::NoModifier);
    const QByteArray control_bracket =
        encoded_key_event(Qt::Key_BracketLeft, Qt::ControlModifier);
    const QByteArray control_three =
        encoded_key_event(Qt::Key_3, Qt::ControlModifier);
    const QByteArray alt_escape =
        encoded_key_event(Qt::Key_Escape, Qt::AltModifier);
    const QByteArray up = encoded_key_event(Qt::Key_Up, Qt::NoModifier);
    const QByteArray f12 = encoded_key_event(Qt::Key_F12, Qt::NoModifier);
    const auto fallback_scan_code = [](UINT virtual_key) {
        return static_cast<int>(MapVirtualKeyW(
            virtual_key, MAPVK_VK_TO_VSC) & 0xffU);
    };
    const int return_scan = fallback_scan_code(VK_RETURN);
    const int escape_scan = fallback_scan_code(VK_ESCAPE);
    const int tab_scan = fallback_scan_code(VK_TAB);
    const int up_scan = fallback_scan_code(VK_UP);
    const int f3_scan = fallback_scan_code(VK_F3);
    const int f12_scan = fallback_scan_code(VK_F12);
    const int letter_a_scan = fallback_scan_code('A');
    const int alt_bracket_scan = fallback_scan_code(VK_OEM_4);
    const QByteArray alt_bracket = encoded_key_event(
        Qt::Key_BracketLeft, Qt::AltModifier, QStringLiteral("["));
    const QByteArray native_alt_bracket = encoded_native_key_event(
        Qt::Key_BracketLeft, Qt::AltModifier, 0x1aU, VK_OEM_4, 0x00000004U,
        QStringLiteral("["));
    const QByteArray letter_a = encoded_key_event(
        Qt::Key_A, Qt::ShiftModifier, QStringLiteral("A"));
    const QByteArray shift_f3 = encoded_key_event(Qt::Key_F3, Qt::ShiftModifier);
    const QByteArray right_control_up = encoded_native_key_event(
        Qt::Key_Up,
        Qt::ControlModifier,
        0xe048U,
        VK_UP,
        0x01000220U,
        {});
    const QString compressed_bmp = QString::fromUtf8("\xc3\xa9x");
    const QByteArray compressed_bmp_input = encoded_native_key_event(
        Qt::Key_A, Qt::NoModifier, 0x1eU, 'A', 0U, compressed_bmp, false, 2);
    const QByteArray compressed_alt_input = encoded_native_key_event(
        Qt::Key_A, Qt::AltModifier, 0x1eU, 'A', 0x00000004U,
        compressed_bmp, false, 2);
    const QString supplementary = QString::fromUtf8("\xf0\x9f\x98\x80");
    const QByteArray supplementary_input = encoded_native_key_event(
        Qt::Key_A, Qt::NoModifier, 0x1eU, 'A', 0U, supplementary, false, 2);
    const QString packet_text = QStringLiteral("xy");
    const QByteArray packet_text_input = encoded_native_key_event(
        Qt::Key_unknown, Qt::NoModifier, 0U, VK_PACKET, 0U, packet_text, false, 4);
    const QString altgr_text = QString::fromUtf8("\xe2\x82\xac");
    const QByteArray altgr_input = encoded_native_key_event(
        Qt::Key_unknown,
        Qt::AltModifier | Qt::GroupSwitchModifier,
        0U,
        VK_PACKET,
        0x00000040U,
        altgr_text);
    const QByteArray ordinary_alt_text = encoded_key_event(
        Qt::Key_A, Qt::AltModifier, QStringLiteral("a"));
    const QByteArray packet_alt_text = encoded_native_key_event(
        Qt::Key_unknown, Qt::AltModifier, 0U, VK_PACKET, 0x00000004U,
        QStringLiteral("x"));
    const QByteArray ordinary_letter = encoded_key_event(
        Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
    const QByteArray plain_tab = encoded_key_event(
        Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t"));
    const QByteArray shift_tab = encoded_native_key_event(
        Qt::Key_Tab, Qt::ShiftModifier, 0x0fU, VK_TAB, 0x00000001U,
        QStringLiteral("\t"));
    const QByteArray control_shift_tab = encoded_native_key_event(
        Qt::Key_Tab, Qt::ShiftModifier | Qt::ControlModifier,
        0x0fU, VK_TAB, 0x00000003U, QStringLiteral("\t"));
    const QByteArray alt_shift_tab = encoded_native_key_event(
        Qt::Key_Tab, Qt::ShiftModifier | Qt::AltModifier,
        0x0fU, VK_TAB, 0x00000005U, QStringLiteral("\t"));
    const QByteArray backtab = encoded_key_event(Qt::Key_Backtab, Qt::NoModifier);
    const QByteArray control_backtab = encoded_key_event(
        Qt::Key_Backtab, Qt::ControlModifier, QStringLiteral("\t"));
    const QByteArray alt_backtab = encoded_key_event(
        Qt::Key_Backtab, Qt::AltModifier, QStringLiteral("\t"));
    const QByteArray control_alt_backtab = encoded_key_event(
        Qt::Key_Backtab, Qt::ControlModifier | Qt::AltModifier, QStringLiteral("\t"));
    const QByteArray right_alt_x = encoded_native_key_event(
        Qt::Key_X, Qt::AltModifier, 45U, static_cast<quint32>('X'),
        0x00000040U, QStringLiteral("x"));
    const QByteArray both_alt_x = encoded_native_key_event(
        Qt::Key_X, Qt::AltModifier, 45U, static_cast<quint32>('X'),
        0x00000044U, QStringLiteral("x"));
    const QByteArray left_alt_x = encoded_native_key_event(
        Qt::Key_X, Qt::AltModifier, 45U, static_cast<quint32>('X'),
        0x00000004U, QStringLiteral("x"));
    const QByteArray group_switch_right_alt_x = encoded_native_key_event(
        Qt::Key_X, Qt::AltModifier | Qt::GroupSwitchModifier, 45U,
        static_cast<quint32>('X'), 0x00000040U, QStringLiteral("x"));
    const QByteArray control_alt_return = encoded_native_key_event(
        Qt::Key_Return, Qt::ControlModifier | Qt::AltModifier,
        0x1cU, VK_RETURN, 0x00000006U, QStringLiteral("\r"));
    const QByteArray control_alt_return_lf = encoded_native_key_event(
        Qt::Key_Return, Qt::ControlModifier | Qt::AltModifier,
        0x1cU, VK_RETURN, 0x00000006U, QStringLiteral("\n"));
    const QByteArray alt_return = encoded_native_key_event(
        Qt::Key_Return, Qt::AltModifier,
        0x1cU, VK_RETURN, 0x00000004U, QStringLiteral("\r"));
    term::Terminal_input_mode_state application_keypad_modes;
    application_keypad_modes.application_keypad = true;
    const int keypad_comma_scan = fallback_scan_code(VK_SEPARATOR);
    const QByteArray application_keypad_comma = encoded_key_event(
        Qt::Key_Comma, Qt::KeypadModifier, QStringLiteral(","), application_keypad_modes);
    const QByteArray application_keypad_equal = encoded_key_event(
        Qt::Key_Equal, Qt::KeypadModifier, QStringLiteral("="), application_keypad_modes);
    const QByteArray native_application_keypad_equal = encoded_native_key_event(
        Qt::Key_Equal, Qt::KeypadModifier, 0x59U, VK_OEM_NEC_EQUAL, 0U,
        QStringLiteral("="));
    const QByteArray control_alt_return_packet_input =
        native_key_stroke_bytes(VK_PACKET, 0, 0x1b, 0) +
        native_key_stroke_bytes(VK_PACKET, 0, '\r', 0);
    const QByteArray native_escape = native_key_stroke_bytes(
        VK_ESCAPE, escape_scan, VK_ESCAPE, 0);
    const QByteArray native_alt_escape = native_escape + native_escape;
    const QByteArray barrier_sentinel =
        encoded_key_event(Qt::Key_F12, Qt::ShiftModifier);
    const QByteArray completion_sentinel = encoded_key_event(
        Qt::Key_F12, Qt::ShiftModifier | Qt::ControlModifier);

    struct Escape_input_transaction
    {
        std::vector<QByteArray> writes;
        QByteArray expected_observation;
    };

    enum class Escape_input_delivery
    {
        PACED,
        QUEUE_BURST,
    };

    QByteArray last_observed_journal;
    bool ok = true;
    ok &= check(shift_return == native_key_stroke_bytes(
            VK_RETURN, return_scan, VK_RETURN, SHIFT_PRESSED),
        "Escape transport probe uses native Win32 Shift+Return framing");
    ok &= check(escape == native_escape,
        "Escape transport probe uses native Win32 Escape framing");
    ok &= check(control_bracket == native_escape,
        "Escape transport probe uses native framing for Ctrl+[");
    ok &= check(control_three == native_escape,
        "Escape transport probe uses native framing for Ctrl+3");
    ok &= check(alt_escape == native_alt_escape,
        "Escape transport probe frames Alt+Escape as two Escape events");
    ok &= check(alt_bracket == native_key_stroke_bytes(
            VK_OEM_4, alt_bracket_scan, '[', LEFT_ALT_PRESSED),
        "Escape transport probe uses a balanced native key stroke for Alt+[");
    ok &= check(letter_a == native_key_stroke_bytes(
            'A', letter_a_scan, 'A', SHIFT_PRESSED),
        "Escape transport probe encodes uppercase A without synthetic Shift key events");
    ok &= check(shift_f3 == native_key_stroke_bytes(
            VK_F3, f3_scan, 0, SHIFT_PRESSED),
        "Escape transport probe uses a native key frame for Shift+F3");
    ok &= check(barrier_sentinel == native_key_stroke_bytes(
            VK_F12, f12_scan, 0, SHIFT_PRESSED),
        "Escape transport child-ack barrier uses a framed Shift+F12");
    ok &= check(completion_sentinel == native_key_stroke_bytes(
            VK_F12, f12_scan, 0, SHIFT_PRESSED | LEFT_CTRL_PRESSED),
        "Escape transport completion uses a framed Ctrl+Shift+F12");
    if (!ok) {
        return false;
    }

    const auto run_case = [&] (
        const QString&                               reader_mode,
        const char*                                  name,
        const std::vector<Escape_input_transaction>& transactions,
        Escape_input_delivery                        delivery,
        std::size_t                                  priming_transaction_count,
        bool                                         expect_normal_exit = true,
        std::chrono::milliseconds                    normal_exit_timeout = k_wait_timeout)
    {
        last_observed_journal.clear();
        QTemporaryDir observation_directory;
        if (!check(observation_directory.isValid(),
                "Escape transport observation directory is created")) {
            return false;
        }
        const QString observation_path =
            observation_directory.filePath(QStringLiteral("observed.bin"));
        Backend_capture capture;
        std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
        const term::Terminal_backend_result started = backend->start(
            launch_config(executable_path, {reader_mode, observation_path}),
            capture.callbacks());
        bool case_ok = check(
            started.code == term::Terminal_backend_result_code::ACCEPTED,
            "Escape transport reader starts");
        if (!case_ok) {
            return false;
        }

        const bool vt_reader =
            reader_mode == QStringLiteral("--escape-vt-input-reader") ||
            reader_mode == QStringLiteral("--escape-vt-utf16-input-reader");
        const QByteArray child_ready_marker = vt_reader
            ? QByteArrayLiteral("escape-vt-input-reader-ready")
            : QByteArrayLiteral("escape-input-reader-ready");
        const bool child_ready = capture.wait_for_output(child_ready_marker);
        case_ok &= check(child_ready,
            "Escape transport reader reaches its ready marker");
        const QByteArray ack_prefix = vt_reader
            ? QByteArrayLiteral("escape-vt-input-reader-ack[")
            : QByteArrayLiteral("escape-input-reader-ack[");
        QByteArray expected_journal;
        std::size_t ack_count = 0U;
        const auto send_sentinel_and_wait = [&](const QByteArray& sentinel) {
            const term::Terminal_backend_result write_result = backend->write(sentinel);
            if (!check(write_result.code == term::Terminal_backend_result_code::ACCEPTED,
                    "Escape transport probe queues a complete child-ack frame"))
            {
                return false;
            }

            ++ack_count;
            const QByteArray marker =
                ack_prefix + QByteArray::number(static_cast<qulonglong>(ack_count)) + ']';
            const bool acknowledged = capture.wait_for_output(marker);
            case_ok &= check(acknowledged,
                "Escape transport child flushes its journal before acknowledging input");
            return acknowledged;
        };
        bool transactions_complete = child_ready;
        for (std::size_t transaction_index = 0U;
             transaction_index < transactions.size() && transactions_complete;
             ++transaction_index)
        {
            const Escape_input_transaction& transaction = transactions[transaction_index];
            for (const QByteArray& write : transaction.writes) {
                const term::Terminal_backend_result write_result = backend->write(write);
                if (!check(write_result.code == term::Terminal_backend_result_code::ACCEPTED,
                        "Escape transport probe accepts encoded input"))
                {
                    transactions_complete = false;
                    break;
                }
            }
            if (!transactions_complete) {
                break;
            }

            expected_journal += transaction.expected_observation;
            const bool acknowledge_transaction =
                delivery == Escape_input_delivery::PACED ||
                transaction_index < priming_transaction_count;
            if (acknowledge_transaction && !send_sentinel_and_wait(barrier_sentinel)) {
                transactions_complete = false;
            }
        }

        bool completion_acknowledged = false;
        if (transactions_complete && !transactions.empty()) {
            completion_acknowledged = send_sentinel_and_wait(completion_sentinel);
        }
        case_ok &= check(completion_acknowledged,
            "Escape transport reader acknowledges the complete workload");

        const bool normal_exit_observed = completion_acknowledged &&
            capture.wait_for_exit_within(normal_exit_timeout);
        case_ok &= check(normal_exit_observed == expect_normal_exit,
            expect_normal_exit
                ? "Escape transport reader exits normally after completion"
                : "negative Escape reader remains alive after its completion acknowledgment");
        if (normal_exit_observed) {
            const std::optional<term::Terminal_backend_exit> exit =
                capture.exit_snapshot();
            case_ok &= check(exit.has_value() && exit->exit_code == 0,
                "Escape transport reader reports successful normal completion");
        }

        bool reader_reaped = normal_exit_observed;
        if (!normal_exit_observed) {
            const term::Terminal_backend_result terminated = backend->terminate();
            const bool termination_started =
                terminated.code == term::Terminal_backend_result_code::ACCEPTED ||
                capture.exit_snapshot().has_value();
            case_ok &= check(termination_started,
                "Escape transport failure cleanup can stop the reader");
            reader_reaped = capture.wait_for_exit();
            case_ok &= check(reader_reaped,
                "Escape transport failure cleanup observes the reader exit");
        }
        if (reader_reaped) {
            QFile observed_file(observation_path);
            const bool read_ok = observed_file.open(QIODevice::ReadOnly);
            const QByteArray actual = read_ok ? observed_file.readAll() : QByteArray();
            last_observed_journal = actual;
            if (!read_ok || actual != expected_journal) {
                std::cerr << name << " expected="
                    << expected_journal.toHex(' ').constData() << " actual="
                    << actual.toHex(' ').constData() << '\n';
            }
            case_ok &= check(read_ok && actual == expected_journal, name);
        }
        case_ok &= check_no_backend_errors(capture,
            "Escape transport probe produces no backend errors");
        return case_ok;
    };

    // Native key-event frames must retain key identity for classic console
    // readers, while VT-input readers receive the corresponding VT byte stream.
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "plain Escape reaches a fresh native console reader",
        {{{escape}, native_key_stroke_records(VK_ESCAPE, 1, VK_ESCAPE, 0)}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "isolated Escape reaches the child after Shift+Return",
        {
            {{shift_return}, native_key_stroke_records(
                VK_RETURN, return_scan, VK_RETURN, SHIFT_PRESSED)},
            {{escape}, native_key_stroke_records(VK_ESCAPE, 1, VK_ESCAPE, 0)},
        },
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "coalesced Shift+Return and Escape reach the child",
        {{
            {shift_return + escape},
            native_key_stroke_records(VK_RETURN, return_scan, VK_RETURN, SHIFT_PRESSED) +
                native_key_stroke_records(VK_ESCAPE, 1, VK_ESCAPE, 0),
        }},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "Ctrl+[ reaches the child after Shift+Return",
        {
            {{shift_return}, native_key_stroke_records(
                VK_RETURN, return_scan, VK_RETURN, SHIFT_PRESSED)},
            {{control_bracket}, native_key_stroke_records(VK_ESCAPE, 1, VK_ESCAPE, 0)},
        },
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "Ctrl+3 reaches the child after Shift+Return",
        {
            {{shift_return}, native_key_stroke_records(
                VK_RETURN, return_scan, VK_RETURN, SHIFT_PRESSED)},
            {{control_three}, native_key_stroke_records(VK_ESCAPE, 1, VK_ESCAPE, 0)},
        },
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "Alt+Escape reaches the child after Shift+Return",
        {
            {{shift_return}, native_key_stroke_records(
                VK_RETURN, return_scan, VK_RETURN, SHIFT_PRESSED)},
            {{alt_escape}, native_key_stroke_records(VK_ESCAPE, 1, VK_ESCAPE, 0) +
                native_key_stroke_records(VK_ESCAPE, 1, VK_ESCAPE, 0)},
        },
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "separate Escape presses produce separate balanced native strokes",
        {
            {{shift_return}, native_key_stroke_records(
                VK_RETURN, return_scan, VK_RETURN, SHIFT_PRESSED)},
            {{escape}, native_key_stroke_records(VK_ESCAPE, 1, VK_ESCAPE, 0)},
            {{escape}, native_key_stroke_records(VK_ESCAPE, 1, VK_ESCAPE, 0)},
        },
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "Up reaches the child after Shift+Return",
        {
            {{shift_return}, native_key_stroke_records(
                VK_RETURN, return_scan, VK_RETURN, SHIFT_PRESSED)},
            {{up}, native_key_stroke_records(VK_UP, up_scan, 0, ENHANCED_KEY)},
        },
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "F12 reaches the child after Shift+Return",
        {
            {{shift_return}, native_key_stroke_records(
                VK_RETURN, return_scan, VK_RETURN, SHIFT_PRESSED)},
            {{f12}, native_key_stroke_records(VK_F12, f12_scan, 0, 0)},
        },
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "Alt+[ and uppercase A remain separate native key events",
        {
            {{shift_return}, native_key_stroke_records(
                VK_RETURN, return_scan, VK_RETURN, SHIFT_PRESSED)},
            {{alt_bracket}, native_key_stroke_records(
                VK_OEM_4, alt_bracket_scan, '[', LEFT_ALT_PRESSED)},
            {{letter_a}, native_key_stroke_records(
                'A', letter_a_scan, 'A', SHIFT_PRESSED)},
        },
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "Alt+[ with explicit native fields retains its physical OEM-4 identity",
        {{{native_alt_bracket}, native_key_stroke_records(
            VK_OEM_4, 0x1a, '[', LEFT_ALT_PRESSED)}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "Shift+F3 reaches a native reader as an F3 key record",
        {
            {{shift_return}, native_key_stroke_records(
                VK_RETURN, return_scan, VK_RETURN, SHIFT_PRESSED)},
            {{shift_f3}, native_key_stroke_records(VK_F3, f3_scan, 0, SHIFT_PRESSED)},
        },
        Escape_input_delivery::PACED,
        0U);

    // Observe complete native strokes and their state bits through the exact
    // packaged ConPTY reader, not merely the encoder's private frame bytes.
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "right-Ctrl Up preserves NumLock, extended-key state, and matching key-up",
        {{
            {right_control_up},
            native_key_stroke_records(
                VK_UP,
                72,
                0,
                RIGHT_CTRL_PRESSED | NUMLOCK_ON | ENHANCED_KEY),
        }},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "ordinary unmodified A reaches the native reader as a balanced key stroke",
        {{
            {ordinary_letter},
            native_key_stroke_records('A', letter_a_scan, 'a', 0),
        }},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "Alt+A reaches the native reader with the left-Alt state on both records",
        {{
            {ordinary_alt_text},
            native_key_stroke_records('A', letter_a_scan, 'a', LEFT_ALT_PRESSED),
        }},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "compressed BMP text is ordered VK_PACKET strokes, independent of event count",
        {{
            {compressed_bmp_input},
            native_key_stroke_records(VK_PACKET, 0, 0x00e9, 0) +
                native_key_stroke_records(VK_PACKET, 0, 'x', 0),
        }},
        Escape_input_delivery::PACED,
        0U);
    {
        QString reconstructed_text;
        int down_record_count = 0;
        const bool reconstructed = reconstruct_native_record_text(
            last_observed_journal, reconstructed_text, down_record_count);
        ok &= check(reconstructed && reconstructed_text == compressed_bmp &&
                down_record_count == 2,
            "native reader reconstructs compressed BMP text in record order");
    }
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "supplementary text preserves both UTF-16 packet records in order",
        {{
            {supplementary_input},
            native_key_stroke_records(VK_PACKET, 0, 0xd83d, 0) +
                native_key_stroke_records(VK_PACKET, 0, 0xde00, 0),
        }},
        Escape_input_delivery::PACED,
        0U);
    {
        QString reconstructed_text;
        int down_record_count = 0;
        const bool reconstructed = reconstruct_native_record_text(
            last_observed_journal, reconstructed_text, down_record_count);
        ok &= check(reconstructed && reconstructed_text == supplementary &&
                down_record_count == 2,
            "native reader reconstructs a supplementary character from ordered surrogates");
    }
    // ConPTY converts the records into text for a VT-input reader. 1.25 forgets
    // a pending leading surrogate at a key-up that carries no character
    // (microsoft/terminal#19817). The reader reads UTF-16: a byte reader would
    // also see the console's own UTF-8 conversion, which splits the pair on
    // 1.24 whatever the records are.
    ok &= run_case(
        QStringLiteral("--escape-vt-utf16-input-reader"),
        "supplementary text reaches a VT-input reader whole",
        {{{supplementary_input}, supplementary.toUtf8()}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "packet text does not multiply UTF-16 units by event.count",
        {{
            {packet_text_input},
            native_key_stroke_records(VK_PACKET, 0, 'x', 0) +
                native_key_stroke_records(VK_PACKET, 0, 'y', 0),
        }},
        Escape_input_delivery::PACED,
        0U);

    // GroupSwitch/AltGr is committed text, not ordinary Alt+text. The native
    // reader verifies the actual package's Unicode records; the VT reader
    // below separately verifies the legacy plain-text byte stream.
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "AltGr text reaches a native reader as committed VK_PACKET Unicode",
        {{{altgr_input}, native_key_stroke_records(VK_PACKET, 0, 0x20ac, 0)}},
        Escape_input_delivery::PACED,
        0U);
    {
        QString reconstructed_text;
        int down_record_count = 0;
        const bool reconstructed = reconstruct_native_record_text(
            last_observed_journal, reconstructed_text, down_record_count);
        ok &= check(reconstructed && reconstructed_text == altgr_text &&
                down_record_count == 1,
            "native consumer reconstructs exactly one committed AltGr character");
    }
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "plain Tab remains a text key while Shift+Tab and Backtab are balanced",
        {
            {{plain_tab}, native_key_stroke_records(VK_TAB, tab_scan, '\t', 0, '\t')},
            {{shift_tab}, native_key_stroke_records(
                VK_TAB, tab_scan, '\t', SHIFT_PRESSED)},
            {{backtab}, native_key_stroke_records(
                VK_TAB, tab_scan, '\t', SHIFT_PRESSED)},
        },
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "modified Tab preserves VT sequences; classic readers receive VK_PACKET text",
        {
            {{control_shift_tab}, packet_key_stroke_records(decode_hex("1b5b313b365a"))},
            {{alt_shift_tab}, packet_key_stroke_records(decode_hex("1b5b313b345a"))},
            {{control_backtab}, packet_key_stroke_records(decode_hex("1b5b313b365a"))},
            {{alt_backtab}, packet_key_stroke_records(decode_hex("1b5b313b345a"))},
            {{control_alt_backtab}, packet_key_stroke_records(decode_hex("1b5b313b385a"))},
        },
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "normal keypad Equal keeps native identity; application mode prioritizes VT SS3 X",
        {
            {{application_keypad_comma}, native_key_stroke_records(
                VK_SEPARATOR, keypad_comma_scan, ',', 0)},
            {{native_application_keypad_equal}, native_key_stroke_records(
                VK_OEM_NEC_EQUAL, 0x59, '=', 0)},
            {{application_keypad_equal}, packet_key_stroke_records(
                decode_hex("1b4f58"))},
        },
        Escape_input_delivery::PACED,
        0U);

    // Closure gates for the non-text Right-Alt paths. These deliberately use
    // real native Qt metadata and the public encoder, not hand-built packets.
    // Native console records lose the selected Alt distinction in ConPTY, so
    // verify the VT bytes separately from the classic-reader packet records.
    const QByteArray gate_right_alt_up = encoded_native_key_event(
        Qt::Key_Up, Qt::AltModifier, 0xe048U, VK_UP, 0x01000040U, {});
    const QByteArray gate_right_alt_f3 = encoded_native_key_event(
        Qt::Key_F3, Qt::AltModifier, 0x3dU, VK_F3, 0x00000040U, {});
    const QByteArray gate_right_alt_return = encoded_native_key_event(
        Qt::Key_Return, Qt::AltModifier, 0x1cU, VK_RETURN, 0x00000040U,
        QStringLiteral("\r"));
    const QByteArray gate_ctrl_right_alt_return_cr = encoded_native_key_event(
        Qt::Key_Return, Qt::ControlModifier | Qt::AltModifier,
        0x1cU, VK_RETURN, 0x00000042U, QStringLiteral("\r"));
    const QByteArray gate_ctrl_right_alt_return_lf = encoded_native_key_event(
        Qt::Key_Return, Qt::ControlModifier | Qt::AltModifier,
        0x1cU, VK_RETURN, 0x00000042U, QStringLiteral("\n"));
    const QByteArray gate_right_alt_backspace = encoded_native_key_event(
        Qt::Key_Backspace, Qt::AltModifier, 0x0eU, VK_BACK, 0x00000040U,
        QStringLiteral("\b"));
    const QByteArray gate_ctrl_right_alt_a = encoded_native_key_event(
        Qt::Key_A, Qt::ControlModifier | Qt::AltModifier,
        0x1eU, 'A', 0x00000042U, QString(QChar(u'\x01')));
    const QByteArray gate_ctrl_left_alt_a_soh = encoded_native_key_event(
        Qt::Key_A, Qt::ControlModifier | Qt::AltModifier,
        0x1eU, 'A', 0x00000006U, QString(QChar(u'\x01')));
    const QByteArray gate_ctrl_left_alt_packet_m_cr = encoded_native_key_event(
        Qt::Key_M, Qt::ControlModifier | Qt::AltModifier,
        0U, VK_PACKET, 0x00000006U, QStringLiteral("\r"));
    const QByteArray gate_group_switch_up = encoded_native_key_event(
        Qt::Key_Up, Qt::GroupSwitchModifier,
        0xe048U, VK_UP, 0x01000042U, {});
    const QByteArray gate_shift_group_switch_return = encoded_native_key_event(
        Qt::Key_Return, Qt::ShiftModifier | Qt::GroupSwitchModifier,
        0x1cU, VK_RETURN, 0x00000050U, QStringLiteral("\r"));
    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "Right-Alt navigation/control keys retain their selected VT operations",
        {
            {{gate_right_alt_up}, decode_hex("1b5b313b3341")},
            {{gate_right_alt_f3}, decode_hex("1b5b313b3352")},
            {{gate_right_alt_return}, decode_hex("1b0d")},
            {{gate_ctrl_right_alt_return_cr}, decode_hex("1b0a")},
            {{gate_ctrl_right_alt_return_lf}, decode_hex("1b0a")},
            {{gate_right_alt_backspace}, decode_hex("1b7f")},
            {{gate_ctrl_right_alt_a}, decode_hex("1b01")},
            {{gate_group_switch_up}, decode_hex("1b5b41")},
            {{gate_shift_group_switch_return}, QByteArrayLiteral("\r")},
        },
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "Right-Alt VT operations reach classic readers as VK_PACKET bytes",
        {
            {{gate_right_alt_up}, packet_key_stroke_records(decode_hex("1b5b313b3341"))},
            {{gate_right_alt_f3}, packet_key_stroke_records(decode_hex("1b5b313b3352"))},
            {{gate_right_alt_return}, packet_key_stroke_records(decode_hex("1b0d"))},
            {{gate_ctrl_right_alt_return_cr}, packet_key_stroke_records(decode_hex("1b0a"))},
            {{gate_ctrl_right_alt_return_lf}, packet_key_stroke_records(decode_hex("1b0a"))},
            {{gate_right_alt_backspace}, packet_key_stroke_records(decode_hex("1b7f"))},
            {{gate_ctrl_right_alt_a}, packet_key_stroke_records(decode_hex("1b01"))},
            {{gate_group_switch_up}, packet_key_stroke_records(decode_hex("1b5b41"))},
            {{gate_shift_group_switch_return},
                packet_key_stroke_records(QByteArrayLiteral("\r"))},
        },
        Escape_input_delivery::PACED,
        0U);

    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "Ctrl+left-Alt C0 inputs preserve ESC-prefixed bytes through VT input",
        {
            {{gate_ctrl_left_alt_a_soh}, decode_hex("1b01")},
            {{gate_ctrl_left_alt_packet_m_cr}, decode_hex("1b0d")},
        },
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "Ctrl+left-Alt C0 VT-first routes reach classic readers as VK_PACKET strokes",
        {
            {{gate_ctrl_left_alt_a_soh}, packet_key_stroke_records(decode_hex("1b01"))},
            {{gate_ctrl_left_alt_packet_m_cr}, packet_key_stroke_records(decode_hex("1b0d"))},
        },
        Escape_input_delivery::PACED,
        0U);

    // Retain the selected native-identity tradeoffs while checking actual
    // transitions in one converter instance, without intervening ack keys.
    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "Right/both/left Alt and committed GroupSwitch text remain distinct in a burst",
        {
            {{shift_return}, QByteArrayLiteral("\r")},
            {{right_alt_x}, decode_hex("1b78")},
            {{both_alt_x}, decode_hex("1b78")},
            {{left_alt_x}, decode_hex("1b78")},
            {{group_switch_right_alt_x}, QByteArrayLiteral("x")},
            {{ordinary_letter}, QByteArrayLiteral("a")},
            {{right_alt_x}, decode_hex("1b78")},
        },
        Escape_input_delivery::QUEUE_BURST,
        1U);

    const QByteArray gate_native_application_equal = encoded_native_key_event(
        Qt::Key_Equal, Qt::KeypadModifier, 0x59U, VK_OEM_NEC_EQUAL, 0U,
        QStringLiteral("="), false, 1, application_keypad_modes);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "native keypad Equal metadata does not bypass application-mode packet policy",
        {{{gate_native_application_equal}, packet_key_stroke_records(decode_hex("1b4f58"))}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "application keypad Equal with supplied native metadata preserves SS3 X",
        {{{gate_native_application_equal}, decode_hex("1b4f58")}},
        Escape_input_delivery::PACED,
        0U);

    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "VT-first Right-Alt+X gives classic readers Escape then native Right-Alt+X",
        {{{right_alt_x}, native_key_stroke_records(VK_ESCAPE, 1, VK_ESCAPE, 0) +
            native_key_stroke_records('X', 45, 'x', RIGHT_ALT_PRESSED)}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "Right-Alt+X produces ESC+x without GroupSwitch",
        {{{right_alt_x}, decode_hex("1b78")}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "Right-Alt+X with left Alt held keeps both native modifier states",
        {{{both_alt_x}, native_key_stroke_records(
            'X', 45, 'x', LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "Right-Alt+X with left Alt held produces exactly one ESC prefix",
        {{{both_alt_x}, decode_hex("1b78")}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "Left-Alt+X keeps its native key and modifier",
        {{{left_alt_x}, native_key_stroke_records('X', 45, 'x', LEFT_ALT_PRESSED)}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "Left-Alt+X produces ESC+x",
        {{{left_alt_x}, decode_hex("1b78")}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "GroupSwitch right-Alt+X remains committed VK_PACKET text",
        {{{group_switch_right_alt_x}, native_key_stroke_records(VK_PACKET, 0, 'x', 0)}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "GroupSwitch right-Alt+X remains plain text",
        {{{group_switch_right_alt_x}, QByteArrayLiteral("x")}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "Ctrl+Alt+Return preserves its native key identity and modifiers",
        {{{control_alt_return}, native_key_stroke_records(
            VK_RETURN, 0x1c, '\r', LEFT_CTRL_PRESSED | LEFT_ALT_PRESSED)}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "ConPTY maps Ctrl+Alt+Return to ESC+LF",
        {{{control_alt_return}, decode_hex("1b0a")}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "Ctrl+Alt+Return with LF text preserves native fields",
        {{{control_alt_return_lf}, native_key_stroke_records(
            VK_RETURN, 0x1c, '\n', LEFT_CTRL_PRESSED | LEFT_ALT_PRESSED)}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "ConPTY maps Ctrl+Alt+Return with LF text to ESC+LF",
        {{{control_alt_return_lf}, decode_hex("1b0a")}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "Alt+Return preserves native VK_RETURN and Alt state",
        {{{alt_return}, native_key_stroke_records(
            VK_RETURN, 0x1c, '\r', LEFT_ALT_PRESSED)}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "Alt+Return remains ESC+CR",
        {{{alt_return}, decode_hex("1b0d")}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "packet Ctrl+Alt+Return can emit ESC+CR for VT input",
        {{{control_alt_return_packet_input}, decode_hex("1b0d")}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "packet Ctrl+Alt+Return loses native VK_RETURN identity",
        {{{control_alt_return_packet_input},
            native_key_stroke_records(VK_PACKET, 0, 0x1b, 0) +
                native_key_stroke_records(VK_PACKET, 0, '\r', 0)}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "application-keypad Equal yields SS3 X in VT input",
        {{{application_keypad_equal}, decode_hex("1b4f58")}},
        Escape_input_delivery::PACED,
        0U);
    // Keep ordinary Alt's legacy prefix distinct from committed AltGr text.
    // A compressed multi-unit Alt event is one Escape stroke followed by all
    // committed text units.
    ok &= run_case(
        QStringLiteral("--escape-input-reader"),
        "compressed ordinary Alt text keeps one Escape before all Unicode units",
        {{{compressed_alt_input},
            native_key_stroke_records(VK_ESCAPE, 1, VK_ESCAPE, 0) +
                native_key_stroke_records(VK_PACKET, 0, 0x00e9, 0) +
                native_key_stroke_records(VK_PACKET, 0, 'x', 0)}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "compressed ordinary Alt text preserves ESC and the complete UTF-8 payload",
        {{{compressed_alt_input}, decode_hex("1bc3a978")}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "ordinary Alt+A retains the legacy ESC-prefixed VT byte",
        {{{ordinary_alt_text}, decode_hex("1b61")}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "single VK_PACKET Alt text delivers exactly ESC followed by its character",
        {{{packet_alt_text}, decode_hex("1b78")}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "AltGr GroupSwitch text remains plain UTF-8 without an ESC prefix",
        {{{altgr_input}, altgr_text.toUtf8()}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "Tab variants retain their distinct VT byte sequences",
        {
            {{plain_tab}, QByteArrayLiteral("\t")},
            {{shift_tab}, decode_hex("1b5b5a")},
            {{backtab}, decode_hex("1b5b5a")},
            {{control_shift_tab}, decode_hex("1b5b313b365a")},
            {{alt_shift_tab}, decode_hex("1b5b313b345a")},
            {{control_backtab}, decode_hex("1b5b313b365a")},
            {{alt_backtab}, decode_hex("1b5b313b345a")},
            {{control_alt_backtab}, decode_hex("1b5b313b385a")},
        },
        Escape_input_delivery::PACED,
        0U);

    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "plain Escape reaches a VT reader",
        {{{escape}, decode_hex("1b")}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "Alt+Escape maps to two ESC bytes for VT input",
        {{{alt_escape}, decode_hex("1b1b")}},
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "isolated Escape reaches a VT reader after Shift+Return",
        {
            {{shift_return}, decode_hex("0d")},
            {{escape}, decode_hex("1b")},
        },
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "Up reaches a VT reader after Shift+Return",
        {
            {{shift_return}, decode_hex("0d")},
            {{up}, decode_hex("1b5b41")},
        },
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "Alt+[ followed by A forms the standard VT Up sequence",
        {
            {{shift_return}, decode_hex("0d")},
            {{alt_bracket}, decode_hex("1b5b")},
            {{letter_a}, decode_hex("41")},
        },
        Escape_input_delivery::PACED,
        0U);
    ok &= run_case(
        QStringLiteral("--escape-vt-input-reader"),
        "Shift+F3 reaches a VT reader as its CPR-like sequence",
        {
            {{shift_return}, decode_hex("0d")},
            {{shift_f3}, decode_hex("1b5b313b3252")},
        },
        Escape_input_delivery::PACED,
        0U);

    // A complete priming frame is acknowledged before any split frame. Each
    // split's prefix and suffix stay in one transaction; only the completed
    // journal is asserted, not parser state between the two writes.
    for (const bool vt_reader : {false, true}) {
        const QByteArray observation = vt_reader
            ? QByteArrayLiteral("\x1b")
            : native_key_stroke_records(VK_ESCAPE, 1, VK_ESCAPE, 0);
        std::vector<Escape_input_transaction> transactions{
            {{escape}, observation},
        };
        for (qsizetype split = 1; split < escape.size(); ++split) {
            transactions.push_back({
                {escape.left(split), escape.mid(split)},
                observation,
            });
        }
        ok &= run_case(
            vt_reader ? QStringLiteral("--escape-vt-input-reader")
                      : QStringLiteral("--escape-input-reader"),
            vt_reader ? "activated parser preserves every complete-frame split in VT mode"
                      : "activated parser preserves every complete-frame split in native mode",
            transactions,
            Escape_input_delivery::PACED,
            0U);
    }

    const std::vector<Escape_input_transaction> native_queue_workload{
        {{shift_return}, native_key_stroke_records(
            VK_RETURN, return_scan, VK_RETURN, SHIFT_PRESSED)},
        {{escape}, native_key_stroke_records(VK_ESCAPE, 1, VK_ESCAPE, 0)},
        {{up}, native_key_stroke_records(VK_UP, up_scan, 0, ENHANCED_KEY)},
        {{f12}, native_key_stroke_records(VK_F12, f12_scan, 0, 0)},
    };
    const std::vector<Escape_input_transaction> vt_queue_workload{
        {{shift_return}, decode_hex("0d")},
        {{escape}, decode_hex("1b")},
        {{up}, decode_hex("1b5b41")},
        {{f12}, decode_hex("1b5b32347e")},
    };
    for (const bool vt_reader : {false, true}) {
        const QString reader_mode = vt_reader
            ? QStringLiteral("--escape-vt-input-reader")
            : QStringLiteral("--escape-input-reader");
        const std::vector<Escape_input_transaction>& workload = vt_reader
            ? vt_queue_workload
            : native_queue_workload;
        ok &= run_case(
            reader_mode,
            vt_reader ? "paced VT input transactions acknowledge each complete frame"
                      : "paced native input transactions acknowledge each complete frame",
            workload,
            Escape_input_delivery::PACED,
            0U);
        ok &= run_case(
            reader_mode,
            vt_reader ? "unpaced VT queue burst drains before its final acknowledgment"
                      : "unpaced native queue burst drains before its final acknowledgment",
            workload,
            Escape_input_delivery::QUEUE_BURST,
            1U);
    }

    ok &= run_case(
        QStringLiteral("--escape-input-reader-hold-after-ack"),
        "completion acknowledgment does not hide a reader that misses normal exit",
        {{{ordinary_letter}, native_key_stroke_records(
            'A', letter_a_scan, 'a', 0)}},
        Escape_input_delivery::PACED,
        0U,
        false,
        std::chrono::milliseconds(250));

    return ok;
}

bool test_unicode_paste_preserves_console_input(const QString& executable_path)
{
    QTemporaryDir directory;
    if (!check(directory.isValid(), "Unicode paste fixture has a temporary directory")) {
        return false;
    }

    const QString output_path = directory.filePath(QStringLiteral("paste.txt"));
    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    const auto started = backend->start(
        launch_config(executable_path, {QStringLiteral("--paste-input-reader"), output_path}),
        capture.callbacks());
    if (!check(started.code == term::Terminal_backend_result_code::ACCEPTED,
            "Unicode paste reader starts") ||
        !check(capture.wait_for_output(QByteArrayLiteral("paste-reader-ready")),
            "Unicode paste reader is ready"))
    {
        return false;
    }

    // Long box borders cross the console host's input read boundaries. The
    // reader filters key releases, as interactive console clients do.
    const QString text =
        QStringLiteral("\u256d") + QString(80, QChar(0x2500)) + QStringLiteral("\u256e\n") +
        QStringLiteral("\u2502 OpenAI Codex 23\u00b0 C \u20ac \u2502\n") +
        QStringLiteral("\u2570") + QString(80, QChar(0x2500)) + QStringLiteral("\u256f");
    QByteArray payload = term::encode_terminal_paste_text(
        text, {}, term::Terminal_paste_framing_policy::ENABLED);
    payload.append('!');
    bool ok = check(backend->write(payload).code == term::Terminal_backend_result_code::ACCEPTED,
        "Unicode paste is accepted by the native backend");
    ok &= check(capture.wait_for_exit(), "Unicode paste reader exits");
    QFile output(output_path);
    if (!check(output.open(QIODevice::ReadOnly), "Unicode paste reader records received text")) {
        return false;
    }
    const QByteArray received = output.readAll();
    // The console delivers exactly what was written, so the encoded paste is
    // the expectation: its line breaks are carriage returns, not the line feeds
    // the source text was written with.
    QByteArray expected = payload;
    expected.chop(1);
    if (received != expected) {
        std::cerr << "Unicode paste received=" << received.toHex(' ').constData() << '\n';
    }
    ok &= check(received == expected, "Unicode paste preserves box borders and symbols without protocol residue");
    ok &= check_no_backend_errors(capture, "Unicode paste produces no backend errors");
    return ok;
}

bool test_terminal_child_starts_with_default_error_mode(const QString& reporter_path)
{
    // Stand in for a host that suppresses loader dialogs for its own helpers.
    const UINT suppression = SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX;
    const UINT host_mode   = SetErrorMode(GetErrorMode() | suppression);
    bool ok = check((GetErrorMode() & suppression) == suppression,
        "hosting process suppresses loader dialogs");

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    const auto started = backend->start(launch_config(reporter_path, {}), capture.callbacks());
    SetErrorMode(host_mode);
    if (!check(started.code == term::Terminal_backend_result_code::ACCEPTED,
            "error mode reporter starts"))
    {
        return false;
    }

    ok &= check(capture.wait_for_exit(), "error mode reporter exits");
    const QRegularExpressionMatch reported =
        QRegularExpression(QStringLiteral("error-mode=(\\d+)"))
            .match(QString::fromUtf8(capture.output_snapshot()));
    bool parsed = false;
    const UINT child_mode = reported.captured(1).toUInt(&parsed);
    std::cerr << "terminal child error mode=0x" << std::hex << child_mode << std::dec << '\n';
    ok &= check(parsed, "error mode reporter prints its error mode");
    ok &= check((child_mode & suppression) == 0,
        "terminal child keeps the loader dialogs its hosting process suppresses");
    ok &= check_no_backend_errors(capture, "error mode reporter produces no backend errors");
    return ok;
}

bool test_resize_storm_reports_final_shell_size(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    const term::Terminal_backend_result start_result =
        backend->start(
            launch_config(fixture_path, {QStringLiteral("--shell-like-smoke")}),
            capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "resize-storm shell fixture starts");
    if (start_result.code != term::Terminal_backend_result_code::ACCEPTED) {
        return false;
    }

    ok &= check(capture.wait_for_output(shell_fixture_prompt()),
        "resize-storm shell prompt reaches output");

    std::uint64_t resize_id = 1U;
    ok &= apply_resize_storm(
        *backend,
        resize_id,
        "ConPTY backend accepts resize storm step");

    const term::terminal_grid_size_t final_grid = resize_storm_grid_sizes().back();
    ok &= wait_for_shell_size_report(
        *backend,
        capture,
        final_grid,
        "resize-storm shell reports final ConPTY size");

    ok &= stop_shell_like_fixture(*backend, capture);
    return ok;
}

bool test_resize_interleaved_with_shell_output(const QString& fixture_path)
{
    bool ok = true;

    std::uint64_t resize_id = 1U;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    const term::Terminal_backend_result start_result =
        backend->start(
            launch_config(fixture_path, {QStringLiteral("--shell-like-smoke")}),
            capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "output-interleaved resize shell fixture starts");
    if (start_result.code != term::Terminal_backend_result_code::ACCEPTED) {
        return false;
    }

    ok &= check(capture.wait_for_output(shell_fixture_prompt()),
        "output-interleaved resize shell prompt reaches output");

    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();
    const int stream_count = contract.stream_max_count;

    const term::terminal_grid_size_t final_grid = resize_storm_grid_sizes().back();
    const term::Terminal_backend_result stream_write_result =
        backend->write(shell_fixture_gated_stream_command(
            stream_count,
            final_grid,
            gated_stream_resize_pad_rows()));
    ok &= check(stream_write_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "output-interleaved resize shell accepts gated stream command");
    ok &= check(capture.wait_for_output(shell_fixture_stream_line(1)),
        "output-interleaved resize shell starts stream output");
    ok &= check(capture.wait_for_output(shell_fixture_gated_stream_ready_marker()),
        "output-interleaved resize shell pauses with pending stream rows");

    ok &= apply_resize_storm(
        *backend,
        resize_id,
        "ConPTY backend accepts output-interleaved resize storm step");
    ok &= check(capture.wait_for_output(shell_fixture_gated_stream_resized_marker(final_grid)),
        "output-interleaved resize shell observes final resize before completing stream");
    ok &= check(shell_fixture_authored_stream_row_count(capture.output_snapshot()) == 1U,
        "output-interleaved resize shell has exactly one authored stream row before continue");

    const term::Terminal_backend_result continue_write_result =
        backend->write(shell_fixture_gated_stream_continue_command());
    ok &= check(continue_write_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "output-interleaved resize shell accepts gated stream continue command");
    ok &= check(capture.wait_for_output(shell_fixture_stream_line(stream_count)),
        "output-interleaved resize shell reaches final stream row after resize");

    const QByteArray output = capture.output_snapshot();
    ok &= shell_fixture_stream_rows_are_exactly_ordered(
        output,
        stream_count,
        "output-interleaved resize shell preserves authored stream rows exactly once in order");

    ok &= wait_for_shell_size_report(
        *backend,
        capture,
        final_grid,
        "output-interleaved resize shell reports final ConPTY size");

    ok &= stop_shell_like_fixture(*backend, capture);
    return ok;
}

bool test_scroll_region_scrollback_survives_conpty(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    term::Terminal_launch_config config = launch_config(
        fixture_path,
        {
            QStringLiteral("--behavior-smoke"),
            QStringLiteral("primary-scrollback-insert"),
        });
    config.initial_grid_size = term::terminal_grid_size_t{5, 20};

    const term::Terminal_backend_result start_result =
        backend->start(config, capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY starts scroll-region scrollback fixture");
    ok &= check(capture.wait_for_exit(), "scroll-region scrollback fixture exits");
    ok &= check_no_backend_errors(capture,
        "scroll-region scrollback fixture produces no backend errors");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::EXITED &&
        exit->exit_code == 0,
        "scroll-region scrollback fixture reports clean exit");

    term::Terminal_screen_model_config recovery_model_config;
    recovery_model_config.grid_size                                = {5, 20};
    recovery_model_config.scrollback_limit                         = 32;
    recovery_model_config.tab_width                                = 8;
    recovery_model_config.recover_scrollback_from_primary_repaints = true;
    recovery_model_config.retain_structural_actions                = true;
    term::Terminal_screen_model model(recovery_model_config);
    const term::Terminal_screen_model_result result =
        model.ingest(capture.output_snapshot());
    // The host can advertise extensions that this model ignores. Malformed or
    // truncated output still fails the scrollback integration contract.
    ok &= check(parser_failure_count(result) == 0,
        "ConPTY-observed scroll-region scrollback has no parsing failures");
    if (!check(model.scrollback_size() > 0,
        "ConPTY-observed scroll-region scrollback creates model scrollback"))
    {
        std::cerr
            << "ConPTY scroll-region output hex: "
            << capture.output_snapshot().toHex(' ').constData()
            << '\n';
        ok = false;
    }

    return ok;
}

bool test_utf8_payload_preserves_exact_conpty_bytes(const QString& fixture_path)
{
    bool ok = true;

    const QByteArray expected_payload = conpty_utf8_payload();
    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    const term::Terminal_backend_result start_result =
        backend->start(
            launch_config(fixture_path, {QStringLiteral("--utf8-payload")}),
            capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "UTF-8 payload fixture starts");
    if (start_result.code != term::Terminal_backend_result_code::ACCEPTED) {
        return false;
    }

    ok &= check(capture.wait_for_output(expected_payload),
        "ConPTY backend receives deterministic UTF-8 payload bytes");
    ok &= check(capture.wait_for_exit(), "UTF-8 payload fixture exits");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::EXITED &&
        exit->exit_code == 0,
        "UTF-8 payload fixture reports clean exit");
    ok &= check(count_occurrences(capture.output_snapshot(), expected_payload) == 1U,
        "ConPTY backend receives the exact authored UTF-8 payload once");
    ok &= check_no_backend_errors(capture,
        "UTF-8 payload fixture produces no backend errors");

    return ok;
}

bool test_sync_raw_resize_gate_preserves_order(const QString& fixture_path)
{
    bool ok = true;

    QTemporaryDir checkpoint_dir;
    ok &= check(checkpoint_dir.isValid(), "sync raw resize gate checkpoint directory is available");
    if (!checkpoint_dir.isValid()) {
        return false;
    }

    const QString checkpoint_path =
        checkpoint_dir.filePath(QStringLiteral("sync-raw-resize.checkpoint"));

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    const term::Terminal_backend_result start_result =
        backend->start(
            launch_config(
                fixture_path,
                {
                    QStringLiteral("--sync-raw-resize-gate"),
                    checkpoint_path,
                }),
            capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "raw synchronized-output gate fixture starts");
    if (start_result.code != term::Terminal_backend_result_code::ACCEPTED) {
        return false;
    }

    ok &= check(wait_for_file(checkpoint_path),
        "raw synchronized-output fixture reaches gate before release write");

    std::uint64_t resize_id = 1U;
    ok &= apply_resize_storm(
        *backend,
        resize_id,
        "ConPTY backend accepts resize storm between synchronized-output writes");

    const term::Terminal_backend_result continue_result =
        backend->write(QByteArrayLiteral("x"));
    ok &= check(continue_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "raw synchronized-output gate fixture accepts continue byte");
    ok &= check(capture.wait_for_output(conpty_sync_final_payload()),
        "raw synchronized-output gate fixture emits final payload after resize storm");
    ok &= check(capture.wait_for_exit(),
        "raw synchronized-output gate fixture exits");

    const QByteArray output = capture.output_snapshot();
    ok &= check(output.contains(conpty_sync_hidden_payload()),
        "ConPTY raw output contains synchronized-output hidden payload");
    // This raw backend stress intentionally depends on ConPTY passing
    // DECSET/DECRST 2026 mode bytes through as output instead of interpreting
    // them inside the backend layer.
    if (!output.contains(conpty_sync_release_sequence())) {
        std::cerr << "raw synchronized-output capture hex: "
            << output.toHex(' ').constData() << '\n';
    }
    ok &= output_contains_in_order(
        output,
        {
            conpty_sync_enter_sequence(),
            conpty_sync_hidden_payload(),
            conpty_sync_release_sequence(),
            conpty_sync_final_payload(),
        },
        "ConPTY raw output preserves gated synchronized-output write order");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::EXITED &&
        exit->exit_code == 0,
        "raw synchronized-output gate fixture reports clean exit");
    ok &= check_no_backend_errors(capture,
        "raw synchronized-output gate fixture produces no backend errors");

    return ok;
}

bool test_fast_start_exit_loop(const QString& fixture_path)
{
    bool ok = true;

    constexpr int k_iterations = 12;
    for (int iteration = 0; iteration < k_iterations; ++iteration) {
        Backend_capture capture;
        std::unique_ptr<term::Terminal_backend> backend =
            term::make_windows_conpty_backend();
        const term::Terminal_backend_result start_result =
            backend->start(
                launch_config(fixture_path, {QStringLiteral("--quick-exit")}),
                capture.callbacks());
        ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
            "quick-exit fixture starts");
        if (start_result.code != term::Terminal_backend_result_code::ACCEPTED) {
            return false;
        }

        ok &= check(capture.wait_for_output(conpty_quick_exit_payload()),
            "quick-exit fixture marker reaches backend output");
        ok &= check(capture.wait_for_exit(), "quick-exit fixture exits");
        ok &= check(capture.exit_count_snapshot() == 1,
            "quick-exit fixture reports one process exit callback");

        const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
        ok &= check(exit.has_value() &&
            exit->reason == term::Terminal_exit_reason::EXITED &&
            exit->exit_code == 0,
            "quick-exit fixture reports clean exit");
        ok &= check(count_occurrences(
            capture.output_snapshot(),
            conpty_quick_exit_payload()) == 1U,
            "quick-exit fixture emits one authored marker");
        ok &= check_no_backend_errors(capture,
            "quick-exit fixture produces no backend errors");
        const term::Terminal_backend_result after_exit = backend->write(QByteArrayLiteral("after"));
        ok &= check(after_exit.code == term::Terminal_backend_result_code::REJECTED &&
                after_exit.child_exited,
            "ConPTY write after the quick-exit fixture exits reports the exited child");
    }

    return ok;
}

bool test_paused_release_with_tight_delivery_limits_chunks_conpty_output()
{
    bool ok = true;

    QTemporaryDir checkpoint_dir;
    ok &= check(checkpoint_dir.isValid(),
        "tight ConPTY paused-output checkpoint directory is available");
    if (!checkpoint_dir.isValid()) {
        return false;
    }

    const QString script_path =
        checkpoint_dir.filePath(QStringLiteral("tight-paused-output.ps1"));
    const QString ready_path =
        checkpoint_dir.filePath(QStringLiteral("tight-paused-output.ready"));
    const QString done_path =
        checkpoint_dir.filePath(QStringLiteral("tight-paused-output.done"));
    const QString exit_gate_path =
        checkpoint_dir.filePath(QStringLiteral("tight-paused-output-exit.gate"));
    const QByteArray payload = QByteArrayLiteral("~~~~~~~~~~~~~~");

    ok &= check(
        write_tight_paused_output_powershell_script(
            script_path,
            ready_path,
            done_path,
            exit_gate_path),
        "tight ConPTY paused-output PowerShell script is writable");
    if (!ok) {
        return false;
    }

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    term::Terminal_launch_config config = powershell_script_launch_config(script_path);
    constexpr std::size_t k_replay_chunk_bytes = 4U;
    constexpr std::size_t k_paused_budget_bytes = 20U;
    config.output_delivery_limits =
        term::Terminal_backend_output_delivery_limits{
            k_replay_chunk_bytes,
            term::k_native_backend_output_read_chunk_bytes +
                k_replay_chunk_bytes +
                k_paused_budget_bytes};

    const term::Terminal_backend_result start_result =
        backend->start(config, capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "tight ConPTY paused-output fixture starts");
    ok &= check(wait_for_file(ready_path),
        "tight ConPTY paused-output fixture reaches ready marker");

    const term::Terminal_backend_result pause_result =
        backend->set_output_paused(true);
    ok &= check(pause_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "tight ConPTY paused-output fixture accepts pause");
    const std::size_t output_events_before_release =
        capture.output_event_count_snapshot();
    ok &= check(backend->write(QByteArrayLiteral("g")).code ==
        term::Terminal_backend_result_code::ACCEPTED,
        "tight ConPTY paused-output fixture receives release input");
    ok &= check(wait_for_file(done_path),
        "tight ConPTY paused-output fixture writes payload while paused");
    ok &= check(!capture.wait_for_output_within(
            payload.left(1),
            std::chrono::milliseconds(250)),
        "tight ConPTY paused-output fixture holds payload before resume");

    const term::Terminal_backend_result resume_result =
        backend->set_output_paused(false);
    ok &= check(resume_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "tight ConPTY paused-output fixture accepts resume");
    ok &= check(capture.wait_for_output(payload),
        "tight ConPTY paused-output fixture delivers ordered paused payload");
    ok &= check(write_gate_file(exit_gate_path),
        "tight ConPTY paused-output fixture exit gate opens after resume");
    ok &= check(capture.wait_for_exit(),
        "tight ConPTY paused-output fixture reports exit");

    std::size_t max_payload_callback_bytes = 0U;
    std::size_t payload_callback_count = 0U;
    const std::vector<QByteArray> output_events = capture.output_events_snapshot();
    for (std::size_t index = output_events_before_release;
         index < output_events.size();
         ++index)
    {
        const QByteArray& event = output_events[index];
        const bool contains_payload_byte = std::any_of(
            event.cbegin(),
            event.cend(),
            [&payload](char byte) { return payload.contains(byte); });
        if (contains_payload_byte) {
            max_payload_callback_bytes = std::max(
                max_payload_callback_bytes,
                static_cast<std::size_t>(event.size()));
            ++payload_callback_count;
        }
    }
    if (max_payload_callback_bytes > k_replay_chunk_bytes) {
        std::cerr << "tight ConPTY paused-output payload callback sizes:";
        for (std::size_t index = output_events_before_release;
             index < output_events.size();
             ++index)
        {
            const QByteArray& event = output_events[index];
            if (event.contains('~')) {
                std::cerr << ' ' << event.size()
                    << ':' << event.toHex().constData();
            }
        }
        std::cerr << '\n';
    }
    ok &= check(max_payload_callback_bytes <= k_replay_chunk_bytes,
        "tight ConPTY paused-output callback size stays within configured limit");
    ok &= check(payload_callback_count >= 4U,
        "tight ConPTY paused-output fixture delivers payload across multiple callbacks");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::EXITED &&
        exit->exit_code == 0,
        "tight ConPTY paused-output fixture reports clean exit");
    ok &= check_no_backend_errors(capture,
        "tight ConPTY paused-output fixture produces no backend errors");

    return ok;
}

bool test_exit_drain_ignores_repause_and_delivers_buffered_output(const QString& fixture_path)
{
    bool ok = true;

    const QByteArray marker            = QByteArrayLiteral("paused-exit-drain");
    std::atomic_bool repause_attempted = false;
    std::atomic_bool repause_accepted  = false;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend     = term::make_windows_conpty_backend();
    term::Terminal_backend*                 backend_ptr = backend.get();
    const term::Terminal_backend_result start_result =
        backend->start(
            launch_config(fixture_path, {QStringLiteral("--shell-like-smoke")}),
            capture.callbacks([&](const QByteArray& bytes) {
                if (bytes.contains(marker) && !repause_attempted.exchange(true)) {
                    const term::Terminal_backend_result pause_result =
                        backend_ptr->set_output_paused(true);
                    repause_accepted =
                        pause_result.code == term::Terminal_backend_result_code::ACCEPTED;
                }
            }));
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "paused exit-drain shell starts");
    ok &= check(capture.wait_for_output(shell_fixture_prompt()),
        "paused exit-drain shell reaches prompt");

    const term::Terminal_backend_result pause_result = backend->set_output_paused(true);
    ok &= check(pause_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend accepts pre-exit output pause");
    ok &= check(backend->write(QByteArrayLiteral("echo paused-exit-drain\r")).code ==
        term::Terminal_backend_result_code::ACCEPTED,
        "paused exit-drain shell receives marker command");
    ok &= check(backend->write(QByteArrayLiteral("exit\r")).code ==
        term::Terminal_backend_result_code::ACCEPTED,
        "paused exit-drain shell receives exit command");
    ok &= check(capture.wait_for_output(marker),
        "exit drain delivers output buffered before child exit");
    ok &= check(repause_attempted,
        "exit drain callback attempted to re-pause output");
    ok &= check(repause_accepted,
        "exit drain re-pause request remains accepted");
    ok &= check(capture.wait_for_exit(),
        "exit drain re-pause does not block exit reporting");
    ok &= check(capture.output_precedes_exit(marker),
        "exit drain delivers buffered output before exit callback");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::EXITED &&
        exit->exit_code == 0,
        "paused exit-drain shell reports direct child clean exit");
    ok &= check_no_backend_errors(capture,
        "paused exit-drain shell produces no backend errors");

    return ok;
}

bool test_resize_races_natural_exit_close(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    const term::Terminal_backend_result start_result =
        backend->start(
            launch_config(fixture_path, {QStringLiteral("--shell-like-smoke")}),
            capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "resize-close race shell starts");
    ok &= check(capture.wait_for_output(shell_fixture_prompt()),
        "resize-close race shell reaches prompt");
    if (!ok) {
        return false;
    }

    const std::vector<term::terminal_grid_size_t>& sizes = resize_storm_grid_sizes();
    const term::Terminal_backend_result first_resize = backend->resize({
        1U,
        sizes.front(),
    });
    ok &= check(first_resize.code == term::Terminal_backend_result_code::ACCEPTED,
        "resize-close race accepts deterministic resize before exit");
    if (!ok) {
        return false;
    }

    std::atomic_bool stop_resizes = false;
    std::atomic_bool resize_results_valid = true;
    std::thread resize_thread([&] {
        std::uint64_t resize_id = 2U;
        std::size_t size_index = 1U % sizes.size();
        while (!stop_resizes.load(std::memory_order_acquire)) {
            const term::Terminal_backend_result resize_result = backend->resize({
                resize_id++,
                sizes[size_index],
            });
            size_index = (size_index + 1U) % sizes.size();

            if (resize_result.code != term::Terminal_backend_result_code::ACCEPTED &&
                resize_result.code != term::Terminal_backend_result_code::REJECTED)
            {
                resize_results_valid = false;
                stop_resizes = true;
            }
        }
    });

    ok &= check(backend->write(QByteArrayLiteral("exit\r")).code ==
        term::Terminal_backend_result_code::ACCEPTED,
        "resize-close race shell receives exit command");
    ok &= check(capture.wait_for_exit(),
        "resize-close race shell exits");

    stop_resizes = true;
    resize_thread.join();

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::EXITED &&
        exit->exit_code == 0,
        "resize-close race shell reports direct child clean exit");
    ok &= check(resize_results_valid,
        "resize-close race only accepts or rejects public resize calls");
    ok &= check_no_backend_errors(capture,
        "resize-close race shell produces no backend errors");

    return ok;
}

bool test_start_resize_terminate_ordering(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    term::Terminal_launch_config config =
        launch_config(fixture_path, {QStringLiteral("--hold-open-no-read")});
    config.termination_policy.graceful_interval = std::chrono::milliseconds(0);
    config.termination_policy.kill_interval     = std::chrono::milliseconds(500);

    const term::Terminal_backend_result start_result =
        backend->start(config, capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "start-resize-terminate fixture starts");
    if (start_result.code != term::Terminal_backend_result_code::ACCEPTED) {
        return false;
    }

    std::uint64_t resize_id = 1U;
    ok &= apply_resize_storm(
        *backend,
        resize_id,
        "ConPTY backend accepts resize immediately after start");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("hold-open-no-read")),
        "start-resize-terminate fixture reaches hold-open path before terminate");

    const term::Terminal_backend_result terminate_result = backend->terminate();
    ok &= check(terminate_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend accepts terminate after start-time resize storm");
    ok &= check(backend->resize({resize_id++, {24, 80}}).code ==
        term::Terminal_backend_result_code::REJECTED,
        "ConPTY resize rejects after terminate is accepted");
    ok &= check(capture.wait_for_exit(),
        "start-resize-terminate fixture exits");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::TERMINATED,
        "start-resize-terminate reports typed terminated exit");
    ok &= check_no_backend_errors(capture,
        "start-resize-terminate fixture produces no backend errors");

    return ok;
}

bool test_terminate_after_resize_storm_stops_child_process(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    Win32_process_handle child_process;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    term::Terminal_launch_config config =
        launch_config(fixture_path, {QStringLiteral("--hold-open-pid-no-read")});
    config.termination_policy.graceful_interval = std::chrono::milliseconds(0);
    config.termination_policy.kill_interval     = std::chrono::milliseconds(500);

    const term::Terminal_backend_result start_result =
        backend->start(config, capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "terminate-after-resize-storm fixture starts");
    if (start_result.code != term::Terminal_backend_result_code::ACCEPTED) {
        return false;
    }

    const std::optional<DWORD> child_pid =
        capture.wait_for_pid_output(QByteArrayLiteral("hold-open-pid-no-read "));
    ok &= check(child_pid.has_value(),
        "terminate-after-resize-storm fixture reports child pid");
    if (!child_pid.has_value()) {
        return false;
    }

    child_process = open_process_handle(*child_pid);
    ok &= check(child_process.is_valid(),
        "terminate-after-resize-storm child process handle opens");
    if (!child_process.is_valid()) {
        return false;
    }

    const std::optional<bool> child_running =
        process_handle_is_running(child_process.get());
    ok &= check(child_running.has_value() && *child_running,
        "terminate-after-resize-storm child process is alive before terminate");

    std::uint64_t resize_id = 1U;
    ok &= apply_resize_storm(
        *backend,
        resize_id,
        "ConPTY backend accepts lifecycle resize storm step");

    const term::Terminal_backend_result terminate_result = backend->terminate();
    ok &= check(terminate_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend accepts terminate after lifecycle resize storm");
    ok &= check(capture.wait_for_exit(),
        "terminate-after-resize-storm fixture exits");

    const bool child_exited = wait_for_process_exit(child_process.get());
    ok &= check(child_exited,
        "terminate-after-resize-storm child process exits");
    ok &= check(capture.exit_count_snapshot() == 1,
        "terminate-after-resize-storm reports one process exit callback");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::TERMINATED,
        "terminate-after-resize-storm reports typed terminated exit");
    ok &= check(backend->resize({resize_id++, {24, 80}}).code ==
        term::Terminal_backend_result_code::REJECTED,
        "ConPTY resize after lifecycle termination rejects");
    ok &= check(backend->write(QByteArrayLiteral("after")).code ==
        term::Terminal_backend_result_code::REJECTED,
        "ConPTY write after lifecycle termination rejects");
    ok &= check_no_backend_errors(capture,
        "terminate-after-resize-storm fixture produces no backend errors");

    return ok;
}

bool test_held_resize_allows_stop_escalation(const QString& fixture_path)
{
    bool ok = true;
    Backend_capture capture;
    auto backend = std::make_unique<term::Windows_conpty_backend>();
    auto resize_control = std::make_shared<term::Windows_conpty_resize_control_for_testing>();
    auto close_control  = std::make_shared<term::Windows_conpty_close_control_for_testing>();
    close_control->allow_close.release();
    close_control->allow_observer.release();
    ok &= check(backend->set_resize_control_for_testing(resize_control),
        "held-resize gate is installed before native start");
    ok &= check(backend->set_close_control_for_testing(close_control),
        "held-resize close observer is installed before native start");
    if (!ok) {
        return false;
    }

    term::Terminal_launch_config config =
        launch_config(fixture_path, {QStringLiteral("--hold-open-pid-no-read")});
    config.termination_policy.graceful_interval = std::chrono::milliseconds(0);
    config.termination_policy.kill_interval     = std::chrono::milliseconds(500);
    const term::Terminal_backend_result start_result = backend->start(config, capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "held-resize fixture starts");
    if (start_result.code != term::Terminal_backend_result_code::ACCEPTED) {
        return false;
    }

    const std::optional<DWORD> child_pid =
        capture.wait_for_pid_output(QByteArrayLiteral("hold-open-pid-no-read "));
    ok &= check(child_pid.has_value(), "held-resize fixture reports its child pid");
    Win32_process_handle child_process =
        child_pid.has_value() ? open_process_handle(*child_pid) : Win32_process_handle{};
    ok &= check(child_process.is_valid(), "held-resize child handle opens");
    if (!child_process.is_valid()) {
        (void)backend->terminate();
        return false;
    }
    const std::optional<bool> child_running =
        process_handle_is_running(child_process.get());
    ok &= check(child_running.has_value() && *child_running,
        "held-resize child remains alive before stop");
    if (!child_running.has_value() || !*child_running) {
        (void)backend->terminate();
        return false;
    }

    std::mutex              watchdog_mutex;
    std::condition_variable watchdog_cv;
    std::atomic_bool        resize_released = false;
    std::atomic_bool        watchdog_fired = false;
    const auto release_resize = [&] {
        if (!resize_released.exchange(true, std::memory_order_acq_rel)) {
            resize_control->allow_resize.release();
        }
        watchdog_cv.notify_all();
    };
    std::thread watchdog([&] {
        std::unique_lock lock(watchdog_mutex);
        if (!watchdog_cv.wait_for(lock, std::chrono::seconds(45), [&] {
                return resize_released.load(std::memory_order_acquire);
            }))
        {
            watchdog_fired.store(true, std::memory_order_release);
            lock.unlock();
            release_resize();
        }
    });

    std::thread resize_thread([&] {
        (void)backend->resize({1U, {25, 81}});
    });
    const bool resize_entered = resize_control->resize_entered.try_acquire_for(k_wait_timeout);
    ok &= check(resize_entered, "held resize reaches the native call seam");

    term::Terminal_backend_result stop_result;
    std::atomic_bool stop_returned = false;
    std::thread stop_thread;
    bool stop_committed_before_release = false;
    bool stop_and_child_exited_before_release = false;
    if (resize_entered) {
        stop_thread = std::thread([&] {
            stop_result = backend->terminate();
            stop_returned.store(true, std::memory_order_release);
        });

        const auto commitment_deadline = std::chrono::steady_clock::now() + k_wait_timeout;
        do {
            stop_committed_before_release = backend->write_state_for_testing().stopping;
            if (stop_committed_before_release) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        while (std::chrono::steady_clock::now() < commitment_deadline &&
            !resize_released.load(std::memory_order_acquire));

        if (stop_committed_before_release) {
            const auto progress_deadline = std::chrono::steady_clock::now() + k_wait_timeout;
            do {
                stop_and_child_exited_before_release =
                    stop_returned.load(std::memory_order_acquire) &&
                    WaitForSingleObject(child_process.get(), 0) == WAIT_OBJECT_0 &&
                    !resize_released.load(std::memory_order_acquire);
                if (stop_and_child_exited_before_release) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            while (std::chrono::steady_clock::now() < progress_deadline &&
                !resize_released.load(std::memory_order_acquire));
        }
    }

    ok &= check(stop_committed_before_release,
        "stop commits while the admitted native resize is held");
    ok &= check(stop_and_child_exited_before_release,
        "zero-grace stop returns and escalates the child before resize release");
    ok &= check(!close_control->close_entered.try_acquire() &&
            close_control->close_count.load(std::memory_order_acquire) == 0U,
        "ConPTY close does not begin while a native resize owns the handle");
    ok &= check(!watchdog_fired.load(std::memory_order_acquire),
        "held-resize watchdog does not need to release the native call");

    release_resize();
    watchdog.join();
    resize_thread.join();
    if (stop_thread.joinable()) {
        stop_thread.join();
    }
    else {
        stop_result = backend->terminate();
    }

    ok &= check(stop_result.code == term::Terminal_backend_result_code::ACCEPTED ||
            stop_result.stop_committed,
        "held-resize stop remains committed after release");
    ok &= check(capture.wait_for_exit(), "held-resize child exit is reported");
    ok &= check(capture.exit_count_snapshot() == 1,
        "held-resize child exit is reported exactly once");
    backend.reset();
    ok &= check(term::Native_backend_cleanup_reservation::wait_until_idle(
            std::chrono::steady_clock::now() + k_wait_timeout),
        "held-resize native cleanup settles after release");
    ok &= check(close_control->close_count.load(std::memory_order_acquire) == 1U &&
            close_control->observer_retirement_count.load(std::memory_order_acquire) == 1U,
        "held-resize ConPTY close and observer retirement happen exactly once");
    return ok;
}

bool test_async_held_resize_stop_coalesces_and_retires(const QString& fixture_path)
{
    bool ok = true;
    Backend_capture capture;
    auto backend = std::make_unique<term::Windows_conpty_backend>();
    auto resize_control = std::make_shared<term::Windows_conpty_resize_control_for_testing>();
    auto close_control = std::make_shared<term::Windows_conpty_close_control_for_testing>();
    ok &= check(backend->set_resize_control_for_testing(resize_control),
        "async held-resize gate is installed before native start");
    ok &= check(backend->set_close_control_for_testing(close_control),
        "async held-resize close observer is installed before native start");

    term::Terminal_launch_config config = launch_config(
        fixture_path,
        {QStringLiteral("--spawn-hold-open-child-pid-no-read")});
    config.termination_policy.graceful_interval = std::chrono::milliseconds(0);
    config.termination_policy.kill_interval = std::chrono::milliseconds(500);
    const term::Terminal_backend_result start_result = backend->start(
        config,
        capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "async held-resize tree fixture starts");
    if (start_result.code != term::Terminal_backend_result_code::ACCEPTED) {
        return false;
    }

    const std::optional<DWORD> descendant_pid = capture.wait_for_pid_output(
        QByteArrayLiteral("hold-open-child-pid-no-read "));
    ok &= check(descendant_pid.has_value(),
        "async held-resize fixture reports its descendant pid");
    Win32_process_handle descendant_process = descendant_pid.has_value()
        ? open_process_handle(*descendant_pid)
        : Win32_process_handle{};
    ok &= check(descendant_process.is_valid(),
        "async held-resize descendant handle opens");
    if (!descendant_process.is_valid()) {
        (void)backend->terminate();
        close_control->allow_close.release();
        close_control->allow_observer.release();
        (void)capture.wait_for_exit();
        backend.reset();
        (void)term::Native_backend_cleanup_reservation::wait_until_idle(
            std::chrono::steady_clock::now() + k_wait_timeout);
        return false;
    }

    std::mutex              watchdog_mutex;
    std::condition_variable watchdog_cv;
    std::atomic_bool        resize_released = false;
    std::atomic_bool        close_released = false;
    std::atomic_bool        observer_released = false;
    std::atomic_bool        watchdog_fired = false;
    std::atomic_bool        all_gates_released = false;
    const auto release_resize = [&] {
        if (!resize_released.exchange(true, std::memory_order_acq_rel)) {
            resize_control->allow_resize.release();
        }
    };
    const auto release_close = [&] {
        if (!close_released.exchange(true, std::memory_order_acq_rel)) {
            close_control->allow_close.release();
        }
    };
    const auto release_observer = [&] {
        if (!observer_released.exchange(true, std::memory_order_acq_rel)) {
            close_control->allow_observer.release();
        }
    };
    const auto release_all_gates = [&] {
        release_resize();
        release_close();
        release_observer();
        all_gates_released.store(true, std::memory_order_release);
        watchdog_cv.notify_all();
    };
    std::thread watchdog([&] {
        std::unique_lock lock(watchdog_mutex);
        if (!watchdog_cv.wait_for(lock, std::chrono::seconds(45), [&] {
                return all_gates_released.load(std::memory_order_acquire);
            }))
        {
            watchdog_fired.store(true, std::memory_order_release);
            lock.unlock();
            release_all_gates();
        }
    });

    const term::Terminal_backend_resize_dispatch first = backend->dispatch_resize(
        {101U, {25, 81}});
    ok &= check(first.result.code == term::Terminal_backend_result_code::ACCEPTED &&
            first.completion_pending,
        "first asynchronous resize is accepted without waiting for native completion");
    const bool first_entered = resize_control->resize_entered.try_acquire_for(k_wait_timeout);
    ok &= check(first_entered,
        "first asynchronous resize reaches the held native call");

    term::Terminal_backend_resize_dispatch second;
    term::Terminal_backend_resize_dispatch third;
    if (first_entered) {
        second = backend->dispatch_resize({102U, {26, 82}});
        third = backend->dispatch_resize({103U, {27, 83}});
        ok &= check(second.result.code == term::Terminal_backend_result_code::ACCEPTED &&
                second.completion_pending &&
                third.result.code == term::Terminal_backend_result_code::ACCEPTED &&
                third.completion_pending,
            "newer asynchronous resize requests are accepted while the first call is held");
        ok &= check(capture.wait_for_resize_completion_count(1U,
                std::chrono::seconds(2)),
            "replacing the queued request promptly retires its receipt");
        const auto replacement_receipts = capture.resize_completions_snapshot();
        ok &= check(replacement_receipts.size() == 1U &&
                replacement_receipts.front().request.transaction_id == 102U &&
                replacement_receipts.front().superseded &&
                replacement_receipts.front().result.code ==
                    term::Terminal_backend_result_code::ACCEPTED,
            "the middle resize receives its exact accepted superseded receipt");
    }

    std::atomic_bool stop_returned = false;
    term::Terminal_backend_result stop_result;
    std::thread stop_thread([&] {
        stop_result = backend->terminate();
        stop_returned.store(true, std::memory_order_release);
    });
    const auto stop_deadline = std::chrono::steady_clock::now() + k_wait_timeout;
    bool tree_exited_before_release = false;
    while (std::chrono::steady_clock::now() < stop_deadline &&
        !resize_released.load(std::memory_order_acquire))
    {
        const bool stopping = backend->write_state_for_testing().stopping;
        const bool descendant_exited =
            WaitForSingleObject(descendant_process.get(), 0U) == WAIT_OBJECT_0;
        tree_exited_before_release =
            stopping && stop_returned.load(std::memory_order_acquire) && descendant_exited;
        if (tree_exited_before_release) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ok &= check(tree_exited_before_release,
        "stop returns and terminates the descendant while the active resize remains held");
    const bool close_entered_before_release = close_control->close_entered.try_acquire();
    ok &= check(!resize_released.load(std::memory_order_acquire) &&
            close_control->close_count.load(std::memory_order_acquire) == 0U &&
            !close_entered_before_release,
        "the native close owner waits until the held call retires");

    if (first_entered) {
        ok &= check(capture.wait_for_resize_completion_count(2U,
                std::chrono::seconds(2)),
            "stop retires the coalesced latest request before releasing the active call");
        const auto stopped_receipts = capture.resize_completions_snapshot();
        const auto latest_stopped = std::find_if(
            stopped_receipts.begin(),
            stopped_receipts.end(),
            [](const auto& completion) {
                return completion.request.transaction_id == 103U;
            });
        ok &= check(latest_stopped != stopped_receipts.end() &&
                latest_stopped->superseded &&
                latest_stopped->result.code == term::Terminal_backend_result_code::ACCEPTED,
            "stop retires the latest queued resize with its exact superseded receipt");
        ok &= check(stopped_receipts.size() == 2U &&
                std::none_of(stopped_receipts.begin(), stopped_receipts.end(), [](const auto& completion) {
                    return completion.request.transaction_id == 101U;
                }),
            "the active resize has no receipt until its native call returns");
    }

    release_resize();
    if (stop_thread.joinable()) {
        stop_thread.join();
    }
    ok &= check(stop_result.code == term::Terminal_backend_result_code::ACCEPTED ||
            stop_result.stop_committed,
        "async held-resize stop remains committed after release");
    if (first_entered) {
        ok &= check(capture.wait_for_resize_completion_count(3U),
            "active resize receipt arrives after its native call retires");
        const auto final_receipts = capture.resize_completions_snapshot();
        const auto active_receipt = std::find_if(
            final_receipts.begin(),
            final_receipts.end(),
            [](const auto& completion) {
                return completion.request.transaction_id == 101U;
            });
        const auto active_receipt_count = std::count_if(
            final_receipts.begin(),
            final_receipts.end(),
            [](const auto& completion) {
                return completion.request.transaction_id == 101U;
            });
        ok &= check(final_receipts.size() == 3U &&
                active_receipt_count == 1 &&
                active_receipt != final_receipts.end() &&
                active_receipt->superseded,
            "the late active-call receipt is superseded exactly once after stop");
    }

    const bool close_entered = close_entered_before_release ||
        close_control->close_entered.try_acquire_for(k_wait_timeout);
    ok &= check(close_entered,
        "ConPTY close is handed off after active native calls retire");
    if (close_entered) {
        ok &= check(close_control->observer_retirement_count.load(std::memory_order_acquire) == 0U,
            "window observer remains owned until ConPTY close completes");
        release_close();
        const bool observer_entered = close_control->observer_entered.try_acquire_for(k_wait_timeout);
        ok &= check(observer_entered,
            "observer retirement follows the single ConPTY close");
        ok &= check(close_control->close_count.load(std::memory_order_acquire) == 1U &&
                close_control->observer_retirement_count.load(std::memory_order_acquire) == 0U,
            "observer ownership remains live until the close returns");
        release_observer();
    }
    else {
        release_close();
        release_observer();
    }
    ok &= check(capture.wait_for_exit(),
        "async held-resize process exit is reported after native close handoff");
    ok &= check(capture.exit_count_snapshot() == 1,
        "async held-resize process exit is reported exactly once");
    backend.reset();
    ok &= check(term::Native_backend_cleanup_reservation::wait_until_idle(
            std::chrono::steady_clock::now() + k_wait_timeout),
        "async held-resize cleanup settles after active-call retirement");
    release_all_gates();
    watchdog.join();
    ok &= check(!watchdog_fired.load(std::memory_order_acquire),
        "async held-resize watchdog does not release a gate");
    ok &= check(close_control->close_count.load(std::memory_order_acquire) == 1U &&
            close_control->observer_retirement_count.load(std::memory_order_acquire) == 1U,
        "async held-resize close and observer retire exactly once");
    return ok;
}

bool test_close_path_stops_descendant_process(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    Win32_process_handle descendant_process;
    auto destroy_started_at = std::chrono::steady_clock::time_point{};
    {
        std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
        term::Terminal_launch_config config = launch_config(
            fixture_path,
            {QStringLiteral("--spawn-hold-open-child-pid-no-read")});
        config.termination_policy.graceful_interval = std::chrono::milliseconds(0);
        config.termination_policy.kill_interval     = std::chrono::milliseconds(500);

        const term::Terminal_backend_result start_result =
            backend->start(config, capture.callbacks());
        ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
            "close-path descendant fixture starts");
        if (start_result.code != term::Terminal_backend_result_code::ACCEPTED) {
            return false;
        }

        const std::optional<DWORD> descendant_pid =
            capture.wait_for_pid_output(QByteArrayLiteral("hold-open-child-pid-no-read "));
        ok &= check(descendant_pid.has_value(),
            "close-path descendant fixture reports descendant pid");
        if (!descendant_pid.has_value()) {
            return false;
        }

        descendant_process = open_process_handle(*descendant_pid);
        ok &= check(descendant_process.is_valid(),
            "close-path descendant process handle opens");
        if (!descendant_process.is_valid()) {
            return false;
        }

        const std::optional<bool> descendant_running =
            process_handle_is_running(descendant_process.get());
        ok &= check(descendant_running.has_value() && *descendant_running,
            "close-path descendant process is alive before terminal teardown");

        const term::Terminal_backend_result terminate_result = backend->terminate();
        ok &= check(terminate_result.code == term::Terminal_backend_result_code::ACCEPTED,
            "close-path backend accepts terminate before destruction");

        destroy_started_at = std::chrono::steady_clock::now();
    }
    const auto destroy_elapsed = std::chrono::steady_clock::now() - destroy_started_at;
    ok &= check(
        destroy_elapsed < std::chrono::milliseconds(2500),
        "ConPTY close path returns promptly while stopping descendants");

    const bool descendant_exited = wait_for_process_exit(descendant_process.get());
    if (!descendant_exited) {
        terminate_process_if_running(descendant_process.get());
    }
    ok &= check(descendant_exited,
        "ConPTY close path stops descendant processes");

    return ok;
}

bool test_missing_working_directory(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    term::Terminal_launch_config config =
        launch_config(fixture_path, {QStringLiteral("--hold-open")});
    config.working_directory = QFileInfo(fixture_path).absolutePath() +
        QStringLiteral("/missing-directory");

    const term::Terminal_backend_result start_result =
        backend->start(config, capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::REJECTED,
        "missing working directory rejects start");
    ok &= check(start_result.error.has_value() &&
        start_result.error->code ==
            term::Terminal_backend_error_code::WORKING_DIRECTORY_UNAVAILABLE,
        "missing working directory reports typed error");
    ok &= check(!capture.errors_snapshot().empty() &&
        capture.errors_snapshot().front().code ==
            term::Terminal_backend_error_code::WORKING_DIRECTORY_UNAVAILABLE,
        "missing working directory emits backend error callback");

    config.working_directory = QFileInfo(fixture_path).absolutePath();
    const term::Terminal_backend_result retry_result =
        backend->start(config, capture.callbacks());
    ok &= check(retry_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY start can retry after preflight working-directory rejection");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("hold-open")),
        "retried working-directory fixture starts");
    ok &= check(backend->terminate().code == term::Terminal_backend_result_code::ACCEPTED,
        "retried working-directory fixture terminates");
    ok &= check(capture.wait_for_exit(), "retried working-directory fixture exits");

    return ok;
}

bool test_absolute_forward_slash_cmd_stays_running()
{
    bool ok = true;

    QString cmd_path = qEnvironmentVariable("ComSpec");
    ok &= check(!cmd_path.isEmpty(), "ComSpec names the Windows command processor");
    ok &= check(QFileInfo::exists(cmd_path), "Windows command processor exists");
    if (cmd_path.isEmpty() || !QFileInfo::exists(cmd_path)) {
        return false;
    }

    cmd_path = QDir::fromNativeSeparators(QFileInfo(cmd_path).absoluteFilePath());
    ok &= check(cmd_path.contains(QLatin1Char('/')),
        "Windows command processor path uses forward slashes");

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    term::Terminal_launch_config config = launch_config(cmd_path, {});
    config.working_directory = QDir::tempPath();

    const term::Terminal_backend_result start_result =
        backend->start(config, capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "absolute forward-slash cmd path starts");
    if (start_result.code != term::Terminal_backend_result_code::ACCEPTED) {
        return false;
    }

    const bool exited_early =
        capture.wait_for_exit_within(std::chrono::milliseconds(1000));
    ok &= check(!exited_early,
        "absolute forward-slash cmd path remains interactive");
    if (exited_early) {
        const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
        if (exit.has_value()) {
            std::cerr << "absolute forward-slash cmd exited early: exit_code="
                << exit->exit_code << '\n';
        }
        return false;
    }

    const term::Terminal_backend_result terminate_result = backend->terminate();
    ok &= check(terminate_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "absolute forward-slash cmd accepts terminate");
    ok &= check(capture.wait_for_exit(),
        "absolute forward-slash cmd exits after terminate");
    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::TERMINATED,
        "absolute forward-slash cmd reports terminated exit");
    ok &= check_no_backend_errors(capture,
        "absolute forward-slash cmd produces no backend errors");

    return ok;
}

bool test_windows_command_line_quoting(const QString& fixture_path)
{
    bool ok = true;

    QTemporaryDir temporary_directory;
    ok &= check(temporary_directory.isValid(),
        "command-line quoting temporary directory is available");
    if (!temporary_directory.isValid()) {
        return false;
    }

    const QString fixture_directory =
        temporary_directory.filePath(QStringLiteral("fixture path with spaces"));
    ok &= check(QDir().mkpath(fixture_directory),
        "command-line quoting fixture directory is available");
    const QString copied_fixture_path = QDir::fromNativeSeparators(
        QDir(fixture_directory).filePath(QFileInfo(fixture_path).fileName()));
    ok &= check(QFile::copy(fixture_path, copied_fixture_path),
        "command-line quoting fixture copy is available");
    if (!QFileInfo::exists(copied_fixture_path)) {
        return false;
    }

    const QStringList forwarded_arguments = {
        QStringLiteral("plain"),
        QStringLiteral("/D"),
        QStringLiteral("C:/payload/path"),
        QStringLiteral("contains spaces"),
        QStringLiteral("embedded\"quote"),
        QStringLiteral("space and trailing slash\\"),
        QStringLiteral(""),
    };
    QStringList command_arguments = {QStringLiteral("--echo-argv")};
    command_arguments.append(forwarded_arguments);

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    const term::Terminal_backend_result start_result = backend->start(
        launch_config(copied_fixture_path, command_arguments),
        capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "forward-slash executable path with spaces starts");
    if (start_result.code != term::Terminal_backend_result_code::ACCEPTED) {
        return false;
    }

    ok &= check(capture.wait_for_exit(),
        "command-line quoting fixture exits");
    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::EXITED &&
        exit->exit_code == 0,
        "command-line quoting fixture reports clean exit");

    const QByteArray output = capture.output_snapshot();
    for (qsizetype index = 0; index < forwarded_arguments.size(); ++index) {
        const QByteArray expected = QStringLiteral("argv[%1]=%2")
            .arg(index + 2)
            .arg(forwarded_arguments.at(index))
            .toUtf8();
        ok &= check(output.contains(expected),
            "Windows command-line argument survives quoting");
    }
    ok &= check_no_backend_errors(capture,
        "command-line quoting fixture produces no backend errors");

    return ok;
}

bool test_native_cmd_script_quoting(const QString& fixture_path, bool keep_open)
{
    QTemporaryDir workspace;
    if (!check(workspace.isValid(), "native cmd workspace is available")) {
        return false;
    }
    const QString cmd_path = qEnvironmentVariable("ComSpec");
    if (!check(!cmd_path.isEmpty() && QFileInfo::exists(cmd_path),
            "ComSpec names an existing command processor"))
    {
        return false;
    }
    const QString fixture_directory = workspace.filePath(QStringLiteral("program path with spaces"));
    if (!check(QDir().mkpath(fixture_directory), "native cmd fixture directory is available")) {
        return false;
    }
    const QString copied_fixture = QDir(fixture_directory).filePath(QFileInfo(fixture_path).fileName());
    if (!check(QFile::copy(fixture_path, copied_fixture), "native cmd fixture copy is available")) {
        return false;
    }

    // The first case preserves embedded quotes and a quoted redirect target.
    // The second starts with a quoted executable, exercising /s framing too.
    const QStringList scripts{
        QStringLiteral(">\"result file.txt\" echo \"alpha beta\""),
        QLatin1Char('"') + QDir::toNativeSeparators(copied_fixture) +
            QStringLiteral("\" --echo-argv \"argument with spaces\" > \"arguments result.txt\""),
    };
    const QStringList filenames{
        QStringLiteral("result file.txt"), QStringLiteral("arguments result.txt"),
    };
    bool ok = true;
    for (qsizetype index = 0; index < scripts.size(); ++index) {
        Backend_capture capture;
        std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
        term::Terminal_launch_config config = launch_config(cmd_path, {});
        config.working_directory = workspace.path();
        config.windows_native_arguments =
            (keep_open ? QStringLiteral("/s /k ") : QStringLiteral("/s /c ")) +
            QLatin1Char('"') + scripts.at(index) + QLatin1Char('"');
        const auto started = backend->start(config, capture.callbacks());
        if (!check(started.code == term::Terminal_backend_result_code::ACCEPTED,
                "native cmd script starts"))
        {
            return false;
        }
        const QString result_path = workspace.filePath(filenames.at(index));
        const QString follow_up_name = QStringLiteral("follow-up %1.txt").arg(index);
        if (keep_open) {
            // Unlike /c, /k must execute the script and keep accepting input.
            if (!check(wait_for_file(result_path), "interactive cmd executes its script")) {
                return false;
            }
            if (!check(!capture.exit_snapshot().has_value(), "native /k remains interactive")) {
                return false;
            }
            const QByteArray follow_up = (
                QStringLiteral(">\"") + follow_up_name +
                QStringLiteral("\" echo continued\rexit 0\r")).toUtf8();
            const auto written = backend->write(follow_up);
            if (!check(written.code == term::Terminal_backend_result_code::ACCEPTED,
                    "native /k accepts a subsequent command"))
            {
                return false;
            }
        }
        if (!check(capture.wait_for_exit(), "native cmd exits")) {
            return false;
        }
        const auto exit = capture.exit_snapshot();
        ok &= check(exit.has_value() && exit->reason == term::Terminal_exit_reason::EXITED &&
            exit->exit_code == 0, "native cmd reports clean exit");
        QFile result(result_path);
        if (!check(result.open(QIODevice::ReadOnly), "quoted redirect names the intended file")) {
            return false;
        }
        const QByteArray contents = result.readAll();
        if (index == 0) {
            ok &= check(contents == QByteArrayLiteral("\"alpha beta\"\r\n"),
                "cmd preserves echo's quotation marks and exact output bytes");
        }
        else {
            ok &= check(contents.contains(QByteArrayLiteral("argv[2]=argument with spaces")),
                "quoted executable and argument survive cmd framing");
        }
        if (keep_open) {
            QFile follow_up(workspace.filePath(follow_up_name));
            if (!check(follow_up.open(QIODevice::ReadOnly),
                    "native /k executes subsequent terminal input"))
            {
                return false;
            }
            ok &= check(follow_up.readAll() == QByteArrayLiteral("continued\r\n"),
                "subsequent command produces the expected file contents");
        }
        ok &= check_no_backend_errors(capture, "native cmd produces no backend errors");
    }
    return ok;
}

bool test_failed_executable(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    term::Terminal_launch_config config = launch_config(
        QFileInfo(fixture_path).absolutePath() + QStringLiteral("/missing-fixture.exe"),
        {});
    config.working_directory = QFileInfo(fixture_path).absolutePath();

    const term::Terminal_backend_result start_result =
        backend->start(config, capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::REJECTED,
        "missing executable rejects start");
    ok &= check(start_result.error.has_value() &&
        start_result.error->code == term::Terminal_backend_error_code::START_FAILED,
        "missing executable reports typed start failure");
    ok &= check(!capture.errors_snapshot().empty() &&
        capture.errors_snapshot().front().code ==
            term::Terminal_backend_error_code::START_FAILED,
        "missing executable emits backend error callback");

    return ok;
}

bool test_rejection_paths(const QString& fixture_path)
{
    bool ok = true;

    {
        Backend_capture capture;
        std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
        const term::Terminal_backend_result callback_result =
            backend->start(launch_config(fixture_path, {QStringLiteral("--hold-open")}), {});
        ok &= check(callback_result.code == term::Terminal_backend_result_code::REJECTED &&
            callback_result.error.has_value() &&
            callback_result.error->code ==
                term::Terminal_backend_error_code::CALLBACK_MISSING,
            "ConPTY start rejects missing callbacks");
    }

    {
        std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
        ok &= check(backend->write(QByteArrayLiteral("x")).code ==
            term::Terminal_backend_result_code::REJECTED,
            "ConPTY write before start rejects");
        ok &= check(backend->resize({1U, {24, 80}}).code ==
            term::Terminal_backend_result_code::REJECTED,
            "ConPTY resize before start rejects");
        ok &= check(backend->interrupt().code == term::Terminal_backend_result_code::REJECTED,
            "ConPTY interrupt before start rejects");
        ok &= check(backend->terminate().code == term::Terminal_backend_result_code::REJECTED,
            "ConPTY terminate before start rejects");
    }

    {
        Backend_capture capture;
        std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
        term::Terminal_launch_config config =
            launch_config(fixture_path, {QStringLiteral("--hold-open")});
        config.initial_grid_size = term::terminal_grid_size_t{40000, 80};
        const term::Terminal_backend_result start_result =
            backend->start(config, capture.callbacks());
        ok &= check(start_result.code == term::Terminal_backend_result_code::REJECTED &&
            start_result.error.has_value() &&
            start_result.error->code ==
                term::Terminal_backend_error_code::INVALID_INITIAL_GRID_SIZE,
            "ConPTY start rejects out-of-range initial grid");
    }

    {
        Backend_capture capture;
        std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
        const term::Terminal_backend_result start_result =
            backend->start(
                launch_config(fixture_path, {QStringLiteral("--hold-open")}),
                capture.callbacks());
        ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
            "rejection-path fixture starts");
        ok &= check(capture.wait_for_output(QByteArrayLiteral("hold-open")),
            "rejection-path fixture reaches ready marker");

        const term::Terminal_backend_result second_start =
            backend->start(
                launch_config(fixture_path, {QStringLiteral("--hold-open")}),
                capture.callbacks());
        ok &= check(second_start.code == term::Terminal_backend_result_code::REJECTED &&
            second_start.error.has_value() &&
            second_start.error->code == term::Terminal_backend_error_code::START_FAILED,
            "ConPTY second start rejects");
        ok &= check(backend->resize({1U, {0, 80}}).code ==
            term::Terminal_backend_result_code::REJECTED,
            "ConPTY invalid resize rejects");
        ok &= check(backend->write(QByteArray(
                static_cast<qsizetype>(
                    term::k_native_backend_max_queued_write_bytes + 1U),
                'x')).code ==
            term::Terminal_backend_result_code::REJECTED,
            "ConPTY oversized write rejects");
        ok &= check(backend->terminate().code == term::Terminal_backend_result_code::ACCEPTED,
            "rejection-path fixture terminates");
        ok &= check(capture.wait_for_exit(), "rejection-path fixture exits");
        ok &= check(backend->write(QByteArrayLiteral("after")).code ==
            term::Terminal_backend_result_code::REJECTED,
            "ConPTY write after exit rejects");
    }

    return ok;
}

bool test_repeated_start_precheck_callback_runs_after_gate_unlock(const QString& fixture_path)
{
    bool ok = true;

    std::mutex start_gate_mutex;
    bool running           = true;
    bool start_attempted   = true;
    bool start_in_progress = false;
    bool callback_ran_after_gate_unlock = false;
    std::optional<term::Terminal_backend_error> callback_error;

    term::Terminal_backend_callbacks callbacks;
    callbacks.output_received = [](QByteArray) {};
    callbacks.process_exited  = [](term::Terminal_backend_exit) {};
    callbacks.error_reported  = [&](term::Terminal_backend_error error) {
        std::thread lock_probe([&] {
            callback_ran_after_gate_unlock = start_gate_mutex.try_lock();
            if (callback_ran_after_gate_unlock) {
                start_gate_mutex.unlock();
            }
        });
        lock_probe.join();
        callback_error = std::move(error);
    };

    const term::Native_backend_start_precheck precheck =
        term::validate_native_backend_start_preconditions(
            launch_config(fixture_path, {QStringLiteral("--hold-open")}),
            callbacks,
            {start_gate_mutex, running, start_attempted, start_in_progress},
            QStringLiteral("ConPTY"));

    ok &= check(callback_ran_after_gate_unlock,
        "ConPTY repeated-start error callback runs after releasing start gate mutex");
    ok &= check(precheck.result.code == term::Terminal_backend_result_code::REJECTED &&
        precheck.result.error.has_value() &&
        precheck.result.error->code == term::Terminal_backend_error_code::START_FAILED,
        "ConPTY repeated-start precheck reports typed rejection");
    ok &= check(callback_error.has_value() &&
        callback_error->code == term::Terminal_backend_error_code::START_FAILED,
        "ConPTY repeated-start precheck emits error callback");
    ok &= check(running && start_attempted && !start_in_progress,
        "ConPTY repeated-start precheck leaves start gate state unchanged");

    return ok;
}

bool test_interrupt(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    const term::Terminal_backend_result start_result =
        backend->start(
            launch_config(fixture_path, {QStringLiteral("--hold-open")}),
            capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "interrupt fixture starts");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("hold-open")),
        "interrupt fixture ready marker reaches output");

    const term::Terminal_backend_result interrupt_result = backend->interrupt();
    ok &= check(interrupt_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend accepts interrupt");
    ok &= check(capture.wait_for_exit(), "interrupt fixture exits");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::INTERRUPTED &&
        exit->exit_code == 130,
        "interrupt reports typed interrupted exit");
    ok &= check_no_backend_errors(capture,
        "interrupt fixture produces no backend errors");
    ok &= check(backend->write(QByteArrayLiteral("after")).code ==
        term::Terminal_backend_result_code::REJECTED,
        "ConPTY write after interrupt exit rejects");

    return ok;
}

bool test_queued_interrupt_after_exit_130_write_stays_natural(const QString& fixture_path)
{
    bool ok = true;

    QTemporaryDir gate_dir;
    ok &= check(gate_dir.isValid(), "exit-130 gate directory is available");
    if (!gate_dir.isValid()) {
        return false;
    }
    const QString gate_path = gate_dir.filePath(QStringLiteral("exit-130.gate"));

    Backend_capture capture;
    std::unique_ptr<term::Windows_conpty_backend> backend =
        std::make_unique<term::Windows_conpty_backend>();
    const term::Terminal_backend_result start_result =
        backend->start(
            launch_config(
                fixture_path,
                {
                    QStringLiteral("--exit-130-after-blocked-input"),
                    gate_path,
                }),
            capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "exit-130 blocked-input fixture starts");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("exit-130-after-input")),
        "exit-130 blocked-input fixture ready marker reaches output");

    constexpr std::size_t ordinary_input_size =
        term::k_native_backend_max_queued_write_bytes - 1U;
    QByteArray ordinary_input(static_cast<qsizetype>(ordinary_input_size), 'x');
    ordinary_input[0] = 'q';
    const term::Terminal_backend_result write_result =
        backend->write(std::move(ordinary_input));
    ok &= check(write_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend accepts ordinary exit-triggering input");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("exit-130-input-read")),
        "exit-130 blocked-input fixture consumes trigger byte and stops reading");
    // The scenario needs the ordinary write to still be blocking, and the
    // pseudoconsole host decides that: under load it can absorb the whole
    // backlog before the test observes it in flight. That is the same premise
    // the exit classification below rests on, so a run that cannot establish it
    // is inconclusive rather than failing - asserting here would be the mistake
    // this fixture exists to avoid, one step earlier. Release the gate on the
    // way out so the fixture is not left waiting on it.
    if (!wait_for_in_flight_write(
            *backend,
            ordinary_input_size,
            "exit-130 blocked-input ordinary write remains in flight",
            false))
    {
        std::cerr
            << "note: the pseudoconsole host drained the ordinary write before it "
               "was observed in flight, so this run does not isolate an "
               "undelivered interrupt and the scenario is not exercised\n";
        (void)write_gate_file(gate_path);
        (void)capture.wait_for_exit();
        return ok;
    }

    const term::Terminal_backend_result interrupt_result = backend->interrupt();
    ok &= check(interrupt_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend accepts interrupt queued behind ordinary input");

    ok &= check(write_gate_file(gate_path),
        "exit-130 blocked-input fixture exit gate is released");
    ok &= check(capture.wait_for_exit(), "exit-130 blocked-input fixture exits");

    // The scenario only means anything while the ordinary write is still
    // blocking, because that is what keeps the interrupt queued and
    // undelivered. The child has stopped reading, but the pseudoconsole host
    // keeps draining the pipe into its own input buffer, so a long enough stall
    // anywhere above would let the writer reach the Ctrl+C entry, at which
    // point the backend has recorded an interrupt and classifying a code-130
    // exit as INTERRUPTED is the correct answer. Ask whether the interrupt is
    // still queued rather than asserting an oracle whose premise the host may
    // have dissolved. Bytes in flight cannot answer that: the counter holds
    // whichever write is current, so it is non-zero again once the interrupt
    // itself is being written, and a snapshot taken before the gate is
    // released would miss a drain that happens after it.
    const term::Windows_conpty_backend_write_state_for_testing settled_state =
        backend->write_state_for_testing();
    if (settled_state.interrupt_left_write_queue) {
        std::cerr
            << "note: the queued interrupt left the write queue before the child "
               "exited, so this run no longer isolates an undelivered interrupt "
               "and its exit classification is not asserted\n";
    }
    else {
        const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
        ok &= check(exit.has_value() &&
            exit->reason == term::Terminal_exit_reason::EXITED &&
            exit->exit_code == 130,
            "queued but undelivered interrupt does not classify ordinary code-130 exit");
    }
    ok &= check_no_backend_errors(capture,
        "exit-130 blocked-input fixture produces no backend errors");

    return ok;
}

bool test_future_queued_interrupt_does_not_keep_cleared_exit_130_observation(
    const QString& fixture_path)
{
    bool ok = true;

    QTemporaryDir gate_dir;
    ok &= check(gate_dir.isValid(), "future-interrupt gate directory is available");
    if (!gate_dir.isValid()) {
        return false;
    }
    const QString gate_path = gate_dir.filePath(QStringLiteral("future-interrupt.gate"));

    Backend_capture capture;
    std::unique_ptr<term::Windows_conpty_backend> backend =
        std::make_unique<term::Windows_conpty_backend>();
    const term::Terminal_backend_result start_result =
        backend->start(
            launch_config(
                fixture_path,
                {
                    QStringLiteral("--exit-130-after-interrupt-input-blocked"),
                    gate_path,
                }),
            capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "future-interrupt fixture starts");
    ok &= check(capture.wait_for_output(QByteArrayLiteral(
            "exit-130-after-interrupt-input")),
        "future-interrupt fixture ready marker reaches output");

    const term::Terminal_backend_result first_interrupt = backend->interrupt();
    ok &= check(first_interrupt.code == term::Terminal_backend_result_code::ACCEPTED,
        "future-interrupt fixture accepts first interrupt");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("exit-130-interrupt-read")),
        "future-interrupt fixture consumes first interrupt byte");

    const term::Terminal_backend_result clearing_write =
        backend->write(QByteArrayLiteral("q"));
    ok &= check(clearing_write.code == term::Terminal_backend_result_code::ACCEPTED,
        "future-interrupt fixture accepts clearing ordinary write");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("exit-130-normal-read")),
        "future-interrupt fixture consumes ordinary byte after interrupt");
    const auto cleared_state = backend->write_state_for_testing();

    constexpr std::size_t blocking_write_size =
        term::k_native_backend_max_queued_write_bytes - 1U;
    const term::Terminal_backend_result blocking_write =
        backend->write(QByteArray(static_cast<qsizetype>(blocking_write_size), 'x'));
    ok &= check(blocking_write.code == term::Terminal_backend_result_code::ACCEPTED,
        "future-interrupt fixture accepts blocking ordinary write");
    ok &= wait_for_in_flight_write(
        *backend,
        blocking_write_size,
        "future-interrupt blocking ordinary write remains in flight");
    const term::Terminal_backend_result future_interrupt = backend->interrupt();
    ok &= check(future_interrupt.code == term::Terminal_backend_result_code::ACCEPTED,
        "future-interrupt fixture accepts queued future interrupt");
    const auto queued_state = backend->write_state_for_testing();
    ok &= check(write_gate_file(gate_path),
        "future-interrupt fixture exit gate is released");
    ok &= check(capture.wait_for_exit(), "future-interrupt fixture exits");
    const auto exited_state = backend->write_state_for_testing();

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    // ConPTY can drain the ordinary write after it was observed in flight.
    // The future interrupt is the fourth write; use writer progress rather
    // than the exit-classification flag to distinguish that lost premise.
    const bool future_interrupt_started =
        exited_state.successful_write_count + exited_state.failed_write_count >= 4U ||
        exited_state.in_flight_write_bytes == 1U;
    if (future_interrupt_started) {
        std::cerr
            << "note: the future interrupt reached the writer, so "
               "this run does not isolate an undelivered interrupt and its "
               "natural-exit classification is not asserted\n";
        ok &= check(exit.has_value() && exit->exit_code == 130 &&
            (exit->reason == term::Terminal_exit_reason::EXITED ||
             exit->reason == term::Terminal_exit_reason::INTERRUPTED),
            "future interrupt that reached the writer preserves code-130 exit");
        ok &= check_no_backend_errors(capture,
            "future-interrupt fixture produces no backend errors");
        return ok;
    }

    const bool natural_exit = exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::EXITED &&
        exit->exit_code == 130;
    if (!natural_exit) {
        const auto report_state = [](const char* phase, const auto& state) {
            std::cerr << "future-interrupt state: phase=" << phase
                      << " queued_bytes=" << state.queued_write_bytes
                      << " queued_count=" << state.queued_write_count
                      << " in_flight=" << state.in_flight_write_bytes
                      << " successful=" << state.successful_write_count
                      << " failed=" << state.failed_write_count
                      << " interrupt_left_queue=" << state.interrupt_left_write_queue
                      << " stopping=" << state.stopping << '\n';
        };
        std::cerr << "future-interrupt exit: present=" << exit.has_value()
                  << " reason=" << (exit ? static_cast<int>(exit->reason) : -1)
                  << " code=" << (exit ? exit->exit_code : -1) << '\n';
        report_state("after-clearing-output", cleared_state);
        report_state("after-future-enqueue", queued_state);
        report_state("after-exit-wait", exited_state);
    }
    ok &= check(natural_exit,
        "future queued interrupt does not keep cleared code-130 observation");
    ok &= check_no_backend_errors(capture,
        "future-interrupt fixture produces no backend errors");

    return ok;
}

bool test_write_after_interrupt_does_not_clear_without_child_output(
    const QString& fixture_path)
{
    bool ok = true;

    QTemporaryDir gate_dir;
    ok &= check(gate_dir.isValid(), "post-interrupt-write gate directory is available");
    if (!gate_dir.isValid()) {
        return false;
    }
    const QString gate_path = gate_dir.filePath(QStringLiteral("post-interrupt-write.gate"));

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    const term::Terminal_backend_result start_result =
        backend->start(
            launch_config(
                fixture_path,
                {
                    QStringLiteral("--exit-130-from-interrupt-after-input"),
                    gate_path,
                }),
            capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "post-interrupt-write fixture starts");
    ok &= check(capture.wait_for_output(QByteArrayLiteral(
            "exit-130-from-interrupt-after-input")),
        "post-interrupt-write fixture ready marker reaches output");

    const term::Terminal_backend_result interrupt_result = backend->interrupt();
    ok &= check(interrupt_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "post-interrupt-write fixture accepts interrupt");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("exit-130-from-interrupt-read")),
        "post-interrupt-write fixture consumes interrupt byte");

    const term::Terminal_backend_result ordinary_write =
        backend->write(QByteArrayLiteral("q"));
    ok &= check(ordinary_write.code == term::Terminal_backend_result_code::ACCEPTED,
        "post-interrupt-write fixture accepts ordinary write after interrupt");
    ok &= check(write_gate_file(gate_path),
        "post-interrupt-write fixture exit gate is released");
    ok &= check(capture.wait_for_exit(), "post-interrupt-write fixture exits");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::INTERRUPTED &&
        exit->exit_code == 130,
        "ordinary write without following child output does not clear interrupt exit");
    ok &= check_no_backend_errors(capture,
        "post-interrupt-write fixture produces no backend errors");

    return ok;
}

bool test_blocked_post_interrupt_write_has_best_effort_exit_classification(
    const QString& fixture_path)
{
    bool ok = true;

    QTemporaryDir gate_dir;
    ok &= check(gate_dir.isValid(), "blocked-post-interrupt gate directory is available");
    if (!gate_dir.isValid()) {
        return false;
    }
    const QString gate_path =
        gate_dir.filePath(QStringLiteral("blocked-post-interrupt.gate"));

    Backend_capture capture;
    std::unique_ptr<term::Windows_conpty_backend> backend =
        std::make_unique<term::Windows_conpty_backend>();
    const term::Terminal_backend_result start_result =
        backend->start(
            launch_config(
                fixture_path,
                {
                    QStringLiteral("--exit-130-from-interrupt-with-blocked-input"),
                    gate_path,
                }),
            capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "blocked-post-interrupt fixture starts");
    ok &= check(capture.wait_for_output(QByteArrayLiteral(
            "exit-130-from-interrupt-blocked-input")),
        "blocked-post-interrupt fixture ready marker reaches output");

    const term::Terminal_backend_result interrupt_result = backend->interrupt();
    ok &= check(interrupt_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "blocked-post-interrupt fixture accepts interrupt");
    ok &= check(capture.wait_for_output(QByteArrayLiteral(
            "exit-130-blocked-interrupt-read")),
        "blocked-post-interrupt fixture consumes interrupt byte and stops reading");

    const term::Windows_conpty_backend_write_state_for_testing write_state_before =
        backend->write_state_for_testing();

    constexpr std::size_t blocking_write_size =
        term::k_native_backend_max_queued_write_bytes;
    const term::Terminal_backend_result blocking_write =
        backend->write(QByteArray(static_cast<qsizetype>(blocking_write_size), 'x'));
    ok &= check(blocking_write.code == term::Terminal_backend_result_code::ACCEPTED,
        "blocked-post-interrupt fixture accepts blocked ordinary write");
    ok &= wait_for_in_flight_write(
        *backend,
        blocking_write_size,
        "blocked-post-interrupt ordinary write remains in flight");
    ok &= check(write_gate_file(gate_path),
        "blocked-post-interrupt fixture exit gate is released");
    ok &= check(capture.wait_for_output(QByteArrayLiteral(
            "exit-130-blocked-final-output")),
        "blocked-post-interrupt fixture emits final output");

    // ConPTY acknowledges writes to its input pipe, not bytes consumed by the
    // child. The fixture never reads this write, but its completion and the
    // final child output can race the process-wait thread marking the session
    // as stopping. That ordering leaves either INTERRUPTED or EXITED as the
    // best-effort reason while the exit code and lifecycle guarantees remain
    // exact.
    ok &= wait_for_write_settled(
        *backend,
        write_state_before,
        "blocked post-interrupt ordinary write settles");
    ok &= check(capture.wait_for_exit(), "blocked-post-interrupt fixture exits");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        (exit->reason == term::Terminal_exit_reason::INTERRUPTED ||
            exit->reason == term::Terminal_exit_reason::EXITED) &&
        exit->exit_code == 130,
        "blocked post-interrupt exit has a best-effort reason and code 130");
    ok &= check_no_backend_errors(capture,
        "blocked-post-interrupt fixture produces no backend errors");

    return ok;
}

bool test_interrupt_without_stdin_reader_remains_terminable(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    const term::Terminal_backend_result start_result =
        backend->start(
            launch_config(fixture_path, {QStringLiteral("--hold-open-no-read")}),
            capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "no-read interrupt fixture starts");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("hold-open-no-read")),
        "no-read interrupt fixture ready marker reaches output");

    const term::Terminal_backend_result interrupt_result = backend->interrupt();
    ok &= check(interrupt_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend accepts no-read interrupt");

    if (!capture.wait_for_exit_within(std::chrono::milliseconds(250))) {
        const term::Terminal_backend_result terminate_result = backend->terminate();
        const bool terminate_accepted =
            terminate_result.code == term::Terminal_backend_result_code::ACCEPTED;
        const bool exit_observed = capture.wait_for_exit();
        ok &= check(terminate_accepted || exit_observed,
            "ConPTY backend accepts terminate after no-read interrupt or observes exit");
        ok &= check(exit_observed, "no-read interrupt fixture terminates");
    }

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    const bool reported_interrupted = exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::INTERRUPTED &&
        exit->exit_code == 130;
    const bool reported_terminated = exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::TERMINATED;
    ok &= check(reported_interrupted || reported_terminated,
        "no-read interrupt either reports processed Ctrl+C or remains terminable");
    ok &= check_no_backend_errors(capture,
        "no-read interrupt fixture produces no backend errors");

    return ok;
}

bool test_interrupt_byte_consumed_child_exits_normally()
{
    bool ok = true;

    QTemporaryDir script_dir;
    ok &= check(script_dir.isValid(), "ignored-interrupt script directory is available");
    if (!script_dir.isValid()) {
        return false;
    }

    const QString script_path =
        script_dir.filePath(QStringLiteral("ignored_interrupt.ps1"));
    ok &= check(write_ignored_interrupt_powershell_script(script_path),
        "ignored-interrupt PowerShell script is writable");
    if (!ok) {
        return false;
    }

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    const term::Terminal_backend_result start_result =
        backend->start(
            powershell_script_launch_config(script_path),
            capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ignored-interrupt PowerShell fixture starts");
    if (start_result.code != term::Terminal_backend_result_code::ACCEPTED) {
        return false;
    }

    ok &= check(capture.wait_for_output(QByteArrayLiteral("ignore-int")),
        "ignored-interrupt PowerShell fixture reaches ready marker");

    const term::Terminal_backend_result interrupt_result = backend->interrupt();
    ok &= check(interrupt_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend accepts consumed interrupt");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("caught-int")),
        "ignored-interrupt PowerShell fixture consumes Ctrl+C byte");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("post-int-ready")),
        "ignored-interrupt PowerShell fixture waits for post-interrupt input");
    ok &= check(backend->write(QByteArrayLiteral("q")).code ==
        term::Terminal_backend_result_code::ACCEPTED,
        "ignored-interrupt PowerShell fixture receives post-interrupt input");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("natural-exit")),
        "ignored-interrupt PowerShell fixture reaches natural exit path");
    ok &= check(capture.wait_for_exit(),
        "ignored-interrupt PowerShell fixture exits");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::EXITED &&
        exit->exit_code == 130,
        "consumed interrupt preserves normal child exit status");
    ok &= check_no_backend_errors(capture,
        "ignored-interrupt PowerShell fixture produces no backend errors");

    return ok;
}

bool test_terminate(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    term::Terminal_launch_config config =
        launch_config(fixture_path, {QStringLiteral("--hold-open")});
    const term::Terminal_backend_result start_result =
        backend->start(config, capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "terminate fixture starts");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("hold-open")),
        "terminate fixture ready marker reaches output");

    const term::Terminal_backend_result terminate_result = backend->terminate();
    ok &= check(terminate_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend accepts terminate");
    ok &= check(capture.wait_for_exit(), "terminate fixture exits");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::TERMINATED,
        "terminate reports typed terminated exit");
    ok &= check_no_backend_errors(capture,
        "terminate fixture produces no backend errors");
    ok &= check(backend->resize({1U, {24, 80}}).code ==
        term::Terminal_backend_result_code::REJECTED,
        "ConPTY resize after terminate exit rejects");

    return ok;
}

bool test_terminate_accepts_zero_grace_policy(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    term::Terminal_launch_config config =
        launch_config(fixture_path, {QStringLiteral("--hold-open-no-read")});
    config.termination_policy.graceful_interval = std::chrono::milliseconds(0);
    config.termination_policy.kill_interval     = std::chrono::milliseconds(500);

    const term::Terminal_backend_result start_result =
        backend->start(config, capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "zero-grace terminate fixture starts");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("hold-open-no-read")),
        "zero-grace terminate fixture ready marker reaches output");

    const term::Terminal_backend_result terminate_result = backend->terminate();
    ok &= check(terminate_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend accepts zero-grace terminate");
    ok &= check(capture.wait_for_exit(), "zero-grace terminate fixture exits");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::TERMINATED,
        "zero-grace terminate reports typed terminated exit");
    ok &= check_no_backend_errors(capture,
        "zero-grace terminate fixture produces no backend errors");

    return ok;
}

bool test_terminate_returns_before_child_exit(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
    term::Terminal_launch_config config =
        launch_config(fixture_path, {QStringLiteral("--hold-open-no-read")});
    config.termination_policy.graceful_interval = std::chrono::milliseconds(1000);
    config.termination_policy.kill_interval     = std::chrono::milliseconds(500);

    const term::Terminal_backend_result start_result =
        backend->start(config, capture.callbacks());
    ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "nonblocking terminate fixture starts");
    ok &= check(capture.wait_for_output(QByteArrayLiteral("hold-open-no-read")),
        "nonblocking terminate fixture ready marker reaches output");

    const auto started_at = std::chrono::steady_clock::now();

    const term::Terminal_backend_result terminate_result = backend->terminate();

    const auto terminate_elapsed =
        std::chrono::steady_clock::now() - started_at;
    ok &= check(terminate_result.code == term::Terminal_backend_result_code::ACCEPTED,
        "ConPTY backend accepts nonblocking terminate");
    ok &= check(
        terminate_elapsed < std::chrono::milliseconds(250),
        "ConPTY terminate returns before waiting for child exit");
    ok &= check(capture.wait_for_exit(),
        "nonblocking terminate fixture exits after worker termination");

    const std::optional<term::Terminal_backend_exit> exit = capture.exit_snapshot();
    ok &= check(exit.has_value() &&
        exit->reason == term::Terminal_exit_reason::TERMINATED,
        "nonblocking terminate reports typed terminated exit");
    ok &= check_no_backend_errors(capture,
        "nonblocking terminate fixture produces no backend errors");
    return ok;
}

class Foreign_compatibility_window
{
public:
    Foreign_compatibility_window()
    {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc   = DefWindowProcW;
        window_class.hInstance     = GetModuleHandleW(nullptr);
        window_class.lpszClassName = L"PseudoConsoleWindow";
        m_class_atom = RegisterClassW(&window_class);
        if (m_class_atom == 0) {
            return;
        }
        m_owner = CreateWindowExW(0, L"STATIC", L"ConPTY test foreign owner",
            WS_OVERLAPPED, 0, 0, 100, 80, nullptr, nullptr, window_class.hInstance, nullptr);
        m_window = CreateWindowExW(WS_EX_TOOLWINDOW, L"PseudoConsoleWindow", L"ConPTY test foreign window",
            WS_OVERLAPPEDWINDOW, 20, 20, 160, 100, m_owner, nullptr, window_class.hInstance, nullptr);
    }

    ~Foreign_compatibility_window()
    {
        if (m_window != nullptr) {
            DestroyWindow(m_window);
        }
        if (m_owner != nullptr) {
            DestroyWindow(m_owner);
        }
        if (m_class_atom != 0) {
            UnregisterClassW(L"PseudoConsoleWindow", GetModuleHandleW(nullptr));
        }
    }

    Foreign_compatibility_window(const Foreign_compatibility_window&)            = delete;
    Foreign_compatibility_window& operator=(const Foreign_compatibility_window&) = delete;

    HWND window() const { return m_window; }

private:
    ATOM m_class_atom = 0;
    HWND m_owner      = nullptr;
    HWND m_window     = nullptr;
};

bool wait_for_window_state(HWND window, const std::function<bool(HWND)>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + k_wait_timeout;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (predicate(window)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    while (std::chrono::steady_clock::now() < deadline);
    return false;
}

bool test_compatibility_window_stays_hidden(const QString& fixture_path, bool force_close)
{
    Foreign_compatibility_window foreign;
    if (!check(IsWindow(foreign.window()), "foreign compatibility-class window is test-owned")) {
        return false;
    }
    Backend_capture capture;
    auto backend = term::make_windows_conpty_backend();
    if (!check(backend->start(
            launch_config(fixture_path, {QStringLiteral("--show-console-window")}),
            capture.callbacks()).code == term::Terminal_backend_result_code::ACCEPTED,
            "compatibility window fixture starts")) {
        return false;
    }
    const QByteArray prefix = QByteArrayLiteral("compat-window ");
    if (!check(capture.wait_for_output_matching_within(
            [&](const QByteArray& output) {
                const auto start = output.indexOf(prefix);
                return start >= 0 && output.indexOf('\n', start) >= 0;
            }, k_wait_timeout), "compatibility fixture reports its HWND and healthy output")) {
        return false;
    }
    const QByteArray output = capture.output_snapshot();
    const auto number_start = output.indexOf(prefix) + prefix.size();
    const auto number_end   = output.indexOf(" pid ", number_start);
    bool parsed = false;
    const auto window_value = output.mid(number_start, number_end - number_start).toULongLong(&parsed);
    const HWND compatibility = reinterpret_cast<HWND>((ULONG_PTR)window_value);
    if (!check(parsed && IsWindow(compatibility), "live child compatibility HWND remains valid")) {
        return false;
    }
    const HWND owner = GetWindow(compatibility, GW_OWNER);
    DWORD owner_pid = 0;
    GetWindowThreadProcessId(owner, &owner_pid);
    bool ok = check(owner != nullptr && owner_pid == GetCurrentProcessId(),
        "compatibility HWND is bound to this backend's private owner before child startup");
    ShowWindow(foreign.window(), SW_SHOWNOACTIVATE);
    const bool hidden = wait_for_window_state(compatibility, [](HWND window) {
        return IsWindow(window) && !IsWindowVisible(window);
    });
    ok &= check(hidden, "child's immediate maximize leaves its compatibility HWND alive but hidden");
    ok &= check(IsWindowVisible(foreign.window()),
        "hiding compatibility HWND does not hide unrelated same-class window");
    if (!hidden) {
        return false;
    }
    for (int index = 1; index <= 3; ++index) {
        ok &= check(backend->write(QByteArrayLiteral("s")).code ==
                term::Terminal_backend_result_code::ACCEPTED,
            "compatibility fixture receives repeated show request");
        ok &= check(capture.wait_for_output(QByteArrayLiteral("compat-show ") + QByteArray::number(index)),
            "compatibility child remains responsive after show request");
        ok &= check(wait_for_window_state(compatibility, [](HWND window) {
                return IsWindow(window) && !IsWindowVisible(window);
            }), "repeated maximize leaves compatibility HWND alive but hidden");
        ok &= check(IsWindowVisible(foreign.window()),
            "repeated show handling preserves foreign window visibility");
    }
    if (!force_close) {
        ok &= check(backend->write(QByteArrayLiteral("q")).code ==
                term::Terminal_backend_result_code::ACCEPTED,
            "compatibility fixture receives normal exit");
        ok &= check(capture.wait_for_exit(), "compatibility fixture exits normally");
        const auto exit = capture.exit_snapshot();
        ok &= check(exit.has_value() && exit->reason == term::Terminal_exit_reason::EXITED &&
                exit->exit_code == 0,
            "compatibility fixture normal close preserves its successful exit");
    }
    std::atomic_bool stop_flood   = false;
    std::atomic_bool flood_active = false;
    std::atomic_uint flood_count  = 0U;
    std::thread flood;
    if (force_close) {
        flood = std::thread([&]() {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            flood_active.store(true, std::memory_order_release);
            while (!stop_flood.load(std::memory_order_acquire) &&
                std::chrono::steady_clock::now() < deadline)
            {
                NotifyWinEvent(EVENT_OBJECT_SHOW, foreign.window(), OBJID_WINDOW, CHILDID_SELF);
                flood_count.fetch_add(1U, std::memory_order_release);
            }
            flood_active.store(false, std::memory_order_release);
        });
        ok &= check(wait_for_window_state(foreign.window(), [&](HWND) {
                return flood_count.load(std::memory_order_acquire) >= 100U;
            }), "foreign SHOW notification traffic is active before backend teardown");
    }
    backend.reset();
    ok &= check(wait_for_window_state(compatibility, [](HWND window) { return !IsWindow(window); }),
        "backend close releases the compatibility HWND");
    ok &= check(owner != nullptr && wait_for_window_state(owner, [](HWND window) { return !IsWindow(window); }),
        "backend close releases its private owner after ConPTY closes");
    if (force_close) {
        const bool stopped_during_flood = flood_active.load(std::memory_order_acquire);
        stop_flood.store(true, std::memory_order_release);
        flood.join();
        ok &= check(stopped_during_flood,
            "observer teardown completes without waiting for notification traffic to stop");
    }
    ShowWindow(foreign.window(), SW_HIDE);
    ShowWindow(foreign.window(), SW_SHOWNOACTIVATE);
    ok &= check(IsWindowVisible(foreign.window()),
        "foreign show remains safe after backend observer teardown");
    ok &= check_no_backend_errors(capture, "compatibility window handling produces no backend errors");
    return ok;
}

bool test_destructor_stops_running_process(const QString& fixture_path)
{
    bool ok = true;

    Backend_capture capture;
    Win32_process_handle child_process;
    auto destroy_started_at = std::chrono::steady_clock::time_point{};
    {
        std::unique_ptr<term::Terminal_backend> backend = term::make_windows_conpty_backend();
        const term::Terminal_backend_result start_result =
            backend->start(
                launch_config(fixture_path, {QStringLiteral("--hold-open-pid-no-read")}),
                capture.callbacks());
        ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
            "destructor fixture starts");
        const std::optional<DWORD> child_pid =
            capture.wait_for_pid_output(QByteArrayLiteral("hold-open-pid-no-read "));
        ok &= check(child_pid.has_value(),
            "destructor fixture reports child pid");
        if (!child_pid.has_value()) {
            return false;
        }

        child_process = open_process_handle(*child_pid);
        ok &= check(child_process.is_valid(),
            "destructor fixture child process handle opens");
        if (!child_process.is_valid()) {
            return false;
        }

        const std::optional<bool> child_running =
            process_handle_is_running(child_process.get());
        ok &= check(child_running.has_value() && *child_running,
            "destructor fixture child process is alive before backend teardown");

        destroy_started_at = std::chrono::steady_clock::now();
    }
    const auto destroy_elapsed = std::chrono::steady_clock::now() - destroy_started_at;
    ok &= check(
        destroy_elapsed < std::chrono::milliseconds(2500),
        "ConPTY backend destructor returns promptly while stopping the child process");

    const bool child_exited = wait_for_process_exit(child_process.get());
    ok &= check(child_exited,
        "ConPTY backend destructor stops the running child process");
    if (!child_exited) {
        return false;
    }

    const std::optional<DWORD> child_exit_code = process_exit_code(child_process.get());
    ok &= check(child_exit_code.has_value() && *child_exit_code == 1U,
        "ConPTY backend destructor force-terminates the running child process");
    ok &= check_no_backend_errors(capture,
        "destructor fixture produces no backend errors");

    return ok;
}

bool test_destroy_from_process_exited_callback_on_worker_thread(const QString& fixture_path)
{
    // Regression: destroying the backend from inside its own process_exited
    // callback runs ~Windows_conpty_backend on the wait thread, because that
    // callback is delivered from wait_loop(). The backend must defer its
    // shutdown+delete onto a fresh thread (must_defer_native_backend_destruction/
    // defer_native_backend_shutdown_and_delete);
    // running shutdown() inline there self-detaches the wait thread and frees the
    // Impl while wait_loop() is still on the stack, so the following
    // terminate_process_tree_after_root_exit() would touch freed memory.
    //
    // The backend is a raw pointer kept out of any owning shared_ptr chain (so no
    // reference cycle can leak it), and delete_started hands the single
    // destruction to either the wait-thread callback or, on any early return, the
    // owner. The sync block is heap-owned so a late worker callback can never
    // reference a destroyed local. The quick-exit fixture is deliberate: it lets
    // process_exited race startup, and after an accepted start the owner only
    // waits for the exit callback unless timeout cleanup has to claim destruction.
    //
    // The loop gives ASan several chances to observe the buggy path. The
    // use-after-free happens after process_exited returns to wait_loop(), while
    // the owner waits only for the in-callback exit_delivered flag, so this is a
    // stress regression rather than a fully synchronized red-path oracle.
    bool ok = true;

    struct Destroy_from_exit_state
    {
        std::mutex                                mutex;
        std::condition_variable                   cv;
        std::thread::id                           owner_thread_id = std::this_thread::get_id();
        term::Terminal_backend*                   backend             = nullptr;
        std::vector<term::Terminal_backend_error> errors;
        std::atomic_bool                          delete_started      = false;
        bool                                      exit_delivered      = false;
        bool                                      destroyed_off_owner = false;
        int                                       exit_count          = 0;
    };

    constexpr int k_iterations = 8;
    for (int iteration = 0; iteration < k_iterations; ++iteration) {
        auto state     = std::make_shared<Destroy_from_exit_state>();
        state->backend = term::make_windows_conpty_backend().release();

        term::Terminal_backend_callbacks callbacks;
        callbacks.output_received = [](QByteArray) {};
        callbacks.error_reported = [state](term::Terminal_backend_error error) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->errors.push_back(std::move(error));
        };
        callbacks.process_exited = [state](term::Terminal_backend_exit) {
            // Runs on the wait thread. Destroy the backend here (on that worker
            // thread) unless the owner already claimed the destruction on a timeout.
            const bool off_owner = std::this_thread::get_id() != state->owner_thread_id;
            if (state->delete_started.exchange(true)) {
                return;
            }
            delete state->backend;

            std::lock_guard<std::mutex> lock(state->mutex);
            state->destroyed_off_owner = off_owner;
            state->exit_delivered      = true;
            ++state->exit_count;
            state->cv.notify_all();
        };

        const term::Terminal_backend_result start_result = state->backend->start(
            launch_config(fixture_path, {QStringLiteral("--quick-exit")}),
            std::move(callbacks));
        ok &= check(start_result.code == term::Terminal_backend_result_code::ACCEPTED,
            "worker-thread-destroy fixture starts");

        if (start_result.code == term::Terminal_backend_result_code::ACCEPTED) {
            std::unique_lock<std::mutex> lock(state->mutex);
            const bool delivered = state->cv.wait_for(lock, k_wait_timeout, [&] {
                return state->exit_delivered;
            });
            ok &= check(delivered && state->exit_delivered,
                "quick-exit fixture delivers process exit while the backend is destroyed in-callback");
            ok &= check(state->destroyed_off_owner,
                "backend is destroyed on a worker thread, not the owner thread");
            ok &= check(state->exit_count == 1,
                "worker-thread-destroy fixture reports exactly one process exit callback");
            ok &= check(state->errors.empty(),
                "worker-thread-destroy fixture produces no backend errors");
        }

        // Deterministic teardown on every path: if the wait-thread callback did
        // not destroy the backend (start rejected, or an exit timeout), do it.
        if (!state->delete_started.exchange(true)) {
            delete state->backend;
        }
    }

    return ok;
}


bool test_start_retry_after_pseudoconsole_rejection(const QString& fixture_path)
{
    bool ok = true;
    Backend_capture capture;
    auto backend = std::make_unique<term::Windows_conpty_backend>();
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto control = std::make_shared<term::Windows_conpty_close_control_for_testing>();
        control->allow_close.release();
        control->allow_observer.release();
        ok &= check(backend->set_close_control_for_testing(control),
            "rejected start permits a new attempt-local close observer");
        auto config = launch_config(fixture_path, {});
        config.windows_native_arguments = QString(32768, QChar(u'x'));
        const auto result = backend->start(config, capture.callbacks());
        ok &= check(result.code == term::Terminal_backend_result_code::REJECTED &&
                result.native_dispatch_occurred,
            "retry fixture reaches CreateProcessW after pseudoconsole birth");
        ok &= check(control->close_entered.try_acquire_for(k_wait_timeout),
            "each rejected attempt hands off its own pseudoconsole");
        ok &= check(control->observer_entered.try_acquire_for(k_wait_timeout) &&
                control->close_count == 1U,
            "each rejected attempt closes exactly once");
    }
    const auto final_control = std::make_shared<term::Windows_conpty_close_control_for_testing>();
    final_control->allow_close.release();
    final_control->allow_observer.release();
    ok &= check(backend->set_close_control_for_testing(final_control),
        "successful retry receives fresh close gates");
    const auto successful = backend->start(
        launch_config(fixture_path, {QStringLiteral("--quick-exit")}), capture.callbacks());
    ok &= check(successful.code != term::Terminal_backend_result_code::REJECTED &&
            capture.wait_for_exit(),
        "the same backend accepts a child after two post-pseudoconsole rejections");
    backend.reset();
    ok &= check(term::Native_backend_cleanup_reservation::wait_until_idle(
            std::chrono::steady_clock::now() + k_wait_timeout),
        "all rejected and successful attempt owners settle");
    ok &= check(final_control->close_count == 1U &&
            final_control->observer_retirement_count == 1U,
        "successful retry closes and retires its observer exactly once");
    return ok;
}

bool test_close_and_observer_retirement_remain_accounted(const QString& fixture_path)
{
    bool ok = true;
    for (const bool fail_before_child : {false, true}) {
        const auto control = std::make_shared<term::Windows_conpty_close_control_for_testing>();
        struct Close_release
        {
            std::shared_ptr<term::Windows_conpty_close_control_for_testing> control;
            bool close_released = false;
            bool observer_released = false;

            ~Close_release()
            {
                release_close();
                release_observer();
            }

            void release_close()
            {
                if (!close_released) {
                    close_released = true;
                    control->allow_close.release();
                }
            }

            void release_observer()
            {
                if (!observer_released) {
                    observer_released = true;
                    control->allow_observer.release();
                }
            }
        } release{control};
        Backend_capture capture;
        auto backend = std::make_unique<term::Windows_conpty_backend>();
        ok &= check(backend->set_close_control_for_testing(control),
            "close/observer gates are installed before native birth");
        auto config = launch_config(fixture_path, {QStringLiteral("--quick-exit")});
        QTemporaryDir working_directory;
        ok &= check(working_directory.isValid(), "close-ownership temporary directory exists");
        if (fail_before_child) {
            // The valid cwd passes the shared precheck; CreateProcess rejects
            // the oversized command line after the pseudoconsole and WinEvent
            // observer already exist.
            config.argv = {fixture_path};
            config.working_directory = working_directory.path();
            config.windows_native_arguments = QString(32768, QChar(u'x'));
        }
        const auto result = backend->start(config, capture.callbacks());
        ok &= check((result.code == term::Terminal_backend_result_code::REJECTED) == fail_before_child,
            "close fixture reaches the intended normal or pre-child failure path");
        if (!fail_before_child) {
            ok &= check(capture.wait_for_exit(), "real child exit precedes native close retirement");
        }
        const bool close_started = control->close_entered.try_acquire_for(k_wait_timeout);
        ok &= check(close_started,
            "native close task owns the real pseudoconsole");
        const auto before = std::chrono::steady_clock::now();
        backend.reset();
        ok &= check(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(250),
            "facade destruction does not await held native close");
        ok &= check(!term::Native_backend_cleanup_reservation::wait_until_idle(
                std::chrono::steady_clock::now() + std::chrono::milliseconds(20)),
            "held pseudoconsole close remains in registry accounting");
        {
            Backend_capture independent_capture;
            term::Windows_conpty_backend independent;
            const auto independent_start = independent.start(
                launch_config(fixture_path, {QStringLiteral("--quick-exit")}), independent_capture.callbacks());
            ok &= check(independent_start.code != term::Terminal_backend_result_code::REJECTED &&
                    independent_capture.wait_for_exit(),
                "another native backend progresses while one close task is held");
        }
        release.release_close();
        ok &= check(control->observer_entered.try_acquire_for(k_wait_timeout),
            "physical close completes before observer retirement");
        ok &= check(control->close_count == 1U && control->observer_retirement_count == 0U,
            "close runs once while the exact observer remains owned");
        ok &= check(!term::Native_backend_cleanup_reservation::wait_until_idle(
                std::chrono::steady_clock::now() + std::chrono::milliseconds(20)),
            "held observer retirement remains in registry accounting");
        release.release_observer();
        ok &= check(term::Native_backend_cleanup_reservation::wait_until_idle(
                std::chrono::steady_clock::now() + k_wait_timeout),
            "registry settles after native close and observer thread retirement");
        ok &= check(control->close_count == 1U && control->observer_retirement_count == 1U,
            "close and observer retirement each finish exactly once");
    }
    return ok;
}

bool test_facade_hands_off_unconfirmed_native_cleanup(const QString& fixture_path)
{
    auto blocked = std::make_shared<std::atomic_bool>(true);
    struct Observation_release {
        std::shared_ptr<std::atomic_bool> gate;
        ~Observation_release() { gate->store(false, std::memory_order_release); }
    } release{blocked};
    Backend_capture first_capture;
    auto first = std::make_unique<term::Windows_conpty_backend>();
    bool ok = check(first->set_cleanup_observation_gate_for_testing(blocked),
        "reserve an observation gate that outlives the public facade");
    ok &= check(first->set_start_fault_for_testing(term::Windows_conpty_start_fault_for_testing::JOB_ASSIGNMENT),
        "arm post-birth unassigned-root failure before native dispatch");
    const auto result = first->start(launch_config(fixture_path, {QStringLiteral("--quick-exit")}),
        first_capture.callbacks());
    ok &= check(result.code == term::Terminal_backend_result_code::REJECTED &&
            result.native_dispatch_occurred && !result.start_outcome_determinate,
        "a born root with unconfirmed cleanup cannot report determinate start failure");
    ok &= check(first->write_state_for_testing().process_handle_retained && first_capture.exit_count_snapshot() == 0,
        "the original native handle is retained before handoff");
    const auto before = std::chrono::steady_clock::now();
    first.reset();
    ok &= check(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(250),
        "destroying the facade does not wait on a negative native observation");
    ok &= check(!term::Native_backend_cleanup_reservation::wait_until_idle(
            std::chrono::steady_clock::now() + std::chrono::milliseconds(20)),
        "unconfirmed native cleanup remains owned after facade destruction");
    {
        Backend_capture second_capture;
        term::Windows_conpty_backend second;
        const auto independent = second.start(launch_config(fixture_path, {QStringLiteral("--quick-exit")}),
            second_capture.callbacks());
        ok &= check(independent.code != term::Terminal_backend_result_code::REJECTED && second_capture.wait_for_exit(),
            "one retained unconfirmed backend does not block another actual ConPTY backend");
    }
    ok &= check(first_capture.exit_count_snapshot() == 0,
        "a deleted facade has not received a fabricated native completion callback");
    blocked->store(false, std::memory_order_release);
    ok &= check(term::Native_backend_cleanup_reservation::wait_until_idle(
            std::chrono::steady_clock::now() + k_wait_timeout),
        "the continuing owner eventually observes native settlement and joins its workers");
    ok &= check(first_capture.exit_count_snapshot() == 0,
        "continuing native cleanup does not call a retired receiver");
    return ok;
}

bool test_post_birth_failure_keeps_native_custody(const QString& fixture_path)
{
    bool ok = true;
    for (const auto fault : {term::Windows_conpty_start_fault_for_testing::JOB_ASSIGNMENT,
             term::Windows_conpty_start_fault_for_testing::THREAD_RESUME_FAILURE})
    {
        Backend_capture capture; // Outlives the backend and every native callback.
        term::Windows_conpty_backend backend;
        struct Observation_release
        {
            term::Windows_conpty_backend& backend;
            ~Observation_release() { backend.set_cleanup_observation_blocked_for_testing(false); }
        } release{backend};
        ok &= check(backend.set_start_fault_for_testing(fault),
            "post-birth failure injection is armed before native dispatch");
        backend.set_cleanup_observation_blocked_for_testing(true);
        const auto result = backend.start(
            launch_config(fixture_path, {QStringLiteral("--quick-exit")}), capture.callbacks());
        ok &= check(result.code == term::Terminal_backend_result_code::REJECTED &&
                result.native_dispatch_occurred && !result.start_outcome_determinate,
            "post-birth rejection does not claim determinate cleanup before observation");
        const auto state = backend.write_state_for_testing();
        ok &= check(state.process_handle_retained && !state.native_cleanup_settled,
            "a rejected start retains the exact process while cleanup is unconfirmed");
        ok &= check(state.process_assigned_to_job ==
                (fault == term::Windows_conpty_start_fault_for_testing::THREAD_RESUME_FAILURE),
            "the empty-Job assignment-failure case remains distinct from an assigned root");
        ok &= check(backend.process_cpu_time().has_value() == state.process_assigned_to_job,
            "CPU counters require successful root admission to the retained Job");
        ok &= check(capture.exit_count_snapshot() == 0,
            "termination requests alone cannot publish a completed native exit");
        backend.set_cleanup_observation_blocked_for_testing(false);
        ok &= check(capture.wait_for_exit(), "exact native settlement eventually reports failed start");
        const auto exit = capture.exit_snapshot();
        ok &= check(exit && exit->reason == term::Terminal_exit_reason::FAILED_TO_START &&
                capture.exit_count_snapshot() == 1,
            "both post-birth failure paths publish exactly one failed-start native exit");
        ok &= check(backend.write_state_for_testing().native_cleanup_settled,
            "settlement is recorded only after the root and assigned Job are observed empty");
    }
    return ok;
}

bool test_failed_start_retires_cpu_job(const QString& fixture_path)
{
    // This runs in its own process so Job admission cannot affect other fixtures.
    Win32_process_handle host_job(CreateJobObjectW(nullptr, nullptr));
    if (!check(host_job.is_valid() && AssignProcessToJobObject(host_job.get(), GetCurrentProcess()),
            "failed-start CPU fixture enters its own host Job"))
    {
        return false;
    }
    const auto burn_deadline = std::chrono::steady_clock::now() + k_wait_timeout;
    auto host_cpu = process_cpu_time_usec(GetCurrentProcess());
    std::atomic_uint work{0};
    while (host_cpu && *host_cpu == 0 && std::chrono::steady_clock::now() < burn_deadline) {
        for (unsigned int index = 0; index < 4096; ++index) {
            work.fetch_add(index, std::memory_order_relaxed);
        }
        host_cpu = process_cpu_time_usec(GetCurrentProcess());
    }
    if (!check(host_cpu && *host_cpu > 0, "the fixture host has independently measured CPU work")) {
        return false;
    }
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION host_accounting{};
    if (!check(QueryInformationJobObject(nullptr, JobObjectBasicAccountingInformation,
            &host_accounting, sizeof(host_accounting), nullptr),
            "a null Job handle resolves to the fixture host Job"))
    {
        return false;
    }

    Backend_capture capture;
    term::Windows_conpty_backend backend;
    struct Observation_release
    {
        term::Windows_conpty_backend& backend;
        ~Observation_release() { backend.set_cleanup_observation_blocked_for_testing(false); }
    } release{backend};
    if (!check(backend.set_start_fault_for_testing(
            term::Windows_conpty_start_fault_for_testing::RESIZE_WORKER_START),
            "worker startup failure is armed before native dispatch"))
    {
        return false;
    }
    backend.set_cleanup_observation_blocked_for_testing(true);
    const auto result = backend.start(
        launch_config(fixture_path, {QStringLiteral("--hold-open")}), capture.callbacks());
    bool ok = check(result.code == term::Terminal_backend_result_code::REJECTED &&
            result.native_dispatch_occurred && !result.start_outcome_determinate,
        "worker startup failure rejects start while deferred cleanup retains the native child");
    const auto admitted = backend.write_state_for_testing();
    ok &= check(admitted.process_handle_retained && admitted.process_assigned_to_job &&
            !admitted.native_cleanup_settled && backend.process_cpu_time().has_value(),
        "failed start keeps CPU accounting tied to its admitted child until native settlement");

    backend.set_cleanup_observation_blocked_for_testing(false);
    const auto deadline = std::chrono::steady_clock::now() + k_wait_timeout;
    auto cleaned = backend.write_state_for_testing();
    while (cleaned.process_handle_retained && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        cleaned = backend.write_state_for_testing();
    }
    ok &= check(cleaned.native_cleanup_settled && !cleaned.process_handle_retained,
        "deferred cleanup releases the exact native handles while the facade remains alive");
    ok &= check(!cleaned.process_assigned_to_job,
        "released Job ownership clears process admission");
    const auto sample = backend.process_cpu_time();
    if (sample) {
        QueryInformationJobObject(host_job.get(), JobObjectBasicAccountingInformation,
            &host_accounting, sizeof(host_accounting), nullptr);
        std::cerr << "CPU after failed-start cleanup: returned_usec=" << sample->cpu_time_usec
            << " host_job_usec="
            << (host_accounting.TotalKernelTime.QuadPart + host_accounting.TotalUserTime.QuadPart) / 10
            << " host_process_usec=" << *host_cpu << '\n';
    }
    ok &= check(!sample,
        "the live failed-start facade never reports its host Job as terminal CPU");
    return ok;
}

bool test_failed_start_cpu_in_isolated_process(const QString& executable_path, const QString& fixture_path)
{
    QProcess child;
    child.start(executable_path, {QStringLiteral("--failed-start-cpu-child"), fixture_path});
    if (!check(child.waitForStarted(k_wait_timeout.count()),
            "failed-start CPU subprocess starts"))
    {
        return false;
    }
    const bool finished = child.waitForFinished(2 * k_wait_timeout.count());
    if (!finished) {
        child.kill();
        child.waitForFinished();
    }
    std::cerr << child.readAllStandardError().constData();
    return check(finished && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
        "failed-start CPU subprocess preserves Job ownership through deferred cleanup");
}


bool test_native_cmd_resize()
{
    auto launch = launch_config(qEnvironmentVariable("ComSpec"),
        {QStringLiteral("/d"), QStringLiteral("/q"), QStringLiteral("/k"),
         QStringLiteral("prompt PROMPT$G")});
    launch.initial_grid_size = {12, 40};
    std::mutex event_mutex;
    std::condition_variable event_changed;
    unsigned event_generation = 0U;
    term::Terminal_session_config config;
    config.trace_output_chunk_limit = 1024U;
    config.backend_event_notifier = [&] {
        std::lock_guard lock(event_mutex);
        ++event_generation;
        event_changed.notify_all();
    };
    term::Terminal_session session(term::make_windows_conpty_backend(), config);
    if (!check(session.start(launch).code == term::Terminal_session_result_code::ACCEPTED,
            "native cmd resize starts"))
    {
        return false;
    }
    const auto output = [&] {
        QByteArray bytes;
        for (const auto& chunk : session.output_chunks()) {
            bytes += chunk;
        }
        return bytes;
    };
    const auto wait_for = [&](auto predicate) {
        const auto deadline = std::chrono::steady_clock::now() + k_wait_timeout;
        for (;;) {
            unsigned observed;
            {
                std::lock_guard lock(event_mutex);
                observed = event_generation;
            }
            session.process_backend_callback_events();
            if (predicate()) {
                return true;
            }
            std::unique_lock lock(event_mutex);
            if (!event_changed.wait_until(lock, deadline, [&] { return event_generation != observed; })) {
                return false;
            }
        }
    };
    const auto cursor_row_text = [&] {
        const auto snapshot = session.latest_render_snapshot();
        QString text(snapshot->grid_size.columns, QChar(u' '));
        for (const auto& cell : snapshot->cells) {
            if (cell.position.row == snapshot->cursor.position.row) {
                text.replace(cell.position.column, 1, cell.text.to_qstring());
            }
        }
        return text.trimmed();
    };
    bool ok = check(wait_for([&] { return output().endsWith("PROMPT>"); }),
        "native cmd publishes the initial prompt");
    ok &= check(session.write_user_bytes(
            "echo 012345678901234567890123456789012345678901234567890123456789\r").code ==
            term::Terminal_session_result_code::ACCEPTED,
        "native cmd accepts a command with wrapped output");
    ok &= check(wait_for([&] { return count_occurrences(output(), "\r\nPROMPT>") == 2U; }),
        "native cmd completes its wrapped output command");
    ok &= check(session.resize(QSizeF(600, 240), {12, 60}).code ==
            term::Terminal_session_result_code::ACCEPTED,
        "native cmd width resize is accepted");
    ok &= check(session.latest_render_snapshot()->cursor.position.row == 5 &&
            cursor_row_text() == QStringLiteral("PROMPT>"),
        "session reflows the native prompt when ConPTY supplies no repaint");
    ok &= check(session.write_user_bytes("hello").code == term::Terminal_session_result_code::ACCEPTED,
        "native cmd accepts input after width resize");
    ok &= check(wait_for([&] { return output().contains("hello"); }),
        "native cmd echoes input after width resize");
    ok &= check(cursor_row_text() == QStringLiteral("PROMPT>hello"),
        "native cursor and echoed text remain beside the prompt after width resize");
    ok &= check(session.resize(QSizeF(300, 240), {12, 30}).code ==
            term::Terminal_session_result_code::ACCEPTED,
        "native cmd width shrink is accepted");
    ok &= check(session.write_user_bytes("narrow").code == term::Terminal_session_result_code::ACCEPTED,
        "native cmd accepts input after width shrink");
    ok &= check(wait_for([&] { return output().contains("narrow"); }),
        "native cmd echoes input after width shrink");
    ok &= check(cursor_row_text() == QStringLiteral("PROMPT>hellonarrow"),
        "native input remains attached to its prompt after width shrink");
    ok &= check(session.resize(QSizeF(300, 80), {4, 30}).code ==
            term::Terminal_session_result_code::ACCEPTED,
        "native cmd height shrink is accepted");
    ok &= check(cursor_row_text() == QStringLiteral("PROMPT>hellonarrow"),
        "height shrink retains the native prompt and current input");
    ok &= check(session.write_user_bytes("world").code == term::Terminal_session_result_code::ACCEPTED,
        "native cmd accepts input after height shrink");
    ok &= check(wait_for([&] { return output().contains("world"); }),
        "native cmd echoes input after height shrink");
    ok &= check(cursor_row_text() == QStringLiteral("PROMPT>hellonarrowworld"),
        "native input remains attached to its prompt after height shrink");
    ok &= check(session.resize(QSizeF(300, 240), {12, 30}).code ==
            term::Terminal_session_result_code::ACCEPTED,
        "native cmd height growth is accepted");
    ok &= check(session.write_user_bytes("done").code == term::Terminal_session_result_code::ACCEPTED,
        "native cmd accepts input after height growth");
    ok &= check(wait_for([&] { return output().contains("done"); }),
        "native cmd echoes input after height growth");
    ok &= check(cursor_row_text() == QStringLiteral("PROMPT>hellonarrowworlddone"),
        "native input remains attached to its prompt after height growth");
    ok &= check(session.terminate().code == term::Terminal_session_result_code::ACCEPTED,
        "native cmd resize session terminates");
    ok &= check(wait_for([&] { return session.exit_status().has_value(); }),
        "native cmd resize session reports termination");
    return ok;
}

// The files a cmd session uses to switch ConPTY's own buffers and to read them
// back. `type` writes DECSET/DECRST 1049 through the console, so ConPTY acts on
// them as it would for vim; the probe reports ConPTY's active buffer, row by
// row, and its cursor, and stays alive until the test has compared it. Given a
// size, it reports only once ConPTY's buffer has that size. It publishes the
// report by renaming a finished file, so a report that exists is complete. The
// scripts are run by relative path: cmd does not search the current directory
// when NoDefaultCurrentDirectoryInExePath is set.
bool write_native_cmd_screen_files(const QTemporaryDir& dir)
{
    const auto write_file = [&](
            const QString&    name,
            const QByteArray& bytes)
        {
            QFile file(dir.filePath(name));
            return
                file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                file.write(bytes) == bytes.size();
        };
    return
        write_file(QStringLiteral("lines.cmd"), QByteArrayLiteral(
            "@for /l %%i in (1,1,6) do @echo line%%i\r\n")) &&
        write_file(QStringLiteral("wrapped.cmd"), QByteArrayLiteral(
            "@cls\r\n"
            "@echo 01234567890123456789012345678901234567890123456789\r\n")) &&
        // Five lines, then the cursor three rows up: cmd's next prompt
        // overwrites the third line and two lines stay below the cursor.
        write_file(QStringLiteral("below.txt"), QByteArrayLiteral(
            "line1\r\nline2\r\nline3\r\nline4\r\nline5\x1b[3A")) &&
        // The same, with spaces written to column 35 on the line the prompt
        // overwrites: the prompt row keeps them after its text.
        write_file(QStringLiteral("padded.txt"), QByteArrayLiteral(
            "line1\r\nline2\r\nline3                              \r\nline4\r\nline5\x1b[3A")) &&
        write_file(QStringLiteral("enter.txt"), QByteArrayLiteral("\x1b[?1049h")) &&
        write_file(QStringLiteral("leave.txt"), QByteArrayLiteral("\x1b[?1049l")) &&
        // A restore right after entering the alternate screen, and a restore
        // after a replacement that saved the alternate screen's cursor.
        write_file(QStringLiteral("enter_restore.txt"), QByteArrayLiteral(
            "\x1b[?1049h\x1b" "8X")) &&
        write_file(QStringLiteral("replace_restore.txt"), QByteArrayLiteral(
            "\x1b[3;5H\x1b[?1049h\x1b[8;1H\x1b" "8Y")) &&
        write_file(QStringLiteral("probe.cmd"), QByteArrayLiteral(
            "@powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File \"%~dp0probe.ps1\" %*\r\n")) &&
        write_file(QStringLiteral("probe.ps1"), QByteArrayLiteral(
            "param([int]$Rows = 0, [int]$Columns = 0)\r\n"
            "$ui = $Host.UI.RawUI\r\n"
            "$deadline = (Get-Date).AddSeconds(20)\r\n"
            "while ($Rows -gt 0 -and (Get-Date) -lt $deadline -and\r\n"
            "       ($ui.BufferSize.Height -ne $Rows -or $ui.BufferSize.Width -ne $Columns)) {\r\n"
            "    Start-Sleep -Milliseconds 10\r\n"
            "}\r\n"
            "$size = $ui.BufferSize\r\n"
            "$area = New-Object Management.Automation.Host.Rectangle 0, 0, ($size.Width - 1), ($size.Height - 1)\r\n"
            "$cells = $ui.GetBufferContents($area)\r\n"
            "$lines = @('size ' + $size.Height + ' ' + $size.Width)\r\n"
            "for ($row = 0; $row -lt $size.Height; $row++) {\r\n"
            "    $text = ''\r\n"
            "    for ($column = 0; $column -lt $size.Width; $column++) {\r\n"
            "        $text += $cells[$row, $column].Character\r\n"
            "    }\r\n"
            "    $lines += $text.TrimEnd()\r\n"
            "}\r\n"
            "$cursor = $ui.CursorPosition\r\n"
            "$lines += ('cursor ' + $cursor.Y + ' ' + $cursor.X)\r\n"
            "$draft = Join-Path $PSScriptRoot 'buffer.tmp'\r\n"
            "[IO.File]::WriteAllLines($draft, [string[]]$lines)\r\n"
            "Move-Item -Force $draft (Join-Path $PSScriptRoot 'buffer.txt')\r\n"
            "while (-not (Test-Path (Join-Path $PSScriptRoot 'release.txt'))) {\r\n"
            "    Start-Sleep -Milliseconds 10\r\n"
            "}\r\n"));
}

// What ConPTY's probe reported: the size of its active buffer, each row's text
// without trailing blanks, and its cursor.
struct Conpty_buffer_report
{
    term::terminal_grid_size_t     grid_size;
    QStringList                    rows;
    term::terminal_grid_position_t cursor;
};

std::optional<Conpty_buffer_report> read_conpty_buffer_report(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }

    QStringList lines = QString::fromUtf8(file.readAll()).split(QStringLiteral("\r\n"));
    while (!lines.isEmpty() && lines.back().isEmpty()) {
        lines.removeLast();
    }
    if (lines.size() < 2) {
        return std::nullopt;
    }

    const QStringList size_fields   = lines.front().split(QChar(u' '));
    const QStringList cursor_fields = lines.back().split(QChar(u' '));
    if (size_fields.size()   != 3 || size_fields[0]   != QStringLiteral("size") ||
        cursor_fields.size() != 3 || cursor_fields[0] != QStringLiteral("cursor"))
    {
        return std::nullopt;
    }

    Conpty_buffer_report report;
    report.grid_size = {size_fields[1].toInt(), size_fields[2].toInt()};
    report.rows      = lines.mid(1, lines.size() - 2);
    report.cursor    = {cursor_fields[1].toInt(), cursor_fields[2].toInt()};
    if (report.rows.size() != report.grid_size.rows) {
        return std::nullopt;
    }
    return report;
}

// A real cmd under ConPTY, driven through the session that a surface owns.
class Native_cmd_screen
{
public:
    bool start(const QTemporaryDir& dir, term::terminal_grid_size_t grid_size, bool plain_cmd = false)
    {
        m_dir = dir.path();
        m_prompt = plain_cmd ? QDir::toNativeSeparators(m_dir).toUtf8() + '>' : QByteArray("PROMPT>");
        const QStringList arguments = plain_cmd
            ? QStringList{QStringLiteral("/d")}
            : QStringList{QStringLiteral("/d"), QStringLiteral("/q"), QStringLiteral("/k"),
                  QStringLiteral("prompt PROMPT$G")};
        auto launch = launch_config(qEnvironmentVariable("ComSpec"), arguments);
        launch.working_directory = m_dir;
        launch.initial_grid_size = grid_size;
        return
            m_session.start(launch).code == term::Terminal_session_result_code::ACCEPTED &&
            wait_for([&] { return output().endsWith(m_prompt); });
    }

    void stop()
    {
        (void)m_session.terminate();
        (void)wait_for([&] { return m_session.exit_status().has_value(); });
    }

    QByteArray output() const
    {
        QByteArray bytes;
        for (const QByteArray& chunk : m_session.output_chunks()) {
            bytes += chunk;
        }
        return bytes;
    }

    // Wakes on backend events, and every 10 ms for conditions such as the
    // probe's report file that no backend event announces.
    template <typename Predicate>
    bool wait_for(Predicate predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + k_wait_timeout;
        for (;;) {
            unsigned observed;
            {
                std::lock_guard lock(m_event_mutex);
                observed = m_event_generation;
            }
            m_session.process_backend_callback_events();
            if (predicate()) {
                return true;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return false;
            }
            std::unique_lock lock(m_event_mutex);
            (void)m_event_changed.wait_until(
                lock,
                std::min(deadline, now + std::chrono::milliseconds(10)),
                [&] { return m_event_generation != observed; });
        }
    }

    // Types a command line and waits for the prompt that follows it.
    bool run(const QByteArray& command)
    {
        const std::size_t prompts = count_occurrences(output(), m_prompt);
        return
            m_session.write_user_bytes(command + "\r").code ==
                term::Terminal_session_result_code::ACCEPTED &&
            wait_for([&] { return count_occurrences(output(), m_prompt) > prompts; });
    }

    // Runs the probe and returns what it read while it waits, with the session
    // drained to the same moment: the probe writes nothing, so everything ConPTY
    // sent before it started has reached the model once the command's title
    // update, which follows the command's echo, has.
    std::optional<Conpty_buffer_report> probe()
    {
        return run_probe({}, std::nullopt);
    }

    // Resizes while the probe runs in place of cmd's prompt, and returns what
    // the probe read once ConPTY's buffer had the new size. The prompt's
    // cooked-read redraw cannot follow the resize, and the probe's command
    // echo precedes it on both sides.
    std::optional<Conpty_buffer_report> probe_resize(term::terminal_grid_size_t grid_size)
    {
        return run_probe(
            QByteArray(" ") + QByteArray::number(grid_size.rows) + ' ' +
                QByteArray::number(grid_size.columns),
            grid_size);
    }

    // The snapshot taken while the last probe waited.
    const std::optional<term::Terminal_render_snapshot>& probe_model() const
    {
        return m_probe_model;
    }

    // ConPTY applies a resize on its own thread, apart from input. Waiting until
    // its active buffer has the new size keeps typed commands after the resize.
    bool resize(term::terminal_grid_size_t grid_size)
    {
        if (!request_resize(grid_size)) {
            return false;
        }

        for (int attempt = 0; attempt < 20; ++attempt) {
            const std::optional<Conpty_buffer_report> report = probe();
            if (!report.has_value()) {
                return false;
            }
            if (report->grid_size.rows    == grid_size.rows &&
                report->grid_size.columns == grid_size.columns)
            {
                return true;
            }
        }
        return false;
    }

    term::Terminal_session& session()
    {
        return m_session;
    }

private:
    bool request_resize(term::terminal_grid_size_t grid_size)
    {
        return
            m_session.resize(
                QSizeF(grid_size.columns * 10.0, grid_size.rows * 20.0),
                grid_size).code == term::Terminal_session_result_code::ACCEPTED &&
            wait_for([&] { return m_session.backend_geometry_in_sync(); });
    }

    std::optional<Conpty_buffer_report> run_probe(
        const QByteArray&                         arguments,
        std::optional<term::terminal_grid_size_t> resize_to)
    {
        const QString report_path  = QDir(m_dir).filePath(QStringLiteral("buffer.txt"));
        const QString release_path = QDir(m_dir).filePath(QStringLiteral("release.txt"));
        QFile::remove(report_path);
        QFile::remove(release_path);
        const qsizetype output_before = output().size();
        if (m_session.write_user_bytes(".\\probe" + arguments + "\r").code !=
            term::Terminal_session_result_code::ACCEPTED)
        {
            return std::nullopt;
        }

        const auto started = [&] { return output().indexOf(" - .\\probe", output_before) >= 0; };
        if (resize_to.has_value() && (!wait_for(started) || !request_resize(*resize_to))) {
            return std::nullopt;
        }
        const bool reported = wait_for([&] { return QFileInfo::exists(report_path) && started(); });
        const std::optional<Conpty_buffer_report> report = reported
            ? read_conpty_buffer_report(report_path)
            : std::nullopt;
        m_probe_model = m_session.latest_render_snapshot();
        const std::size_t prompts = count_occurrences(output(), m_prompt);
        (void)write_gate_file(release_path);
        (void)wait_for([&] { return count_occurrences(output(), m_prompt) > prompts; });
        return report;
    }

    std::mutex                                       m_event_mutex;
    std::condition_variable                          m_event_changed;
    unsigned                                         m_event_generation = 0U;
    term::Terminal_session                           m_session{
        term::make_windows_conpty_backend(),
        [this]
        {
            term::Terminal_session_config config;
            config.trace_output_chunk_limit = 4096U;
            config.backend_event_notifier   = [this]
            {
                std::lock_guard lock(m_event_mutex);
                ++m_event_generation;
                m_event_changed.notify_all();
            };
            return config;
        }()};
    QString                                          m_dir;
    QByteArray                                       m_prompt;
    std::optional<term::Terminal_render_snapshot>    m_probe_model;
};

QStringList snapshot_row_texts(const term::Terminal_render_snapshot& snapshot)
{
    QStringList rows;
    for (int row = 0; row < snapshot.grid_size.rows; ++row) {
        rows.push_back(QString(snapshot.grid_size.columns, QChar(u' ')));
    }
    for (const term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row >= 0 && cell.position.row < snapshot.grid_size.rows) {
            rows[cell.position.row].replace(cell.position.column, 1, cell.text.to_qstring());
        }
    }
    for (QString& row : rows) {
        while (!row.isEmpty() && row.back() == QChar(u' ')) {
            row.chop(1);
        }
    }
    return rows;
}

bool screens_match(
    const term::Terminal_render_snapshot& model,
    const Conpty_buffer_report&           conpty,
    const std::string&                    label)
{
    const QStringList model_rows = snapshot_row_texts(model);
    const bool match =
        model.grid_size.rows         == conpty.grid_size.rows    &&
        model.grid_size.columns      == conpty.grid_size.columns &&
        model_rows                   == conpty.rows              &&
        model.cursor.position.row    == conpty.cursor.row        &&
        model.cursor.position.column == conpty.cursor.column;
    if (!match) {
        std::cerr << label << ": model " << model.grid_size.rows << 'x' << model.grid_size.columns
            << " cursor " << model.cursor.position.row << ',' << model.cursor.position.column
            << " | ConPTY " << conpty.grid_size.rows << 'x' << conpty.grid_size.columns
            << " cursor " << conpty.cursor.row << ',' << conpty.cursor.column << '\n';
        for (int row = 0; row < std::max(model_rows.size(), conpty.rows.size()); ++row) {
            std::cerr << "  " << row << " | "
                << (row < model_rows.size() ? model_rows[row].toStdString() : std::string())
                << " | "
                << (row < conpty.rows.size() ? conpty.rows[row].toStdString() : std::string())
                << '\n';
        }
    }
    return check(match, label);
}

// A fresh CMD has no history to pull above its banner when its initial grid
// grows. Compare ConPTY, the native model and the renderer's presentation so
// an otherwise correct native resize cannot conceal a leading display gap.
bool test_native_cmd_startup_growth()
{
    bool ok = true;
    for (const int rows : {42, 43}) {
        for (const bool hold_probe : {false, true}) {
            const std::string label = "startup growth to " + std::to_string(rows) +
                (hold_probe ? " with probe holding the console" : " with cmd reading input");
            QTemporaryDir dir;
            if (!check(dir.isValid() && write_native_cmd_screen_files(dir), label + ": fixture files")) {
                return false;
            }
            Native_cmd_screen screen;
            if (!check(screen.start(dir, {31, 118}, true), label + ": cmd starts")) {
                screen.stop();
                return false;
            }
            const auto initial = screen.session().latest_render_snapshot_handle();
            const QString banner = snapshot_row_texts(*initial).front();
            ok &= check(!banner.isEmpty() && initial->viewport.scrollback_rows == 0,
                label + ": the banner begins at row zero without history");
            std::optional<Conpty_buffer_report> report;
            if (hold_probe) {
                report = screen.probe_resize({rows, 118});
            }
            else {
                ok &= check(screen.resize({rows, 118}), label + ": resize applied");
                report = screen.probe();
            }
            ok &= check(report.has_value(), label + ": native buffer report available");
            if (report.has_value()) {
                ok &= screens_match(*screen.probe_model(), *report, label + ": model equals ConPTY");
                const auto presented = term::make_presented_snapshot(
                    std::make_shared<const term::Terminal_render_snapshot>(*screen.probe_model()));
                ok &= check(presented != nullptr, label + ": renderer projection available");
                if (presented) {
                    ok &= check(snapshot_row_texts(*presented).front() == banner &&
                            presented->cursor.position == screen.probe_model()->cursor.position,
                        label + ": the displayed banner and cursor retain their native rows");
                }
            }
            screen.stop();
        }
    }
    return ok;
}

struct Native_cmd_alternate_case
{
    std::string                             name;
    // The script that fills the primary screen before the child enters the
    // alternate screen.
    QByteArray                              setup;
    std::vector<term::terminal_grid_size_t> sizes_before_replacement;
    bool                                    replace_alternate = false;
    std::vector<term::terminal_grid_size_t> sizes_after_replacement;
};

// ConPTY leaves its main buffer at its size while the alternate buffer is
// active and resizes it once when it is used again: on return, and before an
// active alternate buffer is replaced (screenInfo.cpp _handleDeferredResize).
// No resize moves the cursor 1049 saved; the return restores it clamped
// (adaptDispatch.cpp CursorRestoreState). After the child returns, the
// session's primary screen has to be ConPTY's:
// ConPTY places typed input with absolute cursor moves, so the echo lands
// beside the prompt only then, and the probe compares every row.
bool test_native_cmd_alternate_resize_case(const Native_cmd_alternate_case& test_case)
{
    const std::string label = "native cmd alternate resize, " + test_case.name;
    QTemporaryDir dir;
    if (!check(dir.isValid() && write_native_cmd_screen_files(dir),
            label + ": files are written"))
    {
        return false;
    }

    Native_cmd_screen screen;
    if (!check(screen.start(dir, {12, 40}), label + ": cmd starts at 12x40")) {
        screen.stop();
        return false;
    }

    bool ok = check(screen.run(test_case.setup), label + ": cmd fills the primary screen");
    int smallest_rows = 12;
    for (const auto* sizes : {&test_case.sizes_before_replacement, &test_case.sizes_after_replacement}) {
        for (const term::terminal_grid_size_t grid_size : *sizes) {
            smallest_rows = std::min(smallest_rows, grid_size.rows);
        }
    }
    if (smallest_rows < 12) {
        const std::optional<term::Terminal_render_snapshot> filled =
            screen.session().latest_render_snapshot();
        const QStringList filled_rows = snapshot_row_texts(*filled);
        int last_written_row = filled->cursor.position.row;
        for (int row = 0; row < filled_rows.size(); ++row) {
            if (!filled_rows[row].isEmpty()) {
                last_written_row = std::max(last_written_row, row);
            }
        }
        ok &= check(last_written_row >= smallest_rows,
            label + ": text or the prompt sits below the shrunk height, so shrinking moves rows");
    }

    // windows.h defines ALTERNATE, so the alternate buffer is named by exclusion.
    ok &= check(screen.run("type enter.txt") &&
            screen.session().latest_render_snapshot()->viewport.active_buffer !=
                term::Terminal_buffer_id::PRIMARY,
        label + ": the child enters the alternate screen");
    for (const term::terminal_grid_size_t grid_size : test_case.sizes_before_replacement) {
        ok &= check(screen.resize(grid_size), label + ": ConPTY applies the alternate-screen resize");
    }
    if (test_case.replace_alternate) {
        const std::size_t entries = count_occurrences(screen.output(), "\x1b[?1049h");
        ok &= check(screen.run("type enter.txt") &&
                count_occurrences(screen.output(), "\x1b[?1049h") > entries,
            label + ": the child replaces the active alternate screen");
        for (const term::terminal_grid_size_t grid_size : test_case.sizes_after_replacement) {
            ok &= check(screen.resize(grid_size), label + ": ConPTY applies the resize after replacement");
        }
    }

    ok &= check(screen.run("type leave.txt") &&
            screen.session().latest_render_snapshot()->viewport.active_buffer ==
                term::Terminal_buffer_id::PRIMARY,
        label + ": the child returns to the primary screen");

    const auto cursor_row_text = [&] {
        const std::optional<term::Terminal_render_snapshot> snapshot =
            screen.session().latest_render_snapshot();
        return snapshot_row_texts(*snapshot)[snapshot->cursor.position.row];
    };
    ok &= check(cursor_row_text() == QStringLiteral("PROMPT>"),
        label + ": the cursor is on the returned prompt");
    ok &= check(screen.session().write_user_bytes("hello").code ==
            term::Terminal_session_result_code::ACCEPTED &&
            screen.wait_for([&] { return screen.output().contains("hello"); }),
        label + ": cmd echoes typed input after return");
    ok &= check(cursor_row_text() == QStringLiteral("PROMPT>hello"),
        label + ": ConPTY's echo lands beside the session's prompt");
    const qsizetype output_before_clear = screen.output().size();
    ok &= check(screen.session().write_user_bytes("\x1b").code ==
            term::Terminal_session_result_code::ACCEPTED &&
            screen.wait_for([&] { return screen.output().size() > output_before_clear; }),
        label + ": cmd clears the typed input");

    const std::optional<Conpty_buffer_report> conpty = screen.probe();
    ok &= check(conpty.has_value() && screen.probe_model().has_value(),
        label + ": the probe reports ConPTY's primary buffer");
    if (conpty.has_value() && screen.probe_model().has_value()) {
        ok &= screens_match(*screen.probe_model(), *conpty,
            label + ": every row and the cursor equal ConPTY's");
    }

    screen.stop();
    return ok;
}

bool test_native_cmd_alternate_resize()
{
    const std::vector<Native_cmd_alternate_case> cases{
        {
            "exit at the original size",
            QByteArrayLiteral(".\\lines"),
            {{6, 40}, {12, 40}},
            false,
            {},
        },
        {
            "exit at a different size",
            QByteArrayLiteral(".\\lines"),
            {{6, 40}, {14, 30}},
            false,
            {},
        },
        {
            "replacement",
            QByteArrayLiteral(".\\lines"),
            {{6, 40}},
            true,
            {{12, 40}},
        },
        // Widening joins the wrapped line above the cursor 1049 saved. ConPTY
        // restores the saved coordinates as they were, so the prompt follows
        // them, not the rows.
        {
            "saved cursor through a joining reflow",
            QByteArrayLiteral(".\\wrapped"),
            {{12, 60}},
            false,
            {},
        },
        // Lines stay below the cursor 1049 saved, and the return shrinks and
        // narrows the primary: ConPTY's reflow keeps the last rows with text,
        // so rows above leave the screen instead of the lines below.
        {
            "text below the cursor, shorter and narrower",
            QByteArrayLiteral("type below.txt"),
            {{5, 30}},
            false,
            {},
        },
        // ConPTY trims written spaces from a line before it packs the line
        // (textBuffer.cpp TextBuffer::Reflow), so a line padded past the new
        // width stays one row.
        {
            "padded line above the cursor, shorter and narrower",
            QByteArrayLiteral("type padded.txt"),
            {{5, 30}},
            false,
            {},
        },
    };

    bool ok = true;
    for (const Native_cmd_alternate_case& test_case : cases) {
        ok &= test_native_cmd_alternate_resize_case(test_case);
    }
    return ok;
}

// ConPTY creates an alternate buffer with the cursor of the buffer it is
// entered from: the primary's, or the replaced alternate buffer's
// (screenInfo.cpp _CreateAltBuffer). Output written before the application
// positions the cursor, here cmd's prompt, has to land on ConPTY's rows.
bool test_native_cmd_alternate_screen_entry()
{
    const std::string label = "native cmd alternate entry";
    QTemporaryDir dir;
    if (!check(dir.isValid() && write_native_cmd_screen_files(dir), label + ": files are written")) {
        return false;
    }

    Native_cmd_screen screen;
    if (!check(screen.start(dir, {12, 40}), label + ": cmd starts at 12x40")) {
        screen.stop();
        return false;
    }

    bool ok = check(screen.run(".\\lines"), label + ": cmd fills the primary screen");
    ok &= check(screen.session().latest_render_snapshot()->cursor.position.row > 1,
        label + ": the primary cursor is below the alternate screen's home row");
    for (const char* step : {"entry", "replacement"}) {
        const std::string step_label = label + ", " + step;
        ok &= check(screen.run("type enter.txt") &&
                screen.session().latest_render_snapshot()->viewport.active_buffer !=
                    term::Terminal_buffer_id::PRIMARY,
            step_label + ": the child is in the alternate screen");
        const std::optional<Conpty_buffer_report> conpty = screen.probe();
        ok &= check(conpty.has_value() && screen.probe_model().has_value(),
            step_label + ": the probe reports ConPTY's alternate buffer");
        if (conpty.has_value() && screen.probe_model().has_value()) {
            ok &= screens_match(*screen.probe_model(), *conpty,
                step_label + ": every row and the cursor equal ConPTY's");
        }
    }

    ok &= check(screen.run("type leave.txt"), label + ": the child returns to the primary screen");
    screen.stop();
    return ok;
}

// ConPTY keeps one saved cursor for the main buffer and one for the alternate
// buffer, apart from the buffers' rows, and an unused one restores to home
// (adaptDispatch.hpp _savedCursorState). Every 1049h first saves the active
// buffer's cursor, so a replacement saves the alternate one
// (adaptDispatch.cpp _SetAlternateScreenBufferMode).
bool test_native_cmd_alternate_saved_cursor()
{
    const std::string label = "native cmd alternate saved cursor";
    QTemporaryDir dir;
    if (!check(dir.isValid() && write_native_cmd_screen_files(dir), label + ": files are written")) {
        return false;
    }

    Native_cmd_screen screen;
    if (!check(screen.start(dir, {12, 40}), label + ": cmd starts at 12x40")) {
        screen.stop();
        return false;
    }

    struct Step
    {
        const char*             name;
        std::vector<QByteArray> commands;
    };
    const std::vector<Step> steps{
        {"restore right after entry",       {"type enter_restore.txt"}},
        {"restore after a replacement",     {"type replace_restore.txt"}},
        {"restore after leaving and entering again",
            {"type leave.txt", "type enter_restore.txt"}},
    };

    bool ok = check(screen.run(".\\lines"), label + ": cmd fills the primary screen");
    for (const Step& step : steps) {
        const std::string step_label = label + ", " + step.name;
        for (const QByteArray& command : step.commands) {
            ok &= check(screen.run(command), step_label + ": cmd runs " + command.toStdString());
        }
        const std::optional<Conpty_buffer_report> conpty = screen.probe();
        ok &= check(conpty.has_value() && screen.probe_model().has_value(),
            step_label + ": the probe reports ConPTY's alternate buffer");
        if (conpty.has_value() && screen.probe_model().has_value()) {
            ok &= screens_match(*screen.probe_model(), *conpty,
                step_label + ": every row and the cursor equal ConPTY's");
        }
    }

    ok &= check(screen.run("type leave.txt"), label + ": the child returns to the primary screen");
    screen.stop();
    return ok;
}

// ConPTY reflows its main buffer on every resize and keeps the last rows that
// hold text or the cursor, up to a screen from the cursor down (textBuffer.cpp
// TextBuffer::Reflow). With lines below the cursor, a shrink drops rows from
// the top. The reflow trims written spaces from a line before packing it, so
// a padded line stays one row. The resize runs while the probe holds the
// console, so cmd's prompt cannot redraw in between.
bool test_native_cmd_primary_resize_with_text_below_the_cursor()
{
    bool ok = true;
    for (const char* setup : {"type below.txt", "type padded.txt"}) {
        const std::string label =
            std::string("native cmd primary resize with text below the cursor, ") + setup;
        QTemporaryDir dir;
        if (!check(dir.isValid() && write_native_cmd_screen_files(dir), label + ": files are written")) {
            ok = false;
            continue;
        }

        Native_cmd_screen screen;
        if (!check(screen.start(dir, {12, 40}), label + ": cmd starts at 12x40")) {
            screen.stop();
            ok = false;
            continue;
        }

        ok &= check(screen.run(setup), label + ": cmd leaves lines below its prompt");
        const std::optional<Conpty_buffer_report> conpty = screen.probe_resize({5, 30});
        ok &= check(conpty.has_value() && screen.probe_model().has_value(),
            label + ": the probe reports ConPTY's buffer after the resize");
        if (conpty.has_value() && screen.probe_model().has_value()) {
            ok &= check(conpty->grid_size.rows == 5 && conpty->grid_size.columns == 30,
                label + ": ConPTY has the shorter and narrower size");
            ok &= screens_match(*screen.probe_model(), *conpty,
                label + ": every row and the cursor equal ConPTY's");
        }

        screen.stop();
    }
    return ok;
}

}

int main(int argc, char** argv)
{
    if (argc == 3 && std::string_view(argv[1]) == "--failed-start-cpu-child") {
        bool ok = test_failed_start_retires_cpu_job(QString::fromLocal8Bit(argv[2]));
        ok &= term::Native_backend_cleanup_reservation::wait_until_idle(
            std::chrono::steady_clock::now() + k_wait_timeout);
        ok &= wait_for_console_host_children_to_exit("failed-start CPU");
        return ok ? 0 : 1;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--failed-start-cpu") {
        return test_failed_start_cpu_in_isolated_process(
            QString::fromLocal8Bit(argv[0]), QString::fromLocal8Bit(argv[2])) ? 0 : 1;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--cpu-accounting-child") {
        return run_cpu_accounting_child(QString::fromLocal8Bit(argv[2]));
    }
    if (argc == 3 && std::string_view(argv[1]) == "--cpu-accounting-root") {
        return run_cpu_accounting_root(QString::fromLocal8Bit(argv[0]), QString::fromLocal8Bit(argv[2]));
    }
    if (argc == 2 && std::string_view(argv[1]) == "--cpu-accounting") {
        bool ok = test_cpu_accounting_retains_exited_descendants(QString::fromLocal8Bit(argv[0]));
        ok &= wait_for_console_host_children_to_exit("CPU accounting");
        return ok ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--process-snapshot") {
        bool ok = test_process_snapshot_observes_native_job(QString::fromLocal8Bit(argv[0]));
        ok &= check(term::Native_backend_cleanup_reservation::wait_until_idle(
            std::chrono::steady_clock::now() + k_wait_timeout), "process snapshot cleanup settles");
        ok &= wait_for_console_host_children_to_exit("process snapshot");
        return ok ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--native-cmd-startup-growth") {
        bool ok = test_native_cmd_startup_growth();
        ok &= wait_for_console_host_children_to_exit("native cmd startup growth");
        return ok ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--native-cmd-alternate-resize") {
        bool ok = test_native_cmd_alternate_resize();
        ok &= wait_for_console_host_children_to_exit("native cmd alternate resize");
        return ok ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--native-cmd-resize-text-below") {
        bool ok = test_native_cmd_primary_resize_with_text_below_the_cursor();
        ok &= wait_for_console_host_children_to_exit("native cmd resize with text below");
        return ok ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--native-cmd-alternate-entry") {
        bool ok = test_native_cmd_alternate_screen_entry();
        ok &= test_native_cmd_alternate_saved_cursor();
        ok &= wait_for_console_host_children_to_exit("native cmd alternate entry");
        return ok ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--native-cmd-resize") {
        bool ok = test_native_cmd_resize();
        ok &= wait_for_console_host_children_to_exit("native cmd resize");
        return ok ? 0 : 1;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--native-session") {
        bool ok = vnm_terminal::test_helpers::check_native_session_lifecycle(
            term::make_windows_conpty_backend, launch_config(QString::fromLocal8Bit(argv[2]), {}));
        ok &= wait_for_console_host_children_to_exit("native session");
        return ok ? 0 : 1;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--callback-lifetime") {
        const QString fixture_path = QString::fromLocal8Bit(argv[2]);
        bool ok = vnm_terminal::test_helpers::check_callback_lifetime(
            term::make_windows_conpty_backend(),
            launch_config(fixture_path, {QStringLiteral("--hold-open")}), true);
        ok &= vnm_terminal::test_helpers::check_callback_lifetime(
            term::make_windows_conpty_backend(),
            launch_config(fixture_path, {QStringLiteral("--hold-open")}), false);
        ok &= test_destroy_from_process_exited_callback_on_worker_thread(fixture_path);
        ok &= wait_for_console_host_children_to_exit("callback lifetime");
        return ok ? 0 : 1;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--close-ownership") {
        const QString fixture_path = QString::fromLocal8Bit(argv[2]);
        bool ok = test_start_retry_after_pseudoconsole_rejection(fixture_path);
        ok &= test_close_and_observer_retirement_remain_accounted(fixture_path);
        ok &= wait_for_console_host_children_to_exit("close ownership");
        return ok ? 0 : 1;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--held-resize-stop") {
        const QString fixture_path = QString::fromLocal8Bit(argv[2]);
        bool ok = test_held_resize_allows_stop_escalation(fixture_path);
        ok &= wait_for_console_host_children_to_exit("held resize stop");
        return ok ? 0 : 1;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--async-held-resize-stop") {
        const QString fixture_path = QString::fromLocal8Bit(argv[2]);
        bool ok = test_async_held_resize_stop_coalesces_and_retires(fixture_path);
        ok &= wait_for_console_host_children_to_exit("async held resize stop");
        return ok ? 0 : 1;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--compatibility-window") {
        const QString fixture_path = QString::fromLocal8Bit(argv[2]);
        bool ok = test_compatibility_window_stays_hidden(fixture_path, false);
        ok &= test_compatibility_window_stays_hidden(fixture_path, true);
        ok &= wait_for_console_host_children_to_exit("compatibility window");
        return ok ? 0 : 1;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--sixel-cursor-sync") {
        const QString fixture_path = QString::fromLocal8Bit(argv[2]);
        bool ok = test_sixel_cursor_stays_in_sync_with_openconsole(fixture_path);
        ok &= wait_for_console_host_children_to_exit("sixel cursor sync");
        return ok ? 0 : 1;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--native-cmd-script") {
        const QString fixture_path = QString::fromLocal8Bit(argv[2]);
        bool ok = test_native_cmd_script_quoting(fixture_path, false);
        ok &= test_native_cmd_script_quoting(fixture_path, true);
        ok &= test_windows_command_line_quoting(fixture_path);
        ok &= wait_for_console_host_children_to_exit("native cmd script");
        return ok ? 0 : 1;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--paste-input-reader") {
        return run_paste_input_reader(QString::fromLocal8Bit(argv[2]));
    }
    if (argc == 3 && std::string_view(argv[1]) == "--escape-input-reader") {
        return run_escape_input_reader(QString::fromLocal8Bit(argv[2]));
    }
    if (argc == 3 && std::string_view(argv[1]) == "--escape-input-reader-hold-after-ack") {
        return run_escape_input_reader(QString::fromLocal8Bit(argv[2]), true);
    }
    if (argc == 3 && std::string_view(argv[1]) == "--escape-vt-input-reader") {
        return run_escape_vt_input_reader(QString::fromLocal8Bit(argv[2]));
    }
    if (argc == 3 && std::string_view(argv[1]) == "--escape-vt-utf16-input-reader") {
        return run_escape_vt_input_reader(QString::fromLocal8Bit(argv[2]), true);
    }
    if (argc == 2 && std::string_view(argv[1]) == "--escape-input-transport") {
        bool ok = test_escape_transport_after_native_shift_return(
            QString::fromLocal8Bit(argv[0]));
        ok &= wait_for_console_host_children_to_exit("Escape transport");
        return ok ? 0 : 1;
    }
    if (argc != 3) {
        std::cerr << "usage: windows_conpty_backend_tests <fixture-executable> <error-mode-reporter>\n";
        return 2;
    }

    const QString fixture_path  = QString::fromLocal8Bit(argv[1]);
    const QString reporter_path = QString::fromLocal8Bit(argv[2]);

    bool ok = true;
    const auto run_test = [&ok](std::string_view test_name, bool test_result) {
        ok &= test_result;
        ok &= wait_for_console_host_children_to_exit(test_name);
    };

    run_test("facade hands off unconfirmed native cleanup",
        test_facade_hands_off_unconfirmed_native_cleanup(fixture_path));
    run_test("start retry after pseudoconsole rejection",
        test_start_retry_after_pseudoconsole_rejection(fixture_path));
    run_test("close and observer retirement remain accounted",
        test_close_and_observer_retirement_remain_accounted(fixture_path));
    run_test("native Terminal_session lifecycle",
        vnm_terminal::test_helpers::check_native_session_lifecycle(
            term::make_windows_conpty_backend, launch_config(fixture_path, {})));
    run_test("post-birth failure retains native custody",
        test_post_birth_failure_keeps_native_custody(fixture_path));
    run_test("failed-start cleanup retires CPU Job ownership",
        test_failed_start_cpu_in_isolated_process(QString::fromLocal8Bit(argv[0]), fixture_path));
    run_test("pid parser", test_pid_parser_requires_line_delimiter());
    run_test("progressing output exit wait has absolute bound",
        test_progressing_output_exit_wait_has_absolute_bound());
    run_test("interactive canvas fixture", test_interactive_canvas_fixture(fixture_path));
    run_test("cell pixel size query round-trips through ConPTY",
        test_cell_pixel_size_query_round_trips_through_conpty(fixture_path));
    run_test("compatibility window normal close", test_compatibility_window_stays_hidden(fixture_path, false));
    run_test("compatibility window forced close", test_compatibility_window_stays_hidden(fixture_path, true));
    run_test("Unicode paste preserves console input",
        test_unicode_paste_preserves_console_input(QString::fromLocal8Bit(argv[0])));
    // Escape transport has a dedicated CTest entry; keep it out of this
    // aggregate suite so it is not run twice, including in the ASan target.
    run_test("terminal child starts with default error mode",
        test_terminal_child_starts_with_default_error_mode(reporter_path));
    run_test("CPU accounting retains exited descendants",
        test_cpu_accounting_retains_exited_descendants(QString::fromLocal8Bit(argv[0])));
    run_test("resize storm reports final shell size",
        test_resize_storm_reports_final_shell_size(fixture_path));
    run_test("resize interleaved with shell output",
        test_resize_interleaved_with_shell_output(fixture_path));
    run_test("scroll region scrollback survives conpty",
        test_scroll_region_scrollback_survives_conpty(fixture_path));
    run_test("utf8 payload preserves exact conpty bytes",
        test_utf8_payload_preserves_exact_conpty_bytes(fixture_path));
    run_test("sync raw resize gate preserves order",
        test_sync_raw_resize_gate_preserves_order(fixture_path));
    run_test("fast start exit loop", test_fast_start_exit_loop(fixture_path));
    run_test("tight paused output limits chunk conpty delivery",
        test_paused_release_with_tight_delivery_limits_chunks_conpty_output());
    run_test("exit drain ignores re-pause and delivers buffered output",
        test_exit_drain_ignores_repause_and_delivers_buffered_output(fixture_path));
    run_test("resize races natural exit close",
        test_resize_races_natural_exit_close(fixture_path));
    run_test("start resize terminate ordering",
        test_start_resize_terminate_ordering(fixture_path));
    run_test("terminate after resize storm stops child process",
        test_terminate_after_resize_storm_stops_child_process(fixture_path));
    run_test("close path stops descendant process",
        test_close_path_stops_descendant_process(fixture_path));
    run_test("absolute forward-slash cmd stays running",
        test_absolute_forward_slash_cmd_stays_running());
    run_test("Windows command-line quoting",
        test_windows_command_line_quoting(fixture_path));
    run_test("native cmd /c script quoting", test_native_cmd_script_quoting(fixture_path, false));
    run_test("native cmd /k script quoting", test_native_cmd_script_quoting(fixture_path, true));
    run_test("missing working directory", test_missing_working_directory(fixture_path));
    run_test("failed executable", test_failed_executable(fixture_path));
    run_test("rejection paths", test_rejection_paths(fixture_path));
    run_test("repeated start precheck releases gate before callback",
        test_repeated_start_precheck_callback_runs_after_gate_unlock(fixture_path));
    run_test("interrupt", test_interrupt(fixture_path));
    run_test(
        "queued interrupt after exit-130 write stays natural",
        test_queued_interrupt_after_exit_130_write_stays_natural(fixture_path));
    run_test(
        "future queued interrupt does not keep cleared exit-130 observation",
        test_future_queued_interrupt_does_not_keep_cleared_exit_130_observation(fixture_path));
    run_test(
        "write after interrupt does not clear without child output",
        test_write_after_interrupt_does_not_clear_without_child_output(fixture_path));
    run_test(
        "blocked post-interrupt write has best-effort exit classification",
        test_blocked_post_interrupt_write_has_best_effort_exit_classification(fixture_path));
    run_test("interrupt without stdin reader remains terminable",
        test_interrupt_without_stdin_reader_remains_terminable(fixture_path));
    run_test("interrupt byte consumed child exits normally",
        test_interrupt_byte_consumed_child_exits_normally());
    run_test("terminate", test_terminate(fixture_path));
    run_test("terminate accepts zero grace policy",
        test_terminate_accepts_zero_grace_policy(fixture_path));
    run_test("terminate returns before child exit",
        test_terminate_returns_before_child_exit(fixture_path));
    run_test("destructor stops running process", test_destructor_stops_running_process(fixture_path));
    run_test("destroy from process exited callback on worker thread",
        test_destroy_from_process_exited_callback_on_worker_thread(fixture_path));
    return ok ? 0 : 1;
}
