#include "vnm_terminal/internal/windows_conpty_backend.h"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "native_backend_io_core.h"
#include <windows.h>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

#if defined(__MINGW32__)
DECLARE_HANDLE(HPCON);
#endif

#include <QProcessEnvironment>
#include <QStringList>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <cstddef>

namespace vnm_terminal::internal {

namespace {

constexpr std::chrono::milliseconds k_conpty_reader_close_grace(1000);
// Keep the ConPTY pipe draining while session output is paused, but keep the
// backend-side burst below the session callback/output queue hard limit when it
// is released as one callback.
constexpr qsizetype k_paused_output_high_watermark_bytes = 64 * 1024;

DWORD wait_timeout_from_interval(std::chrono::milliseconds interval)
{
    if (interval <= std::chrono::milliseconds(0)) {
        return 0U;
    }

    constexpr DWORD k_max_finite_wait = INFINITE - 1U;
    const auto      max_interval      = std::chrono::milliseconds(k_max_finite_wait);
    if (interval > max_interval) {
        return k_max_finite_wait;
    }

    return static_cast<DWORD>(interval.count());
}

bool process_exited_within(HANDLE process, std::chrono::milliseconds interval)
{
    return WaitForSingleObject(process, wait_timeout_from_interval(interval)) == WAIT_OBJECT_0;
}

class Unique_handle
{
public:
    Unique_handle() = default;

    explicit Unique_handle(HANDLE value)
    :
        m_handle(value)
    {}

    ~Unique_handle()
    {
        reset();
    }

    Unique_handle(const Unique_handle&)            = delete;
    Unique_handle& operator=(const Unique_handle&) = delete;

    Unique_handle(Unique_handle&& other) noexcept
    :
        m_handle(std::exchange(other.m_handle, nullptr))
    {}

    Unique_handle& operator=(Unique_handle&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_handle = std::exchange(other.m_handle, nullptr);
        }

        return *this;
    }

    HANDLE get() const
    {
        return m_handle;
    }

    void reset(HANDLE value = nullptr)
    {
        if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
        }

        m_handle = value;
    }

    explicit operator bool() const
    {
        return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE m_handle = nullptr;
};

Unique_handle duplicate_handle(HANDLE source)
{
    if (source == nullptr || source == INVALID_HANDLE_VALUE) {
        return {};
    }

    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(
            GetCurrentProcess(),
            source,
            GetCurrentProcess(),
            &duplicate,
            0,
            FALSE,
            DUPLICATE_SAME_ACCESS))
    {
        return {};
    }

    return Unique_handle(duplicate);
}

Unique_handle duplicate_current_thread_handle()
{
    return duplicate_handle(GetCurrentThread());
}

struct Job_object_create_result
{
    Unique_handle handle;
    QString       failed_operation;
    DWORD         error_code = ERROR_SUCCESS;
};

Job_object_create_result create_process_tree_job()
{
    Unique_handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        return {{}, QStringLiteral("CreateJobObjectW"), GetLastError()};
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            job.get(),
            JobObjectExtendedLimitInformation,
            &limits,
            sizeof(limits)))
    {
        return {{}, QStringLiteral("SetInformationJobObject"), GetLastError()};
    }

    return {std::move(job), {}, ERROR_SUCCESS};
}

struct Queued_write
{
    QByteArray bytes;
    bool       marks_interrupted_exit = false;
};

struct Conpty_api
{
    using CreatePseudoConsole_fn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
    using ResizePseudoConsole_fn = HRESULT(WINAPI*)(HPCON, COORD);
    using ClosePseudoConsole_fn  = void(WINAPI*)(HPCON);

    CreatePseudoConsole_fn create = nullptr;
    ResizePseudoConsole_fn resize = nullptr;
    ClosePseudoConsole_fn  close  = nullptr;
};

QString windows_error_message(QStringView context, DWORD code)
{
    wchar_t* buffer = nullptr;
    const DWORD flags =
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM     |
        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(
        flags,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    QString message;
    if (length > 0 && buffer != nullptr) {
        message = QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed();
        LocalFree(buffer);
    }
    else {
        message = QStringLiteral("Windows error %1").arg(code);
    }

    return QStringLiteral("%1: %2").arg(context, message);
}

QString hresult_message(QStringView context, HRESULT result)
{
    return QStringLiteral("%1: HRESULT 0x%2")
        .arg(context)
        .arg(static_cast<qulonglong>(static_cast<unsigned long>(result)), 8, 16, QLatin1Char('0'));
}

std::optional<Conpty_api> load_conpty_api()
{
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32 == nullptr) {
        return std::nullopt;
    }

    Conpty_api api;
    api.create = reinterpret_cast<Conpty_api::CreatePseudoConsole_fn>(
        GetProcAddress(kernel32, "CreatePseudoConsole"));
    api.resize = reinterpret_cast<Conpty_api::ResizePseudoConsole_fn>(
        GetProcAddress(kernel32, "ResizePseudoConsole"));
    api.close  = reinterpret_cast<Conpty_api::ClosePseudoConsole_fn>(
        GetProcAddress(kernel32, "ClosePseudoConsole"));

    if (api.create == nullptr || api.resize == nullptr || api.close == nullptr) {
        return std::nullopt;
    }

    return api;
}

void close_pseudoconsole_detached(
    Conpty_api::ClosePseudoConsole_fn  close_conpty,
    HPCON                             conpty)
{
    if (close_conpty == nullptr || conpty == nullptr) {
        return;
    }

    try {
        std::thread([close_conpty, conpty] {
            close_conpty(conpty);
        }).detach();
    }
    catch (const std::system_error&) {
        // Thread creation failure is rarer than ClosePseudoConsole hanging; keep
        // ownership deterministic when the fallback path is the only option.
        close_conpty(conpty);
    }
}

