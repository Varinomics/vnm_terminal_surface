#include "process_spawn_common_posix.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <variant>

namespace vnm::process_custody {
namespace detail {

bool prepare_spawn(const Spawn_request& request, Prepared_spawn& prepared, std::string& error)
{
    if (request.inherited.size() > 64) {
        error = "spawn: more than 64 inherited descriptors";
        return false;
    }

    // Everything the child needs is materialized here, before fork. Typed
    // inherited arguments resolve from the same descriptor table that dup2
    // applies, so argv cannot name the parent's descriptor by accident.
    prepared.resolved_arguments.reserve(request.argv.size());
    for (const Spawn_argument& argument : request.argv) {
        if (const auto* text = std::get_if<std::string>(&argument.value)) {
            prepared.resolved_arguments.push_back(*text);
            continue;
        }
        const std::size_t index = std::get<Inherited_reference>(argument.value).inherited_index;
        if (index >= request.inherited.size()) {
            error = "spawn: inherited argv reference is out of range";
            return false;
        }
        prepared.resolved_arguments.push_back(std::to_string(request.inherited[index].child_number));
    }

    prepared.argv.reserve(prepared.resolved_arguments.size() + 1);
    for (const std::string& argument : prepared.resolved_arguments) {
        prepared.argv.push_back(const_cast<char*>(argument.c_str()));
    }
    prepared.argv.push_back(nullptr);

    prepared.envp.reserve(request.environment.size() + 1);
    for (const std::string& entry : request.environment) {
        prepared.envp.push_back(const_cast<char*>(entry.c_str()));
    }
    prepared.envp.push_back(nullptr);

    prepared.child_handles.reserve(request.inherited.size());
    for (std::size_t i = 0; i < request.inherited.size(); ++i) {
        const Inherited_descriptor& descriptor = request.inherited[i];
        if (descriptor.parent_handle < 0 || descriptor.child_number < 0 ||
            descriptor.child_number > std::numeric_limits<int>::max() - 65)
        {
            error = "spawn: inherited descriptor entry is not a valid descriptor/number pair";
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (prepared.targets[j] == descriptor.child_number) {
                error = "spawn: two inherited descriptors share child number " +
                    std::to_string(descriptor.child_number);
                return false;
            }
        }
        prepared.sources[i]     = descriptor.parent_handle;
        prepared.targets[i]     = descriptor.child_number;
        prepared.child_handles.push_back(descriptor.child_number);
        prepared.highest_target = std::max(prepared.highest_target, descriptor.child_number);
    }

    return true;
}

} // namespace detail

Inherited_reference inherited_argument(std::size_t in_inherited_index)
{
    return Inherited_reference{in_inherited_index};
}

std::vector<Inherited_descriptor> inherit_standard_streams()
{
    std::vector<Inherited_descriptor> streams;
    for (int fd = 0; fd <= 2; ++fd) {
        if (fcntl(fd, F_GETFD) != -1) {
            streams.push_back(Inherited_descriptor{fd, fd});
        }
    }
    return streams;
}

bool prepare_parent_process(std::string* out_error)
{
    struct sigaction action{};
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGPIPE, &action, nullptr) != 0) {
        *out_error = std::string("sigaction(SIGPIPE): ") + std::strerror(errno);
        return false;
    }
    return true;
}

} // namespace vnm::process_custody
