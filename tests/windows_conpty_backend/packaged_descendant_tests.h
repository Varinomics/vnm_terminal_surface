#pragma once

#include <optional>

// Fixture dispatch runs before the ordinary backend test arguments are parsed.
std::optional<int> run_packaged_descendant_tests(int argc, char** argv);