bool size_fits_conpty(terminal_grid_size_t grid_size)
{
    return
        is_valid_grid_size(grid_size)                       &&
        grid_size.rows <= std::numeric_limits<SHORT>::max() &&
        grid_size.columns <= std::numeric_limits<SHORT>::max();
}

COORD coord_from_grid_size(terminal_grid_size_t grid_size)
{
    return {
        static_cast<SHORT>(grid_size.columns),
        static_cast<SHORT>(grid_size.rows),
    };
}

std::wstring wide_from_qstring(const QString& value)
{
    return value.toStdWString();
}

std::wstring quote_windows_argument(const std::wstring& argument)
{
    if (argument.empty()) {
        return L"\"\"";
    }

    const bool needs_quotes =
        argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needs_quotes) {
        return argument;
    }

    std::wstring quoted;
    quoted.push_back(L'"');

    std::size_t backslashes = 0U;
    for (wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }

        if (ch == L'"') {
            quoted.append(backslashes * 2U + 1U, L'\\');
            quoted.push_back(ch);
            backslashes = 0U;
            continue;
        }

        quoted.append(backslashes, L'\\');
        backslashes = 0U;
        quoted.push_back(ch);
    }

    quoted.append(backslashes * 2U, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring command_line_from_argv(const QStringList& argv)
{
    std::wstring command_line;
    for (const QString& argument : argv) {
        if (!command_line.empty()) {
            command_line.push_back(L' ');
        }

        command_line += quote_windows_argument(wide_from_qstring(argument));
    }

    return command_line;
}

std::wstring environment_block_from_process_environment(
    const QProcessEnvironment& environment)
{
    QStringList entries = environment.toStringList();
    entries.sort(Qt::CaseInsensitive);

    std::wstring block;
    for (const QString& entry : entries) {
        if (entry.contains(QChar(u'\0'))) {
            continue;
        }

        block += wide_from_qstring(entry);
        block.push_back(L'\0');
    }

    if (block.empty()) {
        block.push_back(L'\0');
    }

    block.push_back(L'\0');
    return block;
}

std::vector<std::byte> initialized_attribute_list_storage(
    HPCON                          conpty,
    LPPROC_THREAD_ATTRIBUTE_LIST&  attribute_list,
    Terminal_backend_result&       failure)
{
    SIZE_T attribute_list_size = 0U;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_list_size);

    std::vector<std::byte> storage(attribute_list_size);
    attribute_list =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attribute_list, 1, 0, &attribute_list_size)) {
        failure = backend_reject(
            Terminal_backend_error_code::START_FAILED,
            windows_error_message(QStringLiteral("InitializeProcThreadAttributeList"), GetLastError()));
        return {};
    }

    if (!UpdateProcThreadAttribute(
            attribute_list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, conpty, sizeof(conpty), nullptr, nullptr))
    {
        DeleteProcThreadAttributeList(attribute_list);
        attribute_list = nullptr;
        failure = backend_reject(
            Terminal_backend_error_code::START_FAILED,
            windows_error_message(QStringLiteral("UpdateProcThreadAttribute"), GetLastError()));
        return {};
    }

    return storage;
}

}

class Windows_conpty_backend::Impl
{
public:
    class Public_call_guard
    {
    public:
        explicit Public_call_guard(Impl& impl)
        :
            m_impl(&impl)
        {
            m_impl->enter_public_call();
        }

        Public_call_guard(const Public_call_guard&) = delete;
        Public_call_guard& operator=(const Public_call_guard&) = delete;

        ~Public_call_guard()
        {
            if (m_impl != nullptr) {
                m_impl->leave_public_call();
            }
        }

    private:
        Impl* m_impl = nullptr;
    };

    ~Impl()
    {
        shutdown();
    }

    Public_call_guard public_call_guard()
    {
        return Public_call_guard(*this);
    }

    Terminal_backend_result start(
        const Terminal_launch_config&  config,
        Terminal_backend_callbacks     callbacks)
    {
        Native_backend_start_gate start_gate{
            m_mutex,
            m_running,
            m_start_attempted,
            m_start_in_progress,
        };
        Native_backend_start_precheck precheck = validate_native_backend_start_preconditions(
            config,
            callbacks,
            start_gate,
            QStringLiteral("ConPTY"));
        if (is_backend_rejection(precheck.result)) {
            return precheck.result;
        }

        Terminal_effective_launch_config effective_config =
            std::move(*precheck.effective_config);

        const auto reject_start = [&](Terminal_backend_error_code code, QString message) {
            return reject_native_backend_start_attempt(
                callbacks,
                start_gate,
                code,
                std::move(message));
        };

        const std::optional<Conpty_api> conpty_api = load_conpty_api();
        if (!conpty_api.has_value()) {
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    QStringLiteral("Windows ConPTY API is not available"));
        }

        if (!size_fits_conpty(effective_config.initial_grid_size)) {
            return
                reject_start(
                    Terminal_backend_error_code::INVALID_INITIAL_GRID_SIZE,
                    QStringLiteral("initial terminal size is outside the ConPTY range"));
        }

        Unique_handle pty_input_read;
        Unique_handle pty_input_write;
        Unique_handle pty_output_read;
        Unique_handle pty_output_write;

