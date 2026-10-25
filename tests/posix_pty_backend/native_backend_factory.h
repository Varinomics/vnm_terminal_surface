#pragma once

#include "vnm_terminal/internal/posix_pty_backend.h"

#include <functional>
#include <memory>

namespace vnm_terminal::test_helpers {

using Native_backend_factory = std::function<std::unique_ptr<internal::Terminal_backend>()>;

inline std::unique_ptr<internal::Terminal_backend> make_default_test_backend()
{
    return internal::make_posix_pty_backend();
}

} // namespace vnm_terminal::test_helpers
