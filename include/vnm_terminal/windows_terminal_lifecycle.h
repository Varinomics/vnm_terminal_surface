#pragma once

#include <string>

namespace vnm_terminal {

/**
 * Optional host-owned work associated with one Windows terminal lifetime.
 * The default backend owns only its native root and Job; it does not discover
 * or adopt processes outside that Job. Select this extension before birth.
 *
 * Handles are borrowed Windows HANDLE values, without a windows.h dependency.
 * prepare sees an empty Job before child creation. root_admitted sees the exact
 * root after Job assignment and before its first resume. Refusal or exception
 * fails that admission; native birth is never retried by the backend.
 * The backend retains root/Job ownership. The host must not close these handles
 * or change the Job's lifetime policy; it may associate its completion port
 * during prepare and independently retain exact process observation handles.
 *
 * Once prepare is entered, both cleanup methods run exactly once, including on
 * preparation refusal or failure before birth. begin_cleanup precedes native
 * termination and ConPTY close. finish_cleanup follows the native termination
 * request and must retire all uses of the borrowed handles before returning.
 * The backend retains this object through deferred cleanup and Job closure,
 * independently of presentation callbacks. A host retaining another reference
 * must not use borrowed handles after finish_cleanup.
 *
 * Cleanup runs on native workers, outside backend locks. It may wait for owned
 * cleanup, but must not reenter the surface/backend or require GUI dispatch.
 * io_activity runs on I/O workers, may overlap cleanup while pipes drain, and
 * must only enqueue or mark work without waiting. No thread or periodic work is
 * created by the backend for this extension.
 */
class Windows_terminal_lifecycle
{
public:
    virtual ~Windows_terminal_lifecycle() = default;

    virtual bool prepare(void* root_job, std::string* error) = 0;
    virtual bool root_admitted(void* root_process, std::string* error) = 0;
    virtual void io_activity() noexcept = 0;
    virtual void begin_cleanup() noexcept = 0;
    virtual void finish_cleanup() noexcept = 0;
};

} // namespace vnm_terminal
