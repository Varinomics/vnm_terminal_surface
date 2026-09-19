#include <vnm_process_custody/process_spawn.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <set>

namespace vnm::process_custody {

#ifdef LOGONOMIC_ENABLE_PROCESS_SPAWN_TEST_HOOKS
namespace { std::atomic_bool s_start_unconfirmed_for_test = false; }

void set_spawn_start_unconfirmed_for_test(bool enabled)
{
    s_start_unconfirmed_for_test.store(enabled, std::memory_order_release);
}
#endif

namespace {

constexpr std::size_t k_max_windows_environment_value_units = 32766;
constexpr std::size_t k_max_windows_native_command_line_units = 32766;

std::string last_error_text(const char* in_call)
{
    return std::string(in_call) + " failed: error " + std::to_string(GetLastError());
}

bool utf8_to_wide(const std::string& in_text, std::wstring* out_wide)
{
    if (in_text.empty()) {
        out_wide->clear();
        return true;
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, in_text.data(), static_cast<int>(in_text.size()), nullptr, 0);
    if (needed <= 0) {
        return false;
    }
    out_wide->resize(static_cast<std::size_t>(needed));
    return MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, in_text.data(), static_cast<int>(in_text.size()),
        out_wide->data(), needed) == needed;
}

// The Microsoft C runtime's command-line grammar, which is what a child
// built with that runtime parses back into argv: an argument is quoted when
// it is empty or contains whitespace or a quote; backslashes are literal
// except when they precede a quote (doubled) or end a quoted argument.
void append_quoted_argument(const std::wstring& in_argument, std::wstring* out_command_line)
{
    if (!out_command_line->empty()) {
        out_command_line->push_back(L' ');
    }
    const bool needs_quotes =
        in_argument.empty() ||
        in_argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needs_quotes) {
        out_command_line->append(in_argument);
        return;
    }
    out_command_line->push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t c : in_argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out_command_line->append(backslashes * 2 + 1, L'\\');
            backslashes = 0;
            out_command_line->push_back(L'"');
            continue;
        }
        out_command_line->append(backslashes, L'\\');
        backslashes = 0;
        out_command_line->push_back(c);
    }
    out_command_line->append(backslashes * 2, L'\\');
    out_command_line->push_back(L'"');
}

std::string ascii_lowercase(std::string_view in_text)
{
    std::string lowered(in_text);
    for (char& character : lowered) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return lowered;
}

} // namespace

