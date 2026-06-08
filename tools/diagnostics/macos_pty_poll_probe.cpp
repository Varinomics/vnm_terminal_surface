// Standalone diagnostic probe (TEMP).
//
// Characterizes, on the macOS CI host, the primitives our PTY backend relies on
// during normal operation and teardown, in the state our hung tests reach (the
// direct child has exited but a descendant still holds the PTY slave open):
//
//   E1  poll() POLLIN on the master with no data            -> spurious ready?
//   E2  write() to the master until EAGAIN                  -> does it ever block?
//   E2  poll() POLLOUT on the master                        -> spurious ready?
//   E7  poll()+read() spin rate on the master               -> how hard does it spin?
//   E5a poll() a wake pipe ALONE with a finite timeout      -> timeout honored?
//   E5b poll() a wake pipe ALONE, interrupted by a write    -> wake works?
//   E6a blocking waitpid() when the direct child exits 0    -> returns promptly?
//   E6b blocking waitpid() on a live child SIGKILLed by another thread -> returns?
//
// Every poll/waitpid logs a flushed "begin" line before and an "end" line with
// elapsed ms after. Run under `timeout`/alarm; a primitive that never honors its
// bound hangs and the last "begin" line names it.

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

#define LOG(...)                                    \
    do {                                            \
        std::fprintf(stderr, "PROBE " __VA_ARGS__); \
        std::fprintf(stderr, "\n");                 \
        std::fflush(stderr);                        \
    } while (0)

void set_nonblock(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

pid_t spawn_pty(int* master_out, const char* script)
{
    int master = -1;
    const pid_t pid = forkpty(&master, nullptr, nullptr, nullptr);
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", script, (char*)nullptr);
        _exit(127);
    }
    set_nonblock(master);
    *master_out = master;
    return pid;
}

}

int main()
{
    // Scenario for E1/E2/E7: direct child exited, descendant holds slave open.
    int master = -1;
    const pid_t pid = spawn_pty(&master, "(sleep 30) & echo desc=$!; exit 0");
    if (pid < 0) { LOG("forkpty failed errno=%d", errno); return 2; }

    int status = 0;
    for (int i = 0; i < 300; ++i) {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) { LOG("direct child reaped status=%#x", status); break; }
        if (r < 0)    { LOG("reap waitpid errno=%d", errno); break; }
        usleep(10 * 1000);
    }

    // E1: POLLIN on the master with no data pending.
    for (int i = 0; i < 2; ++i) {
        pollfd f[1] = { { master, POLLIN | POLLHUP | POLLERR, 0 } };
        const long long t0 = now_ms();
        const int r = ::poll(f, 1, 100);
        LOG("E1 pollin r=%d elapsed_ms=%lld revents=%#x (0x1=POLLIN spurious-if-no-data)",
            r, now_ms() - t0, (unsigned)f[0].revents);
    }

    // E2: does write() to the master ever block / EAGAIN; is POLLOUT spurious?
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
    {
        pollfd f[1] = { { master, POLLOUT | POLLHUP | POLLERR, 0 } };
        const long long t0 = now_ms();
        const int r = ::poll(f, 1, 100);
        LOG("E2 pollout r=%d elapsed_ms=%lld revents=%#x", r, now_ms() - t0, (unsigned)f[0].revents);
    }

    // E7: how fast does poll(POLLIN)+read(EAGAIN) spin over ~300ms?
    {
        const long long t0 = now_ms();
        long iters = 0, eagain = 0, got = 0;
        while (now_ms() - t0 < 300) {
            pollfd f[1] = { { master, POLLIN | POLLHUP | POLLERR, 0 } };
            (void)::poll(f, 1, 100);
            char rb[256];
            const ssize_t rd = read(master, rb, sizeof rb);
            ++iters;
            if (rd > 0) ++got;
            else if (rd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) ++eagain;
        }
        LOG("E7 spin over_300ms: iters=%ld eagain=%ld got_data=%ld (high iters => busy spin, no blocking)",
            iters, eagain, got);
    }
    close(master);

    // E5: an ORDINARY pipe (the wake pipe), polled alone.
    int wk[2] = {-1, -1};
    if (pipe(wk) != 0) { LOG("pipe failed errno=%d", errno); return 2; }
    set_nonblock(wk[0]);
    set_nonblock(wk[1]);

    // E5a: finite timeout honored on a regular pipe? (expect ~100ms, r=0)
    {
        pollfd f[1] = { { wk[0], POLLIN, 0 } };
        const long long t0 = now_ms();
        const int r = ::poll(f, 1, 100);
        LOG("E5a wake-only poll r=%d elapsed_ms=%lld (expect r=0 elapsed~100)", r, now_ms() - t0);
    }
    // E5b: does a write to the wake pipe interrupt poll()? (expect ~200ms, r=1)
    {
        std::thread waker([&]{
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const char b = 1;
            const ssize_t w = write(wk[1], &b, 1);
            LOG("E5b wake-write ret=%zd errno=%d", w, (w < 0 ? errno : 0));
        });
        pollfd f[1] = { { wk[0], POLLIN, 0 } };
        const long long t0 = now_ms();
        const int r = ::poll(f, 1, 5000);
        LOG("E5b wake-only poll r=%d elapsed_ms=%lld revents=%#x (expect r=1 elapsed~200)",
            r, now_ms() - t0, (unsigned)f[0].revents);
        waker.join();
        char drain[64];
        while (read(wk[0], drain, sizeof drain) > 0) {}
    }
    close(wk[0]);
    close(wk[1]);

    // E6a: blocking waitpid() when the direct child exits 0 (descendant alive).
    {
        int m = -1;
        const pid_t p = spawn_pty(&m, "(sleep 30) & exit 0");
        if (p > 0) {
            LOG("E6a blocking waitpid begin (expect prompt return)");
            const long long t0 = now_ms();
            int st = 0;
            const pid_t r = waitpid(p, &st, 0);
            LOG("E6a waitpid end r=%d elapsed_ms=%lld status=%#x errno=%d",
                (int)r, now_ms() - t0, st, (r < 0 ? errno : 0));
            kill(-p, SIGKILL);
            kill(p, SIGKILL);
            if (m >= 0) close(m);
        }
    }

    // E6b: blocking waitpid() on a live child SIGKILLed by another thread @200ms.
    {
        int m = -1;
        const pid_t p = spawn_pty(&m, "exec sleep 30");
        if (p > 0) {
            std::thread killer([&]{
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                LOG("E6b kill(%d, SIGKILL)", (int)p);
                kill(p, SIGKILL);
            });
            LOG("E6b blocking waitpid begin (expect ~200ms after the kill)");
            const long long t0 = now_ms();
            int st = 0;
            const pid_t r = waitpid(p, &st, 0);
            LOG("E6b waitpid end r=%d elapsed_ms=%lld status=%#x errno=%d",
                (int)r, now_ms() - t0, st, (r < 0 ? errno : 0));
            killer.join();
            if (m >= 0) close(m);
        }
    }

    LOG("done");
    return 0;
}
