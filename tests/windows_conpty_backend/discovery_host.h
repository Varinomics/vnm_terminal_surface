#pragma once

#include <optional>

// External measurement control is independent of terminal input and output.
std::optional<int> run_discovery_host(int argc, char** argv);
