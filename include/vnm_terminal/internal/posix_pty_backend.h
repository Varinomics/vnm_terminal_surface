#pragma once

#include "vnm_terminal/internal/backend_contract.h"
#include <memory>

namespace vnm_terminal::internal {

#if defined(__linux__) || defined(__APPLE__)

class Posix_pty_backend final : public Terminal_backend
{
public:
    Posix_pty_backend();
    ~Posix_pty_backend() override;

    Posix_pty_backend(const Posix_pty_backend&)            = delete;
    Posix_pty_backend& operator=(const Posix_pty_backend&) = delete;

    Terminal_backend_result start(
        const Terminal_launch_config&   config,
        Terminal_backend_callbacks      callbacks) override;

    Terminal_backend_result write(
        QByteArray                      bytes) override;

    Terminal_backend_result resize(
        Terminal_backend_resize_request request) override;

    Terminal_backend_result set_output_paused(
        bool                            paused) override;

    Terminal_backend_result interrupt() override;
    Terminal_backend_result terminate() override;

private:
    class Impl;

    std::unique_ptr<Impl> m_impl;
};

std::unique_ptr<Terminal_backend> make_posix_pty_backend();

#endif

}
