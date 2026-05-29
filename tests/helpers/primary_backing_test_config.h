#pragma once

#include "vnm_terminal/internal/session_contract.h"
#include "vnm_terminal/internal/terminal_screen_model.h"

namespace vnm_terminal::test_helpers {

inline internal::Terminal_session_config recovery_disabled_primary_backing_session_config(
    internal::Terminal_session_config config = {})
{
    config.recover_scrollback_from_primary_repaints = false;
    return config;
}

inline internal::Terminal_screen_model_config recovery_disabled_primary_backing_screen_model_config(
    internal::Terminal_screen_model_config config = {})
{
    config.recover_scrollback_from_primary_repaints = false;
    return config;
}

} // namespace vnm_terminal::test_helpers