Spawn_result spawn(const Spawn_request& in_request)
{
    Spawn_result result;

    std::wstring executable;
    std::wstring working_directory;
    std::wstring command_line;
    if (!utf8_to_wide(in_request.executable, &executable) ||
        !utf8_to_wide(in_request.working_directory, &working_directory))
    {
        result.error = "spawn: executable or working directory is not valid UTF-8";
        return result;
    }
    // CREATE_UNICODE_ENVIRONMENT block: NAME=VALUE\0 ... \0\0. Windows names
    // are case-insensitive, so refuse a final request that would hand the
    // child ambiguous duplicate variables even if an earlier layer missed it.
    std::vector<std::pair<std::string, std::string>> environment;
    std::set<std::string> names;
    for (const std::string& entry : in_request.environment) {
        const std::size_t equal = entry.find('=');
        if (equal == std::string::npos || equal == 0) {
            result.error = "spawn: an environment entry is not NAME=VALUE";
            return result;
        }
        const std::string name = entry.substr(0, equal);
        if (!names.insert(ascii_lowercase(name)).second) {
            result.error = "spawn: duplicate case-insensitive environment name " + name;
            return result;
        }
        environment.emplace_back(name, entry.substr(equal + 1));
    }
    std::sort(environment.begin(), environment.end(), [](const auto& left, const auto& right) {
        return ascii_lowercase(left.first) < ascii_lowercase(right.first);
    });

    std::wstring environment_block;
    for (const auto& [name, value] : environment) {
        if (name.find('\0') != std::string::npos || value.find('\0') != std::string::npos) {
            result.error = "spawn: an environment entry contains NUL";
            return result;
        }
        std::wstring wide_name;
        std::wstring wide_value;
        if (!utf8_to_wide(name, &wide_name) || !utf8_to_wide(value, &wide_value)) {
            result.error = "spawn: an environment entry is not valid UTF-8";
            return result;
        }
        if (wide_value.size() > k_max_windows_environment_value_units) {
            result.error = "spawn: environment value \"" + name + "\" encodes to " +
                std::to_string(wide_value.size()) + " UTF-16 code units, above the native ceiling of " +
                std::to_string(k_max_windows_environment_value_units);
            return result;
        }
        environment_block.append(wide_name);
        environment_block.push_back(L'=');
        environment_block.append(wide_value);
        environment_block.push_back(L'\0');
    }
    if (environment.empty()) {
        environment_block.push_back(L'\0');
    }
    environment_block.push_back(L'\0');

    // Create one inheritable duplicate per requested child entry. The owning
    // handles are never toggled, so concurrent spawns cannot expose one
    // another's temporary inheritance window. Each duplicate's numeric value
    // is also the exact value the child receives on Windows.
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;

    std::vector<HANDLE> handle_list;
    handle_list.reserve(in_request.inherited.size());
    struct Duplicate_guard
    {
        std::vector<HANDLE>* handles;
        ~Duplicate_guard()
        {
            for (const HANDLE handle : *handles) {
                CloseHandle(handle);
            }
        }
    } duplicate_guard{&handle_list};

    for (const Inherited_descriptor& descriptor : in_request.inherited) {
        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(
                GetCurrentProcess(),
                descriptor.parent_handle,
                GetCurrentProcess(),
                &duplicate,
                0,
                TRUE,
                DUPLICATE_SAME_ACCESS))
        {
            result.error = last_error_text("DuplicateHandle(inherited handle)");
            return result;
        }
        handle_list.push_back(duplicate);
        switch (descriptor.child_number) {
            case 0:  startup.StartupInfo.hStdInput  = duplicate; break;
            case 1:  startup.StartupInfo.hStdOutput = duplicate; break;
            case 2:  startup.StartupInfo.hStdError  = duplicate; break;
            default: break;
        }
    }

    for (const Spawn_argument& argument : in_request.argv) {
        std::string resolved;
        if (const auto* text = std::get_if<std::string>(&argument.value)) {
            resolved = *text;
        }
        else {
            const std::size_t index = std::get<Inherited_reference>(argument.value).inherited_index;
            if (index >= handle_list.size()) {
                result.error = "spawn: inherited argv reference is out of range";
                return result;
            }
            resolved = native_handle_argument(handle_list[index]);
        }
        std::wstring wide;
        if (!utf8_to_wide(resolved, &wide)) {
            result.error = "spawn: an argument is not valid UTF-8";
            return result;
        }
        append_quoted_argument(wide, &command_line);
    }
    if (command_line.size() > k_max_windows_native_command_line_units) {
        result.error = "spawn: assembled native command line is " + std::to_string(command_line.size()) +
            " UTF-16 code units, above the native ceiling of " +
            std::to_string(k_max_windows_native_command_line_units);
        return result;
    }

    const DWORD attribute_count = (in_request.job || in_request.generation_job) ? 2 : 1;
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, attribute_count, 0, &attribute_bytes);
    if (attribute_bytes == 0) {
        result.error = last_error_text("InitializeProcThreadAttributeList(size)");
        return result;
    }
    std::vector<unsigned char> attribute_storage(attribute_bytes);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(attributes, attribute_count, 0, &attribute_bytes)) {
        result.error = last_error_text("InitializeProcThreadAttributeList");
        return result;
    }
    struct Attribute_list_guard
    {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~Attribute_list_guard() { DeleteProcThreadAttributeList(list); }
    } attribute_guard{attributes};

    if (!handle_list.empty() &&
        !UpdateProcThreadAttribute(
            attributes,
            0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            handle_list.data(),
            handle_list.size() * sizeof(HANDLE),
            nullptr,
            nullptr))
    {
        result.error = last_error_text("UpdateProcThreadAttribute(HANDLE_LIST)");
        return result;
    }
    HANDLE jobs[2]{};
    std::size_t job_count = 0;
    if (in_request.job) jobs[job_count++] = in_request.job;
    if (in_request.generation_job) jobs[job_count++] = in_request.generation_job;
    if (job_count &&
        !UpdateProcThreadAttribute(
            attributes,
            0,
            PROC_THREAD_ATTRIBUTE_JOB_LIST,
            jobs,
            job_count * sizeof(HANDLE),
            nullptr,
            nullptr))
    {
        result.error = last_error_text("UpdateProcThreadAttribute(JOB_LIST)");
        return result;
    }
    startup.lpAttributeList = attributes;

    // Suspended creation is what makes Job admission happen before the first
    // instruction of the child; the resume below is the admission point.
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        executable.c_str(),
        command_line.data(),
        nullptr,
        nullptr,
        handle_list.empty() ? FALSE : TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | CREATE_NO_WINDOW,
        environment_block.data(),
        working_directory.empty() ? nullptr : working_directory.c_str(),
        &startup.StartupInfo,
        &process);

    if (!created) {
        result.error = last_error_text("CreateProcessW");
        return result;
    }
    DWORD resumed;
#ifdef LOGONOMIC_ENABLE_PROCESS_SPAWN_TEST_HOOKS
    if (s_start_unconfirmed_for_test.load(std::memory_order_acquire)) {
        SetLastError(ERROR_OPERATION_ABORTED);
        resumed = static_cast<DWORD>(-1);
    }
    else
#endif
    {
        resumed = ResumeThread(process.hThread);
    }
    if (resumed == static_cast<DWORD>(-1)) {
        result.error = last_error_text("ResumeThread");
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        bool exited;
#ifdef LOGONOMIC_ENABLE_PROCESS_SPAWN_TEST_HOOKS
        if (s_start_unconfirmed_for_test.load(std::memory_order_acquire)) {
            exited = false;
        }
        else
#endif
        {
            exited = WaitForSingleObject(process.hProcess,
                static_cast<DWORD>(in_request.failed_start_wait_ms)) == WAIT_OBJECT_0;
        }
        if (exited) {
            CloseHandle(process.hProcess);
        }
        else {
            result.process_id = process.dwProcessId;
            result.process_handle = process.hProcess;
            result.error += "; the original created child's native exit remains unconfirmed";
        }
        return result;
    }
    CloseHandle(process.hThread);

    result.ok             = true;
    result.process_id     = process.dwProcessId;
    result.process_handle = process.hProcess;
    result.inherited_handles.assign(handle_list.begin(), handle_list.end());
    return result;
}

Inherited_reference inherited_argument(std::size_t in_inherited_index)
{
    return Inherited_reference{in_inherited_index};
}

std::vector<Inherited_descriptor> inherit_standard_streams()
{
    std::vector<Inherited_descriptor> streams;
    const DWORD slots[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (int position = 0; position <= 2; ++position) {
        const HANDLE handle = GetStdHandle(slots[position]);
        if (handle && handle != INVALID_HANDLE_VALUE) {
            streams.push_back(Inherited_descriptor{handle, position});
        }
    }
    return streams;
}

bool prepare_parent_process(std::string* /*out_error*/)
{
    return true;
}

} // namespace vnm::process_custody
