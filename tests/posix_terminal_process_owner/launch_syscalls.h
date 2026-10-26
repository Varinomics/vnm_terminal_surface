#pragma once

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/event.h>
#include <sys/sysctl.h>
#include <util.h>
#else
#include <utmp.h>
#endif

extern "C" int owner_test_open(const char* path, int flags);
extern "C" int owner_test_posix_openpt(int flags);
extern "C" int owner_test_login_tty(int descriptor);
extern "C" pid_t owner_test_fork();
#if defined(__APPLE__)
extern "C" int owner_test_pipe(int descriptors[2]);
extern "C" int owner_test_kevent(int queue, const struct kevent* changes, int change_count,
    struct kevent* events, int event_count, const struct timespec* timeout);
extern "C" int owner_test_sysctl(int* name, u_int length, void* data, size_t* size,
    void* replacement, size_t replacement_size);
#define pipe owner_test_pipe
#define kevent(...) owner_test_kevent(__VA_ARGS__)
#define sysctl owner_test_sysctl
#else
extern "C" int owner_test_pipe2(int descriptors[2], int flags);
#define pipe2 owner_test_pipe2
#endif

// Only the separately compiled owner TU keeps these return-point seams. The
// fixture removes the macros and calls the real native operations itself.
#define open owner_test_open
#define posix_openpt owner_test_posix_openpt
#define login_tty owner_test_login_tty
#define fork owner_test_fork