        HANDLE input_read_handle = nullptr;
        HANDLE input_write_handle = nullptr;
        if (!CreatePipe(&input_read_handle, &input_write_handle, nullptr, 0)) {
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    windows_error_message(QStringLiteral("CreatePipe input"), GetLastError()));
        }
        pty_input_read.reset(input_read_handle);
        pty_input_write.reset(input_write_handle);

        HANDLE output_read_handle = nullptr;
        HANDLE output_write_handle = nullptr;
        if (!CreatePipe(&output_read_handle, &output_write_handle, nullptr, 0)) {
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    windows_error_message(QStringLiteral("CreatePipe output"), GetLastError()));
        }
        pty_output_read.reset(output_read_handle);
        pty_output_write.reset(output_write_handle);

        HPCON local_conpty = nullptr;
        const HRESULT create_result = conpty_api->create(
            coord_from_grid_size(effective_config.initial_grid_size),
            pty_input_read.get(),
            pty_output_write.get(),
            0,
            &local_conpty);
        if (FAILED(create_result)) {
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    hresult_message(QStringLiteral("CreatePseudoConsole"), create_result));
        }

        LPPROC_THREAD_ATTRIBUTE_LIST attribute_list = nullptr;
        Terminal_backend_result attribute_failure;
        std::vector<std::byte> attribute_storage = initialized_attribute_list_storage(
            local_conpty,
            attribute_list,
            attribute_failure);
        if (is_backend_rejection(attribute_failure)) {
            conpty_api->close(local_conpty);
            return reject_start(
                attribute_failure.error->code,
                attribute_failure.error->message);
        }

        STARTUPINFOEXW startup_info{};
        startup_info.StartupInfo.cb = sizeof(startup_info);
        // Parent std handles can be redirected by CTest. NULL std handles suppress
        // inheritance here; the pseudoconsole attribute supplies the child terminal.
        startup_info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup_info.lpAttributeList = attribute_list;

        PROCESS_INFORMATION process_information{};
        std::wstring command_line = command_line_from_argv(effective_config.argv);
        std::wstring working_directory;
        LPCWSTR working_directory_ptr = nullptr;
        if (!effective_config.working_directory.isEmpty()) {
            working_directory = wide_from_qstring(effective_config.working_directory);
            working_directory_ptr = working_directory.c_str();
        }

        std::wstring environment_block =
            environment_block_from_process_environment(effective_config.environment);

        Job_object_create_result process_job_result = create_process_tree_job();
        if (!process_job_result.handle) {
            conpty_api->close(local_conpty);
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    windows_error_message(
                        process_job_result.failed_operation,
                        process_job_result.error_code));
        }
        Unique_handle process_job = std::move(process_job_result.handle);

        DWORD creation_flags =
            EXTENDED_STARTUPINFO_PRESENT |
            CREATE_UNICODE_ENVIRONMENT |
            CREATE_SUSPENDED;
        if (effective_config.process_group_policy ==
            Terminal_process_group_policy::CREATE_NEW_SESSION)
        {
            creation_flags |= CREATE_NEW_PROCESS_GROUP;
        }

        const BOOL process_created = CreateProcessW(
            nullptr,
            command_line.data(),
            nullptr,
            nullptr,
            FALSE,
            creation_flags,
            environment_block.data(),
            working_directory_ptr,
            &startup_info.StartupInfo,
            &process_information);
        const DWORD create_process_error = process_created ? ERROR_SUCCESS : GetLastError();

        DeleteProcThreadAttributeList(attribute_list);
        attribute_storage.clear();

        if (!process_created) {
            conpty_api->close(local_conpty);
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    windows_error_message(QStringLiteral("CreateProcessW"), create_process_error));
        }

        Unique_handle process_handle(process_information.hProcess);
        Unique_handle thread_handle(process_information.hThread);
        pty_input_read.reset();
        pty_output_write.reset();

        if (!AssignProcessToJobObject(process_job.get(), process_handle.get())) {
            const DWORD assign_job_error = GetLastError();
            TerminateProcess(process_handle.get(), 1U);
            conpty_api->close(local_conpty);
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    windows_error_message(
                        QStringLiteral("AssignProcessToJobObject"),
                        assign_job_error));
        }

        if (ResumeThread(thread_handle.get()) == static_cast<DWORD>(-1)) {
            const DWORD resume_error = GetLastError();
            TerminateJobObject(process_job.get(), 1U);
            conpty_api->close(local_conpty);
            return
                reject_start(
                    Terminal_backend_error_code::START_FAILED,
                    windows_error_message(QStringLiteral("ResumeThread"), resume_error));
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_api                = *conpty_api;
            m_conpty             = local_conpty;
            m_callbacks          = std::move(callbacks);
            m_input_write        = std::move(pty_input_write);
            m_output_read        = std::move(pty_output_read);
            m_process_job        = std::move(process_job);
            m_process            = std::move(process_handle);
            m_running            = true;
            m_start_attempted    = true;
            m_start_in_progress  = false;
            m_stopping           = false;
            m_output_paused      = false;
            m_paused_output_delivery_in_progress = false;
            m_exit_reported      = false;
            m_reader_finished    = false;
            m_writer_failed      = false;
            m_startup_aborted    = false;
            m_termination_policy = effective_config.termination_policy;
            m_exit_reason_override.reset();
            m_queued_write_bytes = 0U;
            m_reader_thread_handle.reset();
            m_writer_thread_handle.reset();
            m_paused_output.clear();
            m_write_queue.clear();
        }

        std::shared_ptr<std::latch> startup_gate;
        try {
            startup_gate = std::make_shared<std::latch>(1);
            m_reader_thread = std::thread(
                &Impl::run_worker_after_startup_gate,
                this,
                startup_gate,
                &Impl::read_loop);
            m_writer_thread = std::thread(
                &Impl::run_worker_after_startup_gate,
                this,
                startup_gate,
                &Impl::write_loop);
            m_wait_thread = std::thread(
                &Impl::run_worker_after_startup_gate,
                this,
                startup_gate,
                &Impl::wait_loop);
            startup_gate->count_down();
        }
        catch (const std::exception& error) {
            if (startup_gate) {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_startup_aborted = true;
                }
                startup_gate->count_down();
            }
            const QString message = QStringLiteral("ConPTY worker thread startup failed: %1")
                .arg(QString::fromLocal8Bit(error.what()));
            report_error(Terminal_backend_error_code::START_FAILED, message);
            shutdown();
            return backend_reject(Terminal_backend_error_code::START_FAILED, message);
        }

        return backend_accept();
    }

    Terminal_backend_result write(QByteArray bytes)
    {
        if (bytes.isEmpty()) {
            return
                backend_reject(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("ConPTY write requires bytes"));
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running || m_stopping || m_writer_failed || !m_input_write) {
            return
                backend_reject(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("ConPTY backend is not writable"));
        }

        const std::size_t byte_count = static_cast<std::size_t>(bytes.size());
        if (!native_backend_write_queue_can_accept(m_queued_write_bytes, byte_count)) {
            return
                backend_reject(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("ConPTY write queue limit reached"));
        }

        add_native_backend_queued_write_bytes(m_queued_write_bytes, byte_count);
        m_write_queue.push_back({std::move(bytes), false});
        m_write_cv.notify_one();
        return backend_accept();
    }

    Terminal_backend_result resize(Terminal_backend_resize_request request)
    {
        if (!size_fits_conpty(request.grid_size)) {
            return
                backend_reject(
                    Terminal_backend_error_code::RESIZE_FAILED,
                    QStringLiteral("ConPTY resize requires a positive SHORT-sized grid"));
        }

        HPCON conpty = nullptr;
        Conpty_api::ResizePseudoConsole_fn resize_conpty = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running || m_stopping || m_conpty == nullptr) {
                return
                    backend_reject(
                        Terminal_backend_error_code::RESIZE_FAILED,
                        QStringLiteral("ConPTY resize requires a running process"));
            }

            conpty = m_conpty;
            resize_conpty = m_api.resize;
        }

        const HRESULT result =
            resize_conpty(conpty, coord_from_grid_size(request.grid_size));
        if (FAILED(result)) {
            return
                backend_reject(
                    Terminal_backend_error_code::RESIZE_FAILED,
                    hresult_message(QStringLiteral("ResizePseudoConsole"), result));
        }

        return backend_accept();
    }

    Terminal_backend_result set_output_paused(bool paused)
    {
        QByteArray paused_output;
        bool paused_output_delivery_started = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_output_paused = paused;
            if (!m_output_paused && !m_paused_output.isEmpty()) {
                paused_output_delivery_started =
                    take_paused_output_for_delivery_locked(paused_output);
            }
        }

        if (!paused_output.isEmpty()) {
            deliver_output(std::move(paused_output));
        }
        finish_paused_output_delivery(paused_output_delivery_started);
        if (!paused && !paused_output_delivery_started) {
            m_output_cv.notify_all();
        }

        return backend_accept();
    }

    Terminal_backend_result interrupt()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running || m_stopping || m_writer_failed || !m_input_write) {
            return
                backend_reject(
                    Terminal_backend_error_code::INTERRUPT_FAILED,
                    QStringLiteral("ConPTY interrupt requires a running process"));
        }

        if (!native_backend_write_queue_can_accept(m_queued_write_bytes, 1U)) {
            return
                backend_reject(
                    Terminal_backend_error_code::INTERRUPT_FAILED,
                    QStringLiteral("ConPTY write queue limit reached"));
        }

        add_native_backend_queued_write_bytes(m_queued_write_bytes, 1U);
        // Optimistically record interrupt as the requested exit cause: the wait
        // thread can observe process exit before the writer's delivery callback
        // returns, and that exit should still classify as interrupted. If delivery
        // instead fails while the process is still running,
        // mark_writer_failed_after_write_failure() drops this override (in the same
        // critical section that marks the writer failed) and surfaces the failure so
        // a later natural exit is not misreported as interrupted.
        m_exit_reason_override = Terminal_exit_reason::INTERRUPTED;
        m_write_queue.push_back({QByteArray(1, '\x03'), true});
        m_write_cv.notify_one();
        return backend_accept();
    }

    Terminal_backend_result terminate()
    {
        HANDLE process     = nullptr;
        HANDLE process_job = nullptr;
        Terminal_termination_policy policy;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running || m_stopping || !m_process) {
                return
                    backend_reject(
                        Terminal_backend_error_code::TERMINATE_FAILED,
                        QStringLiteral("ConPTY terminate requires a running process"));
            }

            process     = m_process.get();
            process_job = m_process_job.get();
            policy      = m_termination_policy;

            m_stopping             = true;
            m_output_paused        = false;
            m_exit_reason_override = Terminal_exit_reason::TERMINATED;
        }

        m_output_cv.notify_all();
        m_write_cv.notify_all();
        cancel_blocking_input();
        request_conpty_close();
        return start_termination_escalation(process, process_job, policy);
    }

    Terminal_backend_result start_termination_escalation(
        HANDLE                         process,
        HANDLE                         process_job,
        Terminal_termination_policy    policy)
    {
        std::shared_ptr<std::latch> startup_gate;
        try {
            startup_gate = std::make_shared<std::latch>(1);
            m_termination_thread = std::thread(
                &Impl::run_termination_after_startup_gate,
                this,
                startup_gate,
                process,
                process_job,
                policy);
            startup_gate->count_down();
        }
        catch (const std::exception& error) {
            if (startup_gate) {
                startup_gate->count_down();
            }
            const QString message =
                QStringLiteral("ConPTY termination escalation worker failed: %1")
                    .arg(QString::fromLocal8Bit(error.what()));
            if (!TerminateJobObject(process_job, 1U)) {
                const DWORD terminate_job_error = GetLastError();
                TerminateProcess(process, 1U);
                return
                    backend_reject(
                        Terminal_backend_error_code::TERMINATE_FAILED,
                        windows_error_message(
                            QStringLiteral("TerminateJobObject"),
                            terminate_job_error));
            }

            return backend_reject(Terminal_backend_error_code::TERMINATE_FAILED, message);
        }

        return backend_accept();
    }

    void shutdown()
    {
        HANDLE process     = nullptr;
        HANDLE process_job = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_shutdown_started) {
                return;
            }

            m_shutdown_started = true;
            m_stopping         = true;
            m_output_paused    = false;
            m_callbacks        = {};
            process            = m_process.get();
            process_job        = m_process_job.get();
        }

        m_output_cv.notify_all();
        m_write_cv.notify_all();
        cancel_blocking_input();
        request_conpty_close();

        bool process_tree_terminated = false;
        if (process_job != nullptr && process_job != INVALID_HANDLE_VALUE) {
            process_tree_terminated = TerminateJobObject(process_job, 1U);
        }

        if (!process_tree_terminated &&
            process != nullptr &&
            process != INVALID_HANDLE_VALUE)
        {
            DWORD exit_code = 0;
            if (GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE) {
                TerminateProcess(process, 1U);
            }
        }

        if (!reader_finished_within(k_conpty_reader_close_grace)) {
            cancel_blocking_io();
        }
        join_threads();

        std::lock_guard<std::mutex> lock(m_mutex);
        m_input_write.reset();
        m_output_read.reset();
        m_process.reset();
        m_process_job.reset();
        m_reader_thread_handle.reset();
        m_writer_thread_handle.reset();
        m_write_queue.clear();
        m_queued_write_bytes = 0U;
        m_running = false;
    }

    // True when the calling thread is one of this backend's own worker threads.
    // Destruction reached from a worker callback (for example a process_exited
    // delivered by wait_loop) must not tear down inline: shutdown() joins the
    // worker threads, so join_or_detach would detach the calling thread and free
    // this Impl while that worker is still on the stack.
    //
    // Each worker records its id (register_worker_thread, under m_mutex) before it
    // runs any callback-capable work. The startup gate keeps those callbacks
    // blocked until start() has committed the std::thread members, and the id set
    // keeps destruction independent of unsynchronized std::thread member reads.
    bool is_worker_thread()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return
            m_worker_thread_ids.find(std::this_thread::get_id()) !=
            m_worker_thread_ids.end();
    }

    bool has_active_public_call()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_public_call_depth != 0U;
    }

    void register_worker_thread()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_worker_thread_ids.insert(std::this_thread::get_id());
    }

    void run_worker_after_startup_gate(
        std::shared_ptr<std::latch> startup_gate,
        void (Impl::*loop)())
    {
        register_worker_thread();
        startup_gate->wait();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_startup_aborted) {
                return;
            }
        }
        (this->*loop)();
    }

    void run_termination_after_startup_gate(
        std::shared_ptr<std::latch>     startup_gate,
        HANDLE                          process,
        HANDLE                          process_job,
        Terminal_termination_policy     policy)
    {
        register_worker_thread();
        startup_gate->wait();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_startup_aborted) {
                return;
            }
        }
        termination_escalation_loop(process, process_job, policy);
    }

    // Run shutdown() and delete on a fresh, non-worker thread so join_threads()
    // can join worker threads and any public backend call already on the stack
    // can return before the Impl is freed. Takes ownership of this Impl.
    void defer_shutdown_and_delete() noexcept
    {
        try {
            std::thread([this] {
                wait_for_public_calls_to_finish();
                shutdown();
                delete this;
            }).detach();
        }
        catch (...) {
            request_shutdown_without_cleanup();
        }
    }

