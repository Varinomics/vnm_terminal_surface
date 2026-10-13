#pragma once

#include <cstdint>
#include <functional>

namespace vnm_terminal {

enum class Terminal_native_receipt_kind
{
    HELPER_PRE_START,
    ROOT_BORN,
    ROOT_ENDED,
    NO_BIRTH,
};

struct Terminal_native_receipt
{
    Terminal_native_receipt_kind kind = Terminal_native_receipt_kind::HELPER_PRE_START;
    std::uint32_t helper_pid = 0;
    std::uint64_t helper_birth_token = 0;
    std::uint32_t root_pid = 0;
    std::uint64_t root_birth_token = 0;
    int root_exit_code = -1;
    int root_signal = 0;
};

enum class Terminal_native_receipt_ack { REJECTED, RETAINED };

// Darwin startup requires a synchronous supported handoff before workload
// START. RETAINED means the consumer retained and persisted the exact native
// obligation. This hook is independent of revocable presentation callbacks;
// the helper's preregistered kernel status carries final group settlement.
using Terminal_native_receipt_hook =
    std::function<Terminal_native_receipt_ack(const Terminal_native_receipt&)>;

} // namespace vnm_terminal
