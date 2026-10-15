#pragma once

#include "vnm_terminal/internal/backend_contract.h"

#if defined(__APPLE__)
#include <vnm_process_custody/owner.h>
#include <poll.h>
#include <sys/wait.h>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vnm_terminal::test_helpers {

// The test consumer keeps the exact native obligations independently of
// presentation callbacks, including backend destruction from a callback.
class Native_receipt_capture
{
public:
    Terminal_native_receipt_ack retain(const Terminal_native_receipt& receipt)
    {
        const std::lock_guard lock(m_mutex);
        std::string error;
        if (receipt.kind == Terminal_native_receipt_kind::HELPER_PRE_START) {
            auto observation = std::make_unique<Observation>();
            observation->identity = receipt;
            if (!observation->helper.open(receipt.helper_pid, receipt.helper_birth_token, &error)) {
                return Terminal_native_receipt_ack::REJECTED;
            }
            observation->receipts.push_back(receipt);
            m_observations.push_back(std::move(observation));
            m_changed.notify_all();
            return Terminal_native_receipt_ack::RETAINED;
        }
        for (const auto& observation : m_observations) {
            if (observation->identity.helper_pid != receipt.helper_pid ||
                observation->identity.helper_birth_token != receipt.helper_birth_token)
            {
                continue;
            }
            observation->receipts.push_back(receipt);
            m_changed.notify_all();
            if (receipt.kind == Terminal_native_receipt_kind::ROOT_BORN) {
                observation->identity.root_pid         = receipt.root_pid;
                observation->identity.root_birth_token = receipt.root_birth_token;
                observation->root = vnm::process_custody::open_process_observer(
                    receipt.root_pid, receipt.root_birth_token, &error);
                if (observation->root != vnm::process_custody::k_invalid_handle) {
                    return Terminal_native_receipt_ack::RETAINED;
                }
                // Rapid exit can precede local registration. Only the exact
                // subsequent ROOT_ENDED receipt can discharge that obligation.
                return Terminal_native_receipt_ack::REJECTED;
            }
            if (receipt.kind == Terminal_native_receipt_kind::ROOT_ENDED &&
                receipt.root_pid > 1 && receipt.root_birth_token != 0 &&
                receipt.root_pid == observation->identity.root_pid &&
                receipt.root_birth_token == observation->identity.root_birth_token)
            {
                return Terminal_native_receipt_ack::RETAINED;
            }
            if (receipt.kind == Terminal_native_receipt_kind::NO_BIRTH &&
                observation->identity.root_pid == 0)
            {
                return Terminal_native_receipt_ack::RETAINED;
            }
            return Terminal_native_receipt_ack::REJECTED;
        }
        return Terminal_native_receipt_ack::REJECTED;
    }

    std::vector<Terminal_native_receipt> receipts()
    {
        const std::lock_guard lock(m_mutex);
        return m_observations.empty() ? std::vector<Terminal_native_receipt>{}
                                     : m_observations.back()->receipts;
    }

    bool wait_for_receipts(std::size_t count)
    {
        std::unique_lock lock(m_mutex);
        return m_changed.wait_for(lock, std::chrono::seconds(5), [&] {
            return !m_observations.empty() && m_observations.back()->receipts.size() >= count;
        });
    }

    vnm::process_custody::Native_process_status wait_for_helper()
    {
        vnm::process_custody::Native_process_status_observer* observer;
        {
            const std::lock_guard lock(m_mutex);
            if (m_observations.empty()) {
                return {};
            }
            observer = &m_observations.back()->helper;
        }
        pollfd event{observer->native_handle(), POLLIN, 0};
        int ready;
        do {
            ready = ::poll(&event, 1, 5000);
        }
        while (ready < 0 && errno == EINTR);
        const std::lock_guard lock(m_mutex);
        return observer->observe();
    }

    bool helper_exited(vnm::process_custody::Darwin_pty_owner_exit expected)
    {
        const auto status = wait_for_helper();
        return status.state == vnm::process_custody::Native_process_observation::ENDED &&
            status.status_valid && WIFEXITED(status.wait_status) &&
            WEXITSTATUS(status.wait_status) == static_cast<int>(expected);
    }

private:
    struct Observation
    {
        ~Observation() { vnm::process_custody::close_native_handle(root); }

        Terminal_native_receipt identity;
        vnm::process_custody::Native_process_status_observer helper;
        vnm::process_custody::Native_handle root = vnm::process_custody::k_invalid_handle;
        std::vector<Terminal_native_receipt> receipts;
    };

    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<std::unique_ptr<Observation>> m_observations;
};

inline std::shared_ptr<Native_receipt_capture> retain_native_receipts(internal::Terminal_launch_config& config)
{
    auto capture = std::make_shared<Native_receipt_capture>();
    config.native_receipt_hook = [capture](const Terminal_native_receipt& receipt) {
        return capture->retain(receipt);
    };
    return capture;
}

} // namespace vnm_terminal::test_helpers
#endif