private:
    // Best-effort teardown for the unreachable-in-practice case where the
    // deferred-shutdown thread cannot be spawned. It must not join or delete: the
    // worker threads are still running against this Impl, so the Impl is
    // deliberately leaked (safe: no use-after-free) while the child process tree
    // is force-terminated so no process is left running.
    void request_shutdown_without_cleanup() noexcept
    {
        HANDLE process     = nullptr;
        HANDLE process_job = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_shutdown_started) {
                return;
            }

            m_shutdown_started = true;
            m_stopping         = true;
            m_output_paused    = false;
            m_callbacks        = {};
            process            = m_process.get();
            process_job        = m_process_job.get();
        }

        m_output_cv.notify_all();
        m_write_cv.notify_all();
        cancel_blocking_io();
        request_conpty_close();

        // Same job-then-root termination shape as shutdown(): the pseudoconsole is
        // closed and, if the job cannot be terminated, the root process is killed
        // directly, so leaking the Impl never leaves the child tree running.
        bool process_tree_terminated = false;
        if (process_job != nullptr && process_job != INVALID_HANDLE_VALUE) {
            process_tree_terminated = TerminateJobObject(process_job, 1U);
        }

        if (!process_tree_terminated &&
            process != nullptr &&
            process != INVALID_HANDLE_VALUE)
        {
            DWORD exit_code = 0;
            if (GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE) {
                TerminateProcess(process, 1U);
            }
        }
    }

    Terminal_backend_callbacks callbacks_for_delivery()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_callbacks;
    }

    void enter_public_call()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_public_call_depth;
    }

    void leave_public_call()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        --m_public_call_depth;
        if (m_public_call_depth == 0U) {
            m_public_call_cv.notify_all();
        }
    }

    void wait_for_public_calls_to_finish()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_public_call_cv.wait(lock, [this] {
            return m_public_call_depth == 0U;
        });
    }

    void report_error(Terminal_backend_error_code code, QString message)
    {
        Terminal_backend_callbacks callbacks = callbacks_for_delivery();
        report_native_backend_error(callbacks, code, std::move(message));
    }

    void report_exit_once(Terminal_exit_reason default_reason, int exit_code)
    {
        Terminal_backend_callbacks callbacks;
        Terminal_exit_reason reason = default_reason;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_exit_reported) {
                return;
            }

            if (m_exit_reason_override.has_value()) {
                reason = *m_exit_reason_override;
            }

            m_exit_reported = true;
            m_running       = false;
            m_stopping      = true;
            m_output_paused = false;
        }

        m_output_cv.notify_all();
        m_write_cv.notify_all();

        callbacks = callbacks_for_delivery();
        report_native_backend_exit(callbacks, reason, exit_code);
    }

    void deliver_output(QByteArray bytes)
    {
        Terminal_backend_callbacks callbacks = callbacks_for_delivery();
        deliver_native_backend_output(callbacks, std::move(bytes));
    }

    bool take_paused_output_for_delivery_locked(QByteArray& paused_output)
    {
        if (m_paused_output.isEmpty()) {
            return false;
        }

        paused_output = std::move(m_paused_output);
        m_paused_output.clear();
        m_paused_output_delivery_in_progress = true;
        return true;
    }

    void finish_paused_output_delivery(bool delivery_started)
    {
        if (!delivery_started) {
            return;
        }

        for (;;) {
            QByteArray next_paused_output;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_output_paused && !m_paused_output.isEmpty()) {
                    next_paused_output = std::move(m_paused_output);
                    m_paused_output.clear();
                }
                else {
                    m_paused_output_delivery_in_progress = false;
                    break;
                }
            }

            deliver_output(std::move(next_paused_output));
        }

        m_output_cv.notify_all();
    }

    void deliver_or_buffer_output(QByteArray bytes)
    {
        const qsizetype bytes_size = bytes.size();
        bool should_deliver = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const bool should_buffer =
                m_output_paused || m_paused_output_delivery_in_progress;
            if (should_buffer &&
                bytes_size <=
                    k_paused_output_high_watermark_bytes - m_paused_output.size())
            {
                append_native_backend_paused_output(m_paused_output, std::move(bytes));
            }
            else {
                should_deliver = true;
            }
        }

        if (should_deliver) {
            deliver_output(std::move(bytes));
        }
    }

    void mark_reader_finished()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_reader_finished = true;
        }

        m_reader_cv.notify_all();
    }

    void wait_for_reader_finished()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_reader_cv.wait(lock, [&] {
            return m_reader_finished;
        });
    }

    bool reader_finished_within(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_reader_cv.wait_for(lock, timeout, [&] {
            return m_reader_finished;
        });
    }

    void request_conpty_close()
    {
        HPCON conpty = nullptr;
        Conpty_api::ClosePseudoConsole_fn close_conpty_api = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            conpty           = m_conpty;
            close_conpty_api = m_api.close;
            m_conpty         = nullptr;
        }

        close_pseudoconsole_detached(close_conpty_api, conpty);
    }

    void record_reader_thread_handle()
    {
        Unique_handle thread_handle = duplicate_current_thread_handle();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_reader_thread_handle = std::move(thread_handle);
    }

    void record_writer_thread_handle()
    {
        Unique_handle thread_handle = duplicate_current_thread_handle();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_writer_thread_handle = std::move(thread_handle);
    }

    void read_loop()
    {
        record_reader_thread_handle();

        std::vector<char> buffer(k_native_backend_output_read_chunk_bytes);

        for (;;) {
            HANDLE output_read = nullptr;
            DWORD bytes_to_read = static_cast<DWORD>(buffer.size());
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_output_cv.wait(lock, [&] {
                    return
                        m_stopping ||
                        (!m_paused_output_delivery_in_progress &&
                            (!m_output_paused ||
                             m_paused_output.size() <
                                k_paused_output_high_watermark_bytes));
                });

                output_read = m_output_read.get();
                if (output_read == nullptr || output_read == INVALID_HANDLE_VALUE) {
                    break;
                }

                if (m_output_paused &&
                    m_paused_output.size() < k_paused_output_high_watermark_bytes)
                {
                    const qsizetype paused_output_room =
                        k_paused_output_high_watermark_bytes - m_paused_output.size();
                    bytes_to_read = static_cast<DWORD>(std::min<qsizetype>(
                        static_cast<qsizetype>(buffer.size()),
                        paused_output_room));
                }
            }

            DWORD bytes_read = 0U;
            const BOOL read_ok = ReadFile(
                output_read,
                buffer.data(),
                bytes_to_read,
                &bytes_read,
                nullptr);
            if (!read_ok) {
                const DWORD error_code = GetLastError();
                if (error_code != ERROR_BROKEN_PIPE       &&
                    error_code != ERROR_OPERATION_ABORTED &&
                    !stopping())
                {
                    report_error(
                        Terminal_backend_error_code::READ_FAILED,
                        windows_error_message(QStringLiteral("ConPTY output read"), error_code));
                }
                break;
            }

            if (bytes_read == 0U) {
                break;
            }

            deliver_or_buffer_output(QByteArray(
                buffer.data(),
                static_cast<qsizetype>(bytes_read)));
        }

        mark_reader_finished();
    }

    void write_loop()
    {
        record_writer_thread_handle();

        for (;;) {
            Queued_write write;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_write_cv.wait(lock, [&] {
                    return m_stopping || !m_write_queue.empty();
                });

                if (m_stopping) {
                    return;
                }

                write = std::move(m_write_queue.front());
                m_write_queue.pop_front();
                remove_native_backend_queued_write_bytes(
                    m_queued_write_bytes,
                    static_cast<std::size_t>(write.bytes.size()));
            }

            if (!write_all(write.bytes)) {
                mark_writer_failed_after_write_failure(write.marks_interrupted_exit);
                return;
            }

            if (write.marks_interrupted_exit) {
                mark_interrupt_delivered();
            }
        }
    }

    void mark_writer_failed_after_write_failure(bool failed_write_was_interrupt)
    {
        bool should_report_interrupt_failed = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // Mark the writer failed and drop the optimistic INTERRUPTED override in
            // the SAME critical section. The wait thread classifies exit by reading
            // m_exit_reason_override (report_exit_once). If the failed-writer mark
            // and the override clear were two separate locked steps, the wait thread
            // could observe a stale INTERRUPTED override between them and misclassify
            // a natural exit as interrupted even though the Ctrl+C byte was never
            // delivered. One critical section closes that window.
            //
            // This race has no deterministic regression test: the backend test
            // harness drives a real ConPTY child through the public interface and
            // has no seam to force write_all() to fail while simultaneously driving a
            // natural child exit. The fix is correct by construction -- the override
            // clear and the writer-failed mark are atomic under m_mutex, so no other
            // thread can observe a stale INTERRUPTED override after a failed
            // interrupt write -- and a deterministic test would require a
            // write-failure injection seam that does not currently exist.
            if (!m_stopping) {
                m_writer_failed = true;
                m_input_write.reset();
            }

            // The interrupt byte never reached the child. Drop the optimistic
            // INTERRUPTED override taken at request time so a later natural exit is
            // not misreported as interrupted -- but only while the process is still
            // running. If it has already exited and been classified, that
            // classification stands (the exit may well have been the interrupt).
            if (failed_write_was_interrupt &&
                !m_exit_reported &&
                m_exit_reason_override == Terminal_exit_reason::INTERRUPTED)
            {
                m_exit_reason_override.reset();
                should_report_interrupt_failed = true;
            }
        }

        if (should_report_interrupt_failed) {
            report_error(
                Terminal_backend_error_code::INTERRUPT_FAILED,
                QStringLiteral("ConPTY interrupt byte could not be delivered to the child"));
        }
    }

    void mark_interrupt_delivered()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_stopping) {
            m_exit_reason_override = Terminal_exit_reason::INTERRUPTED;
        }
    }

    bool write_all(const QByteArray& bytes)
    {
        qsizetype offset = 0;
        while (offset < bytes.size()) {
            HANDLE input_write = nullptr;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_stopping || !m_input_write) {
                    return false;
                }

                input_write = m_input_write.get();
            }

            const qsizetype remaining = bytes.size() - offset;
            const DWORD write_size = static_cast<DWORD>(
                std::min<qsizetype>(remaining, std::numeric_limits<DWORD>::max()));
            DWORD bytes_written = 0U;
            const BOOL write_ok = WriteFile(
                input_write,
                bytes.constData() + offset,
                write_size,
                &bytes_written,
                nullptr);
            if (!write_ok) {
                const DWORD error_code = GetLastError();
                if (error_code != ERROR_BROKEN_PIPE       &&
                    error_code != ERROR_OPERATION_ABORTED &&
                    !stopping())
                {
                    report_error(
                        Terminal_backend_error_code::WRITE_FAILED,
                        windows_error_message(QStringLiteral("ConPTY input write"), error_code));
                }
                return false;
            }

            if (bytes_written == 0U) {
                report_error(
                    Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("ConPTY input write made no progress"));
                return false;
            }

            offset += static_cast<qsizetype>(bytes_written);
        }

        return true;
    }

    void wait_loop()
    {
        HANDLE process = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            process = m_process.get();
        }

        if (process == nullptr || process == INVALID_HANDLE_VALUE) {
            return;
        }

        WaitForSingleObject(process, INFINITE);

        DWORD exit_code = 0U;
        if (!GetExitCodeProcess(process, &exit_code)) {
            exit_code = 1U;
        }

        QByteArray paused_output;
        bool paused_output_delivery_started = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_output_paused = false;
            if (!m_paused_output.isEmpty()) {
                paused_output_delivery_started =
                    take_paused_output_for_delivery_locked(paused_output);
            }
        }
        if (!paused_output.isEmpty()) {
            deliver_output(std::move(paused_output));
        }
        finish_paused_output_delivery(paused_output_delivery_started);
        if (!paused_output_delivery_started) {
            m_output_cv.notify_all();
        }
        request_conpty_close();
        if (!reader_finished_within(k_conpty_reader_close_grace)) {
            cancel_blocking_io();
            wait_for_reader_finished();
        }
        report_exit_once(Terminal_exit_reason::EXITED, static_cast<int>(exit_code));
        terminate_process_tree_after_root_exit();
    }

    void termination_escalation_loop(
        HANDLE                         process,
        HANDLE                         process_job,
        Terminal_termination_policy    policy)
    {
        if (process_exited_within(process, policy.graceful_interval)) {
            terminate_process_tree_after_root_exit();
            return;
        }

        if (!TerminateJobObject(process_job, 1U)) {
            const DWORD terminate_job_error = GetLastError();
            TerminateProcess(process, 1U);
            report_error(
                Terminal_backend_error_code::TERMINATE_FAILED,
                windows_error_message(
                    QStringLiteral("TerminateJobObject"),
                    terminate_job_error));
            return;
        }

        if (process_exited_within(process, policy.kill_interval)) {
            return;
        }

        report_error(
            Terminal_backend_error_code::TERMINATE_FAILED,
            QStringLiteral("ConPTY process remained active after forced termination"));
    }

    void terminate_process_tree_after_root_exit()
    {
        HANDLE process_job = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            process_job = m_process_job.get();
        }

        if (process_job != nullptr && process_job != INVALID_HANDLE_VALUE) {
            if (!TerminateJobObject(process_job, 1U)) {
                report_error(
                    Terminal_backend_error_code::TERMINATE_FAILED,
                    windows_error_message(QStringLiteral("TerminateJobObject"), GetLastError()));
            }
        }
    }

    bool stopping()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stopping;
    }

    void cancel_blocking_io()
    {
        Unique_handle output_read;
        Unique_handle input_write;
        Unique_handle reader_thread;
        Unique_handle writer_thread;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            output_read   = duplicate_handle(m_output_read.get());
            input_write   = duplicate_handle(m_input_write.get());
            reader_thread = duplicate_handle(m_reader_thread_handle.get());
            writer_thread = duplicate_handle(m_writer_thread_handle.get());
        }

        if (output_read) {
            CancelIoEx(output_read.get(), nullptr);
        }

        if (input_write) {
            CancelIoEx(input_write.get(), nullptr);
        }

        if (reader_thread) {
            CancelSynchronousIo(reader_thread.get());
        }

        if (writer_thread) {
            CancelSynchronousIo(writer_thread.get());
        }
    }

    void cancel_blocking_input()
    {
        Unique_handle input_write;
        Unique_handle writer_thread;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            input_write   = duplicate_handle(m_input_write.get());
            writer_thread = duplicate_handle(m_writer_thread_handle.get());
        }

        if (input_write) {
            CancelIoEx(input_write.get(), nullptr);
        }

        if (writer_thread) {
            CancelSynchronousIo(writer_thread.get());
        }
    }

    void join_threads()
    {
        join_or_detach_native_backend_thread(m_reader_thread);
        join_or_detach_native_backend_thread(m_writer_thread);
        join_or_detach_native_backend_thread(m_wait_thread);
        join_or_detach_native_backend_thread(m_termination_thread);
    }

    std::mutex                         m_mutex;
    std::condition_variable            m_output_cv;
    std::condition_variable            m_write_cv;
    std::condition_variable            m_reader_cv;
    std::condition_variable            m_public_call_cv;
    Terminal_backend_callbacks         m_callbacks;
    Conpty_api                         m_api;
    HPCON                              m_conpty = nullptr;
    Unique_handle                      m_input_write;
    Unique_handle                      m_output_read;
    Unique_handle                      m_process_job;
    Unique_handle                      m_process;
    Unique_handle                      m_reader_thread_handle;
    Unique_handle                      m_writer_thread_handle;
    std::thread                        m_reader_thread;
    std::thread                        m_writer_thread;
    std::thread                        m_wait_thread;
    std::thread                        m_termination_thread;
    std::set<std::thread::id>          m_worker_thread_ids;
    QByteArray                         m_paused_output;
    std::deque<Queued_write>           m_write_queue;
    std::optional<Terminal_exit_reason>
                                       m_exit_reason_override;
    Terminal_termination_policy        m_termination_policy;
    std::size_t                        m_queued_write_bytes = 0U;
    std::size_t                        m_public_call_depth = 0U;
    bool                               m_running = false;
    bool                               m_start_attempted = false;
    bool                               m_start_in_progress = false;
    bool                               m_startup_aborted = false;
    bool                               m_stopping = false;
    bool                               m_output_paused = false;
    bool                               m_paused_output_delivery_in_progress = false;
    bool                               m_exit_reported = false;
    bool                               m_shutdown_started = false;
    bool                               m_reader_finished = false;
    bool                               m_writer_failed = false;
};

