#pragma once

#include "native_backend_factory.h"

namespace vnm_terminal::test_helpers {

int run_foreground_interrupt_tests(int argc, char** argv, const Native_backend_factory& make_backend);

} // namespace vnm_terminal::test_helpers
