#pragma once

#if defined(__linux__) || defined(__APPLE__)

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace vnm_terminal::internal {

inline int create_terminal_pipe(int (&descriptors)[2], bool nonblocking) noexcept
{
#if defined(__linux__)
    return ::pipe2(descriptors, O_CLOEXEC | (nonblocking ? O_NONBLOCK : 0)) == 0 ? 0 : errno;
#else
    // Darwin's public pipe API has no atomic close-on-exec option. Setting
    // the flags here does not serialize an embedding host's independent spawn.
    if (::pipe(descriptors) < 0) {
        return errno;
    }
    int error = 0;
    for (const int descriptor : descriptors) {
        if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0 ||
            (nonblocking && ::fcntl(descriptor, F_SETFL, O_NONBLOCK) < 0))
        {
            error = errno;
            break;
        }
    }
    if (error != 0) {
        for (int& descriptor : descriptors) {
            ::close(descriptor);
            descriptor = -1;
        }
    }
    return error;
#endif
}

} // namespace vnm_terminal::internal

#endif