Windows_conpty_backend::Windows_conpty_backend()
:
    m_impl(std::make_unique<Impl>())
{}

Windows_conpty_backend::~Windows_conpty_backend()
{
    // Defer when teardown is reached from a callback or while a public backend
    // call is still unwinding, so the Impl is not freed under live stack frames.
    if (m_impl &&
        (m_impl->is_worker_thread() || m_impl->has_active_public_call()))
    {
        Impl* impl = m_impl.release();
        impl->defer_shutdown_and_delete();
    }
}

Terminal_backend_result Windows_conpty_backend::start(
    const Terminal_launch_config&  config,
    Terminal_backend_callbacks     callbacks)
{
    Impl* impl = m_impl.get();
    auto guard = impl->public_call_guard();
    return impl->start(config, std::move(callbacks));
}

Terminal_backend_result Windows_conpty_backend::write(QByteArray bytes)
{
    Impl* impl = m_impl.get();
    auto guard = impl->public_call_guard();
    return impl->write(std::move(bytes));
}

Terminal_backend_result Windows_conpty_backend::resize(
    Terminal_backend_resize_request request)
{
    Impl* impl = m_impl.get();
    auto guard = impl->public_call_guard();
    return impl->resize(request);
}

Terminal_backend_result Windows_conpty_backend::set_output_paused(bool paused)
{
    Impl* impl = m_impl.get();
    auto guard = impl->public_call_guard();
    return impl->set_output_paused(paused);
}

Terminal_backend_result Windows_conpty_backend::interrupt()
{
    Impl* impl = m_impl.get();
    auto guard = impl->public_call_guard();
    return impl->interrupt();
}

Terminal_backend_result Windows_conpty_backend::terminate()
{
    Impl* impl = m_impl.get();
    auto guard = impl->public_call_guard();
    return impl->terminate();
}

std::unique_ptr<Terminal_backend> make_windows_conpty_backend()
{
    return std::make_unique<Windows_conpty_backend>();
}

}

#endif
