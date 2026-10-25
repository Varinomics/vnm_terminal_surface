#pragma once

#include <vnm_terminal/terminal_process_owner.h>

#include <memory>

namespace vnm_terminal::internal {

std::unique_ptr<Terminal_process_owner> make_posix_terminal_process_owner();

} // namespace vnm_terminal::internal
