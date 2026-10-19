#include "helpers/test_check.h"
#include "native_receipt_capture.h"
#include "vnm_terminal/internal/posix_pty_backend.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QString>
#include <QTemporaryDir>
#include <unistd.h>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

bool write_executable(const QString& path, const QByteArray& marker)
{
    QFile file(path);
    if (!check(file.open(QIODevice::WriteOnly), "lookup fixture script can be opened")) {
        return false;
    }
    const QByteArray contents = QByteArrayLiteral("#!/bin/sh\nprintf '") + marker +
        QByteArrayLiteral("\\n'\n");
    if (!check(file.write(contents) == contents.size(), "lookup fixture script is written")) {
        return false;
    }
    file.close();
    return check(file.setPermissions(
        QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
        "lookup fixture script is executable");
}

bool check_launch(
    const char*     name,
    const QString& executable,
    const QString& working_directory,
    const QString& path)
{
    std::mutex mutex;
    std::condition_variable completed;
    QByteArray output;
    std::optional<term::Terminal_backend_exit> exit;
    std::vector<term::Terminal_backend_error> errors;
    term::Terminal_backend_callbacks callbacks;
    callbacks.output_received = [
            &mutex,
            &output
        ](
            QByteArray bytes)
        {
            std::lock_guard lock(mutex);
            output.append(bytes);
        };
    callbacks.process_exited = [
            &mutex,
            &completed,
            &exit
        ](
            term::Terminal_backend_exit result)
        {
            std::lock_guard lock(mutex);
            exit = result;
            completed.notify_all();
        };
    callbacks.error_reported = [
            &mutex,
            &errors
        ](
            term::Terminal_backend_error error)
        {
            std::lock_guard lock(mutex);
            errors.push_back(std::move(error));
        };

    term::Terminal_launch_config config;
    config.argv               = {executable};
    config.working_directory  = working_directory;
    config.initial_grid_size  = term::terminal_grid_size_t{24, 80};
    config.environment_edits.push_back({
        term::Terminal_environment_operation::SET, QStringLiteral("PATH"), path});
#if defined(__APPLE__)
    vnm_terminal::test_helpers::retain_native_receipts(config);
#endif
    auto backend = term::make_posix_pty_backend();
    const auto started = backend->start(config, std::move(callbacks));
    const std::string label(name);
    if (!check(started.code == term::Terminal_backend_result_code::ACCEPTED,
        label + " starts"))
    {
        if (started.error.has_value()) {
            std::cerr << started.error->message.toStdString() << '\n';
        }
        return false;
    }

    bool exited = false;
    {
        std::unique_lock lock(mutex);
        exited = completed.wait_for(lock, std::chrono::seconds(10), [&exit] {
            return exit.has_value();
        });
    }
    backend.reset();
    bool ok = check(exited && exit->exit_code == 0, label + " exits normally");
    ok &= check(errors.empty(), label + " has no backend errors");
    ok &= check(output == QByteArrayLiteral("EXPECTED\r\n"),
        label + " launches the executable selected by native path traversal");
    return ok;
}

}

int main()
{
    QTemporaryDir directory;
    if (!check(directory.isValid(), "executable lookup fixture directory is available")) {
        return 1;
    }
    const QDir root(directory.path());
    if (!check(root.mkpath(QStringLiteral("base"))             &&
            root.mkpath(QStringLiteral("target/deep"))         &&
            root.mkpath(QStringLiteral("first/lookup_runner")) &&
            root.mkpath(QStringLiteral("second")),
        "executable lookup fixture directories are available"))
    {
        return 1;
    }
    if (!write_executable(root.filePath(QStringLiteral("base/lookup_runner")), "WRONG") ||
        !write_executable(root.filePath(QStringLiteral("target/lookup_runner")), "EXPECTED") ||
        !write_executable(root.filePath(QStringLiteral("second/lookup_runner")), "EXPECTED"))
    {
        return 1;
    }
    const QByteArray link_path = QFile::encodeName(root.filePath(QStringLiteral("base/link")));
    if (!check(::symlink("../target/deep", link_path.constData()) == 0,
        "lookup fixture symlink is created"))
    {
        return 1;
    }

    bool ok = true;
    const QString traversal = QStringLiteral("base/link/../lookup_runner");
    ok &= check_launch("absolute symlink/.. executable",
        root.path() + QChar(u'/') + traversal, root.path(), QStringLiteral("/usr/bin:/bin"));
    ok &= check_launch("relative symlink/.. executable",
        traversal, root.path(), QStringLiteral("/usr/bin:/bin"));
    ok &= check_launch("symlink/.. PATH entry",
        QStringLiteral("lookup_runner"), root.path(), QStringLiteral("base/link/.."));
    ok &= check_launch("symlink/.. working directory",
        QStringLiteral("./lookup_runner"), root.path() + QStringLiteral("/base/link/.."),
        QStringLiteral("/usr/bin:/bin"));
    ok &= check_launch("directory shadowing a later executable in PATH",
        QStringLiteral("lookup_runner"), root.path(), QStringLiteral("first:second"));
    ok &= check_launch("empty PATH component uses the working directory",
        QStringLiteral("lookup_runner"), root.filePath(QStringLiteral("target")),
        QStringLiteral(":/usr/bin:/bin"));
    return ok ? 0 : 1;
}
