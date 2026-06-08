// Standalone diagnostic probe (TEMP).
//
// Question it answers on the macOS CI host: in the exact state our hung tests
// reach -- the direct child has exited but a descendant still holds the PTY
// slave open, so the master is non-blocking and neither readable, writable, nor
// hung up -- does poll() on the PTY master honor a finite timeout, does a write
// to a wake pipe interrupt it, and does close()ing the master wake it?
//
// Each experiment logs a "begin" line (flushed) before the poll and an "end"
// line with elapsed time after. Run it under `timeout`; if a poll never honors
// its timeout the process hangs and the last "begin" line names the culprit.

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

namespace {

long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

#define LOG(...)                                  \
    do {                                          \
        std::fprintf(stderr, "PROBE " __VA_ARGS__); \
        std::fprintf(stderr, "\n");               \
        std::fflush(stderr);                      \
    } while (0)

void set_nonblock(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

}

int main()
{
    int master = -1;
    const pid_t pid = forkpty(&master, nullptr, nullptr, nullptr);
    if (pid < 0) {
        LOG("forkpty failed errno=%d", errno);
        return 2;
    }
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c",
              "(sleep 30) & echo desc=$!; exit 0", (char*)nullptr);
        _exit(127);
    }

    set_nonblock(master);

    // Reach the "child exited, descendant holds slave" state.
    int status = 0;
    for (int i = 0; i < 300; ++i) {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) { LOG("direct child reaped status=%#x", status); break; }
        if (r < 0)    { LOG("waitpid errno=%d", errno); break; }
        usleep(10 * 1000);
    }

    int wake[2] = {-1, -1};
    if (pipe(wake) != 0) { LOG("pipe failed errno=%d", errno); return 2; }
    set_nonblock(wake[0]);
    set_nonblock(wake[1]);

    // E1: finite-timeout POLLIN on the master (no data, slave held open).
    for (int i = 0; i < 2; ++i) {
        pollfd f[1] = { { master, POLLIN | POLLHUP | POLLERR, 0 } };
        LOG("E1 pollin begin timeout=100 iter=%d", i);
        const long long t0 = now_ms();
        const int r = ::poll(f, 1, 100);
        LOG("E1 pollin end r=%d elapsed_ms=%lld errno=%d revents=%#x",
            r, now_ms() - t0, (r < 0 ? errno : 0), (unsigned)f[0].revents);
    }

    // E2: fill the master until EAGAIN, then finite-timeout POLLOUT.
    {
        char buf[4096];
        memset(buf, 'x', sizeof buf);
        size_t total = 0;
        ssize_t n = 0;
        while ((n = write(master, buf, sizeof buf)) > 0) {
            total += (size_t)n;
            if (total > 16u * 1024u * 1024u) { LOG("E2 fill cap (no EAGAIN) total=%zu", total); break; }
        }
        LOG("E2 master-full total=%zu last_n=%zd errno=%d", total, n, (n < 0 ? errno : 0));
    }
    for (int i = 0; i < 2; ++i) {
        pollfd f[1] = { { master, POLLOUT | POLLHUP | POLLERR, 0 } };
        LOG("E2 pollout begin timeout=100 iter=%d", i);
        const long long t0 = now_ms();
        const int r = ::poll(f, 1, 100);
        LOG("E2 pollout end r=%d elapsed_ms=%lld errno=%d revents=%#x",
            r, now_ms() - t0, (r < 0 ? errno : 0), (unsigned)f[0].revents);
    }

    // E3: does a wake-pipe write interrupt poll({master POLLOUT, wake})?
    {
        std::thread waker([&]{
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const char b = 1;
            const ssize_t w = write(wake[1], &b, 1);
            LOG("E3 wake-write ret=%zd errno=%d", w, (w < 0 ? errno : 0));
        });
        pollfd f[2] = { { master, POLLOUT | POLLHUP | POLLERR, 0 }, { wake[0], POLLIN, 0 } };
        LOG("E3 pollout+wake begin timeout=5000 (expect ~200ms if wake interrupts)");
        const long long t0 = now_ms();
        const int r = ::poll(f, 2, 5000);
        LOG("E3 pollout+wake end r=%d elapsed_ms=%lld revents_master=%#x revents_wake=%#x",
            r, now_ms() - t0, (unsigned)f[0].revents, (unsigned)f[1].revents);
        waker.join();
        char drain[64];
        while (read(wake[0], drain, sizeof drain) > 0) {}
    }

    // E4: does close(master) from another thread wake poll({master POLLIN, wake})?
    {
        std::thread closer([&]{
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            LOG("E4 closing master");
            close(master);
        });
        pollfd f[2] = { { master, POLLIN | POLLHUP | POLLERR, 0 }, { wake[0], POLLIN, 0 } };
        LOG("E4 pollin-after-close begin timeout=5000 (expect ~200ms if close wakes)");
        const long long t0 = now_ms();
        const int r = ::poll(f, 2, 5000);
        LOG("E4 pollin-after-close end r=%d elapsed_ms=%lld revents_master=%#x",
            r, now_ms() - t0, (unsigned)f[0].revents);
        closer.join();
    }

    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    LOG("done");
    return 0;
}
