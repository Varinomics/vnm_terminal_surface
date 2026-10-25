#include "helpers/test_check.h"
#include "posix_terminal_process_owner.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <unistd.h>

namespace {

using vnm_terminal::test_helpers::check;

constexpr int k_allowed_target = 42;
constexpr int k_high_minimum = 128;
constexpr rlim_t k_lowered_limit = 64;

class Owned_descriptor
{
public:
    explicit Owned_descriptor(int value = -1) : m_value(value) {}
    ~Owned_descriptor() { reset(); }
    Owned_descriptor(const Owned_descriptor&) = delete;
    Owned_descriptor& operator=(const Owned_descriptor&) = delete;

    int get() const { return m_value; }
    void reset(int value = -1)
    {
        if (m_value >= 0) {
            ::close(m_value);
        }
        m_value = value;
    }

private:
    int m_value;
};

bool descriptor_absent(int descriptor)
{
    errno = 0;
    return ::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF;
}

int verify_exec_child(const char* excluded_number, const char* source_number)
{
    bool excluded_valid = false;
    bool source_valid = false;
    const int excluded = QString::fromLocal8Bit(excluded_number).toInt(&excluded_valid);
    const int source = QString::fromLocal8Bit(source_number).toInt(&source_valid);
    if (!excluded_valid || !source_valid || excluded < k_high_minimum || source <= excluded) {
        return 2;
    }
    bool ok = check(descriptor_absent(excluded),
        "unlisted descriptor above the lowered allocation limit does not reach exec");
    ok &= check(descriptor_absent(source),
        "whitelisted source descriptor is closed after remapping to its declared target");
    ok &= check(::fcntl(k_allowed_target, F_GETFD) >= 0 &&
        ::write(k_allowed_target, "A", 1) == 1,
        "explicitly allowed descriptor remains usable at its declared target");
    return ok ? 0 : 1;
}

bool check_lowered_limit(const QString& executable, bool lower_hard_limit)
{
    rlimit original{};
    if (!check(::getrlimit(RLIMIT_NOFILE, &original) == 0,
        "isolated worker reads its descriptor allocation limit"))
    {
        return false;
    }
    if (original.rlim_cur <= k_high_minimum + 1) {
        auto raised = original;
        raised.rlim_cur = k_high_minimum + 2;
        if (!check((raised.rlim_max == RLIM_INFINITY || raised.rlim_cur <= raised.rlim_max) &&
            ::setrlimit(RLIMIT_NOFILE, &raised) == 0,
            "isolated worker can allocate descriptors above the later limit"))
        {
            return false;
        }
    }
    Owned_descriptor source(::open("/dev/null", O_RDONLY));
    Owned_descriptor excluded(::fcntl(source.get(), F_DUPFD, k_high_minimum));
    int channel[2] = {-1, -1};
    if (!check(source.get() >= 0 && excluded.get() >= k_high_minimum && ::pipe(channel) == 0,
        "isolated worker opens an excluded descriptor and allowed output pipe"))
    {
        return false;
    }
    Owned_descriptor acknowledgement(channel[0]);
    Owned_descriptor original_output(channel[1]);
    Owned_descriptor allowed(::fcntl(original_output.get(), F_DUPFD, excluded.get() + 1));
    original_output.reset();
    if (!check(allowed.get() > excluded.get(),
        "allowed source is also open above the later allocation limit"))
    {
        return false;
    }
    rlimit lowered{k_lowered_limit, lower_hard_limit ? k_lowered_limit : original.rlim_max};
    if (!check(::setrlimit(RLIMIT_NOFILE, &lowered) == 0,
        "only the disposable worker lowers its allocation limit"))
    {
        return false;
    }
    rlimit observed{};
    if (!check(::getrlimit(RLIMIT_NOFILE, &observed) == 0 &&
        observed.rlim_cur == k_lowered_limit &&
        (!lower_hard_limit || observed.rlim_max == k_lowered_limit) &&
        ::fcntl(excluded.get(), F_GETFD) == 0 && ::fcntl(allowed.get(), F_GETFD) == 0,
        "already-open inheritable descriptors remain above the lowered soft and optional hard limit"))
    {
        return false;
    }

    vnm_terminal::terminal_process_launch_t request;
    request.executable = QFile::encodeName(executable).toStdString();
    request.argv = {request.executable, "--verify-exec-child",
        std::to_string(excluded.get()), std::to_string(allowed.get())};
    for (const auto& variable : QProcessEnvironment::systemEnvironment().toStringList()) {
        request.environment.push_back(variable.toUtf8().toStdString());
    }
    request.inherited = {{allowed.get(), k_allowed_target}};
    request.rows = 24;
    request.columns = 80;
    auto owner = vnm_terminal::internal::make_posix_terminal_process_owner();
    const auto started = owner->launch(request);
    Owned_descriptor master(started.master_descriptor);
    bool ok = check(started.accepted && started.native_dispatch_occurred,
        "default owner launches with an explicit descriptor whitelist after limit reduction");
    if (!started.accepted) {
        std::fprintf(stderr, "native launch failed: %s\n", started.error.c_str());
        return false;
    }
    const auto exited = owner->wait_root();
    char diagnostics[1024];
    const auto count = ::read(master.get(), diagnostics, sizeof(diagnostics));
    if (count > 0) {
        std::fwrite(diagnostics, 1, static_cast<std::size_t>(count), stderr);
    }
    owner->finish();
    ok &= check(exited && exited->exit_code == 0 && exited->signal == 0 && owner->confirmed(),
        "exec verifier confirms descriptor isolation and the exact root is collected");
    pollfd readable{acknowledgement.get(), POLLIN, 0};
    char marker = 0;
    ok &= check(::poll(&readable, 1, 1000) == 1 &&
        ::read(acknowledgement.get(), &marker, 1) == 1 && marker == 'A',
        "declared descriptor carries the child's acknowledgement back to its original pipe");

    // A directory cannot be executed. This reaches native exec without relying
    // on a missing pathname or changing any files outside the disposable worker.
    auto rejected_owner = vnm_terminal::internal::make_posix_terminal_process_owner();
    request.executable = "/";
    request.argv = {"/"};
    const auto rejected = rejected_owner->launch(request);
    Owned_descriptor rejected_master(rejected.master_descriptor);
    ok &= check(!rejected.accepted && rejected.native_dispatch_occurred &&
        rejected.start_outcome_determinate && rejected.error.find("terminal exec:") != std::string::npos,
        "descriptor cleanup preserves the exec-error pipe and reports native exec rejection");
    rejected_owner->finish();
    ok &= check(rejected_owner->confirmed(),
        "failed exec retains and collects the exact original child after limit reduction");
    return ok;
}

bool run_isolated_worker(const QString& executable, const QString& mode)
{
    std::fprintf(stderr, "DESCRIPTOR_LIMIT case=%s\n", mode.toLocal8Bit().constData());
    QProcess worker;
    worker.setProcessChannelMode(QProcess::ForwardedChannels);
    worker.start(executable, {QStringLiteral("--limit-worker"), mode});
    if (!check(worker.waitForStarted(5000), "descriptor-limit worker starts in a fresh process")) {
        return false;
    }
    if (!worker.waitForFinished(15000)) {
        worker.kill();
        worker.waitForFinished(5000);
        return check(false, "descriptor-limit worker finishes within its bound");
    }
    return check(worker.exitStatus() == QProcess::NormalExit && worker.exitCode() == 0,
        "isolated descriptor-limit regression passes");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc == 4 && std::string_view(argv[1]) == "--verify-exec-child") {
        return verify_exec_child(argv[2], argv[3]);
    }
    if (argc == 3 && std::string_view(argv[1]) == "--limit-worker") {
        const std::string_view mode(argv[2]);
        if (mode != "soft" && mode != "hard") {
            return 2;
        }
        ::alarm(12);
        return check_lowered_limit(QCoreApplication::applicationFilePath(), mode == "hard") ? 0 : 1;
    }
    if (argc != 1) {
        return 2;
    }
    const auto executable = QCoreApplication::applicationFilePath();
    bool ok = run_isolated_worker(executable, QStringLiteral("soft"));
    ok &= run_isolated_worker(executable, QStringLiteral("hard"));
    return ok ? 0 : 1;
}
