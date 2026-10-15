#include "helpers/test_check.h"
#include "native_receipt_capture.h"
#include "vnm_terminal/internal/posix_pty_backend.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

namespace term = vnm_terminal::internal;
namespace custody = vnm::process_custody;

namespace {

using vnm_terminal::test_helpers::check;
using vnm_terminal::test_helpers::retain_native_receipts;
using Kind = vnm_terminal::Terminal_native_receipt_kind;
using Ack = vnm_terminal::Terminal_native_receipt_ack;

term::Terminal_backend_callbacks callbacks()
{
    term::Terminal_backend_callbacks result;
    result.output_received = [](QByteArray) {};
    result.process_exited  = [](term::Terminal_backend_exit) {};
    result.error_reported  = [](term::Terminal_backend_error) {};
    return result;
}

term::Terminal_launch_config shell_config(const QString& command)
{
    term::Terminal_launch_config config;
    config.argv = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), command};
    config.working_directory = QStringLiteral("/tmp");
    config.initial_grid_size = term::terminal_grid_size_t{24, 80};
    return config;
}

bool no_ack_prevents_workload(bool missing_hook)
{
    QTemporaryDir directory;
    if (!check(directory.isValid(), "pre-START admission fixture directory exists")) {
        return false;
    }
    const auto marker = directory.filePath(QStringLiteral("workload-ran"));
    auto config = shell_config(QStringLiteral("printf started > '") + marker + QStringLiteral("'"));
    std::shared_ptr<vnm_terminal::test_helpers::Native_receipt_capture> receipts;
    if (!missing_hook) {
        receipts = retain_native_receipts(config);
        config.native_receipt_hook = [receipts](const vnm_terminal::Terminal_native_receipt& receipt) {
            const auto retained = receipts->retain(receipt);
            return receipt.kind == Kind::HELPER_PRE_START ? Ack::REJECTED : retained;
        };
    }
    auto backend = term::make_posix_pty_backend();
    const auto result = backend->start(config, callbacks());
    bool ok = check(result.code == term::Terminal_backend_result_code::REJECTED && result.error &&
        result.error->code == term::Terminal_backend_error_code::START_FAILED,
        "missing or rejected pre-START acknowledgement reports native startup failure");
    backend.reset();
    if (receipts) {
        ok &= check(receipts->helper_exited(custody::Darwin_pty_owner_exit::NO_WORKLOAD),
            "withheld START is confirmed by the exact helper's typed no-workload status");
        const auto retained = receipts->receipts();
        ok &= check(retained.size() == 2 && retained[0].kind == Kind::HELPER_PRE_START &&
            retained[1].kind == Kind::NO_BIRTH && retained[1].root_pid == 0,
            "no acknowledgement yields helper then no-birth receipts without a root");
    }
    ok &= check(!QFileInfo::exists(marker), "unacknowledged startup executes no workload command");
    return ok;
}

bool root_receipts(bool failed_exec)
{
    QTemporaryDir directory;
    if (!check(directory.isValid(), "root receipt fixture directory exists")) {
        return false;
    }
    auto config = shell_config(QStringLiteral("exit 23"));
    if (failed_exec) {
        const auto executable_path = directory.filePath(QStringLiteral("unavailable-interpreter"));
        QFile executable(executable_path);
        const auto contents = QByteArrayLiteral("#!") +
            QFile::encodeName(directory.filePath(QStringLiteral("missing-interpreter"))) + QByteArrayLiteral("\n");
        if (!check(executable.open(QIODevice::WriteOnly) && executable.write(contents) == contents.size() &&
            executable.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner),
            "failed exec fixture reaches native birth with an unavailable interpreter"))
        {
            return false;
        }
        executable.close();
        config.argv = {executable_path};
    }
    const auto receipts = retain_native_receipts(config);
    auto backend = term::make_posix_pty_backend();
    const auto result = backend->start(config, callbacks());
    bool ok = check(result.code == (failed_exec ? term::Terminal_backend_result_code::REJECTED
                                               : term::Terminal_backend_result_code::ACCEPTED),
        "rapid exit starts and native exec failure rejects through the production backend");
    ok &= check(receipts->wait_for_receipts(3), "exact root exit is retained even after rapid death");
    backend.reset();
    ok &= check(receipts->helper_exited(custody::Darwin_pty_owner_exit::CONFIRMED),
        "admitted root birth settles with typed confirmed status rather than no-workload");
    const auto retained = receipts->receipts();
    ok &= check(retained.size() == 3 && retained[0].kind == Kind::HELPER_PRE_START &&
        retained[1].kind == Kind::ROOT_BORN && retained[2].kind == Kind::ROOT_ENDED &&
        retained[1].root_pid > 1 && retained[1].root_birth_token != 0 &&
        retained[2].root_pid == retained[1].root_pid &&
        retained[2].root_birth_token == retained[1].root_birth_token &&
        retained[2].root_exit_code == (failed_exec ? 127 : 23) && retained[2].root_signal == 0,
        "birth precedes the original root's exact collected exit and never becomes no-birth");
    return ok;
}

} // namespace

int main()
{
    bool ok = no_ack_prevents_workload(true);
    ok &= no_ack_prevents_workload(false);
    ok &= root_receipts(false);
    ok &= root_receipts(true);
    return ok ? 0 : 1;
}
