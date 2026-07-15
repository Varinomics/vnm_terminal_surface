#include "helpers/test_check.h"
#include "vnm_terminal/backend_output_capture.h"
#include "vnm_terminal/internal/backend_output_capture_writer.h"
#include "vnm_terminal/internal/terminal_session.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::Backend_output_capture_config;
using vnm_terminal::Backend_output_capture_artifact_kind;
using vnm_terminal::Backend_output_capture_recovery;
using vnm_terminal::Backend_output_capture_status;
using vnm_terminal::test_helpers::check;
using term::Backend_output_capture_test_fault;

QString segment_path(
    const Backend_output_capture_config& config,
    std::uint64_t                        sequence)
{
    return QStringLiteral("%1.vnm-segment-%2.raw")
        .arg(config.base_path)
        .arg(sequence, 20, 10, QLatin1Char('0'));
}

QByteArray recovered_bytes(const Backend_output_capture_recovery& recovery)
{
    QByteArray bytes;
    for (const vnm_terminal::Backend_output_capture_segment& segment : recovery.segments) {
        QFile file(segment.path);
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        bytes += file.readAll();
    }
    return bytes;
}

bool write_crash_left_segment(
    const Backend_output_capture_config& config,
    std::uint64_t                        sequence,
    QByteArrayView                       bytes)
{
    QFile file(segment_path(config, sequence));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        return false;
    }

    const qint64 requested_bytes = static_cast<qint64>(bytes.size());
    return file.write(bytes.data(), requested_bytes) == requested_bytes && file.flush();
}

bool write_bytes(
    const QString& path,
    QByteArrayView bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        return false;
    }

    const qint64 requested_bytes = static_cast<qint64>(bytes.size());
    return file.write(bytes.data(), requested_bytes) == requested_bytes && file.flush();
}

class Output_backend final : public term::Terminal_backend
{
public:
    term::Terminal_backend_result start(
        const term::Terminal_launch_config& config,
        term::Terminal_backend_callbacks    callbacks) override
    {
        const term::Terminal_backend_result callback_result =
            term::validate_backend_callbacks(callbacks);
        if (term::is_backend_rejection(callback_result)) {
            return callback_result;
        }

        const term::Terminal_backend_result config_result =
            term::validate_launch_config(config);
        if (term::is_backend_rejection(config_result)) {
            return config_result;
        }

        m_callbacks = std::move(callbacks);
        m_running   = true;
        return term::backend_accept();
    }

    term::Terminal_backend_result write(QByteArray) override
    {
        return m_running
            ? term::backend_accept()
            : term::backend_reject(
                term::Terminal_backend_error_code::WRITE_FAILED,
                QStringLiteral("output backend is not running"));
    }

    term::Terminal_backend_result resize(term::Terminal_backend_resize_request) override
    {
        return term::backend_accept();
    }

    term::Terminal_backend_result set_output_paused(bool paused) override
    {
        m_paused = paused;
        return term::backend_accept();
    }

    term::Terminal_backend_result interrupt() override
    {
        return terminate();
    }

    term::Terminal_backend_result terminate() override
    {
        if (m_running) {
            m_running = false;
            m_callbacks.process_exited({term::Terminal_exit_reason::TERMINATED, 0});
        }
        return term::backend_accept();
    }

    bool emit_output(QByteArray bytes)
    {
        if (!m_running || m_paused) {
            return false;
        }

        m_callbacks.output_received(std::move(bytes));
        return true;
    }

    bool emit_error(QString message)
    {
        if (!m_running) {
            return false;
        }

        m_callbacks.error_reported({
            term::Terminal_backend_error_code::WRITE_FAILED,
            std::move(message),
        });
        return true;
    }

    bool emit_in_flight_output(QByteArray bytes)
    {
        if (!m_running) {
            return false;
        }

        m_callbacks.output_received(std::move(bytes));
        return true;
    }

private:
    term::Terminal_backend_callbacks m_callbacks;
    bool                             m_running = false;
    bool                             m_paused  = false;
};

term::Terminal_launch_config valid_launch_config()
{
    term::Terminal_launch_config config;
    config.argv              = {QStringLiteral("capture-test")};
    config.working_directory = QStringLiteral("C:/capture-test");
    config.initial_grid_size = term::terminal_grid_size_t{24, 80};
    return config;
}

bool test_capture_is_disabled_by_default()
{
    bool ok = true;

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(), "disabled capture temp dir is valid");

    term::Terminal_session_config config;
    config.trace_output_chunk_limit = 2U;
    ok &= check(!config.backend_output_capture_config.has_value(),
        "backend capture configuration is absent by default");

    auto backend = std::make_unique<Output_backend>();
    Output_backend* backend_ptr = backend.get();
    term::Terminal_session session(std::move(backend), config);
    ok &= check(session.start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "disabled capture session starts");
    ok &= check(backend_ptr->emit_output(QByteArrayLiteral("disabled-output")),
        "disabled capture session accepts output");
    ok &= check(session.output_chunks() ==
        std::vector<QByteArray>{QByteArrayLiteral("disabled-output")},
        "disabled capture leaves normal backend output processing intact");
    ok &= check(QDir(temp_dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
        "disabled capture creates no files or directories");

    return ok;
}

bool test_capture_location_validation_rejects_unsafe_inputs()
{
    bool ok = true;

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(), "capture validation temp dir is valid");

    const auto expect_rejected = [&ok](
        const Backend_output_capture_config& config,
        const QString&                       expected_error,
        const char*                          description)
    {
        const Backend_output_capture_recovery recovery =
            vnm_terminal::recover_backend_output_capture(config);
        ok &= check(
            recovery.status == Backend_output_capture_status::INVALID &&
                recovery.error.contains(expected_error),
            description);
    };

    expect_rejected(
        {{}, 8U},
        QStringLiteral("base path must be non-empty"),
        "capture rejects an empty base path");
    expect_rejected(
        {temp_dir.filePath(QStringLiteral("zero-bound")), 0U},
        QStringLiteral("max_bytes must be between 1"),
        "capture rejects a zero byte bound");
    expect_rejected(
        {temp_dir.filePath(QStringLiteral("missing/capture")), 8U},
        QStringLiteral("parent component is not an existing directory"),
        "capture rejects a missing parent directory");

    const QString existing_path =
        temp_dir.filePath(QStringLiteral("existing-prefix"));
    QFile existing_file(existing_path);
    ok &= check(
        existing_file.open(QIODevice::WriteOnly),
        "capture validation creates an existing base prefix");
    existing_file.close();
    expect_rejected(
        {existing_path, 8U},
        QStringLiteral("must be an unused file-name prefix"),
        "capture rejects an existing base prefix");

#if defined(Q_OS_WIN)
    expect_rejected(
        {QStringLiteral("\\\\server\\share\\capture"), 8U},
        QStringLiteral("does not accept UNC parent paths"),
        "capture rejects a UNC parent path");
#endif

    return ok;
}

bool test_rollover_retains_exact_bounded_suffix()
{
    bool ok = true;

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(), "rollover capture temp dir is valid");
    const Backend_output_capture_config config{
        temp_dir.filePath(QStringLiteral("rollover")),
        7U,
    };

    term::Backend_output_capture_writer writer(config);
    QByteArray accepted_bytes;
    for (const QByteArray& chunk : {
        QByteArrayLiteral("ab"),
        QByteArrayLiteral("cdef"),
        QByteArrayLiteral("ghij")})
    {
        const term::Backend_output_capture_writer_result result = writer.append(chunk);
        ok &= check(result.accepted, "rollover capture accepts chunk");
        accepted_bytes += chunk;
    }

    Backend_output_capture_recovery recovery =
        vnm_terminal::recover_backend_output_capture(config);
    const QByteArray bytes = recovered_bytes(recovery);
    ok &= check(recovery.status == Backend_output_capture_status::INCOMPLETE,
        "active rollover capture is valid and incomplete");
    ok &= check(recovery.segments.size() == 2U,
        "rollover capture normally retains two segments");
    ok &= check(recovery.retained_bytes <= config.max_bytes,
        "rollover capture enforces the aggregate byte bound");
    ok &= check(bytes == accepted_bytes.right(bytes.size()),
        "rollover segment concatenation is the exact retained raw suffix");
    ok &= check(bytes.size() == static_cast<qsizetype>(config.max_bytes),
        "odd aggregate bound is fully usable across two sequenced segments");

    const term::Backend_output_capture_writer_result finalize_result = writer.finalize();
    ok &= check(finalize_result.accepted, "rollover capture finalizes");
    recovery = vnm_terminal::recover_backend_output_capture(config);
    ok &= check(recovery.status == Backend_output_capture_status::FINALIZED,
        "cleanly closed rollover capture is valid and finalized");

    return ok;
}

bool test_oversized_chunk_and_one_byte_bound()
{
    bool ok = true;

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(), "oversized capture temp dir is valid");

    const Backend_output_capture_config oversized_config{
        temp_dir.filePath(QStringLiteral("oversized")),
        7U,
    };
    term::Backend_output_capture_writer oversized_writer(oversized_config);
    const QByteArray oversized_bytes = QByteArrayLiteral("0123456789");
    ok &= check(oversized_writer.append(oversized_bytes).accepted,
        "oversized capture accepts chunk");
    const Backend_output_capture_recovery oversized_recovery =
        vnm_terminal::recover_backend_output_capture(oversized_config);
    ok &= check(oversized_recovery.retained_bytes == oversized_config.max_bytes,
        "oversized capture retains exactly the hard aggregate limit");
    ok &= check(recovered_bytes(oversized_recovery) == QByteArrayLiteral("3456789"),
        "oversized capture discards only the prefix outside the retained suffix");

    const Backend_output_capture_config one_byte_config{
        temp_dir.filePath(QStringLiteral("one-byte")),
        1U,
    };
    term::Backend_output_capture_writer one_byte_writer(one_byte_config);
    ok &= check(one_byte_writer.append(QByteArrayLiteral("abc")).accepted,
        "one-byte capture accepts oversized chunk");
    const Backend_output_capture_recovery one_byte_recovery =
        vnm_terminal::recover_backend_output_capture(one_byte_config);
    ok &= check(one_byte_recovery.segments.size() == 1U &&
        one_byte_recovery.retained_bytes == 1U &&
        recovered_bytes(one_byte_recovery) == QByteArrayLiteral("c"),
        "one-byte capture retains one exact suffix byte without exceeding its bound");

    return ok;
}

bool test_recovery_prunes_extra_crash_left_segment()
{
    bool ok = true;

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(), "recovery capture temp dir is valid");
    const Backend_output_capture_config config{
        temp_dir.filePath(QStringLiteral("recovery")),
        8U,
    };

    {
        term::Backend_output_capture_writer writer(config);
        ok &= check(writer.append(QByteArrayLiteral("abcdefgh")).accepted,
            "recovery seed capture accepts bytes");
        ok &= check(writer.finalize().accepted,
            "recovery seed capture finalizes");
    }

    ok &= check(QFile::remove(config.base_path + QStringLiteral(".vnm-complete")),
        "recovery test simulates append invalidating capture completion");
    ok &= check(write_crash_left_segment(config, 3U, QByteArrayLiteral("ij")),
        "recovery test creates a flushed crash-left segment");
    const Backend_output_capture_recovery recovery =
        vnm_terminal::recover_backend_output_capture(config);
    ok &= check(recovery.status == Backend_output_capture_status::INCOMPLETE,
        "recovery reports a crash-left active segment as incomplete");
    ok &= check(recovery.segments.size() == 2U &&
        recovery.segments.front().sequence == 2U &&
        recovery.segments.back().sequence == 3U,
        "recovery retains the newest two sequenced segments");
    ok &= check(recovered_bytes(recovery) == QByteArrayLiteral("efghij"),
        "recovery retains the exact suffix across the crash-left segment");
    ok &= check(!QFile::exists(segment_path(config, 1U)),
        "recovery prunes the extra oldest raw segment");
    ok &= check(!QFile::exists(segment_path(config, 1U) + QStringLiteral(".finalized")),
        "recovery prunes the extra oldest finalized marker");

    return ok;
}

bool test_segment_markers_do_not_imply_capture_completion()
{
    bool ok = true;

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(), "completion manifest temp dir is valid");
    const Backend_output_capture_config config{
        temp_dir.filePath(QStringLiteral("completion-manifest")),
        8U,
    };

    {
        term::Backend_output_capture_writer writer(config);
        ok &= check(writer.append(QByteArrayLiteral("abcdefgh")).accepted,
            "completion manifest capture accepts rollover bytes");
    }

    const Backend_output_capture_recovery recovery =
        vnm_terminal::recover_backend_output_capture(config);
    ok &= check(recovery.status == Backend_output_capture_status::INCOMPLETE,
        "per-segment finalization does not imply capture-level completion");

    return ok;
}

bool test_partial_finalized_marker_is_recoverable_as_incomplete()
{
    bool ok = true;

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(), "partial marker temp dir is valid");
    const Backend_output_capture_config config{
        temp_dir.filePath(QStringLiteral("partial-marker")),
        8U,
    };

    const QString raw_path = segment_path(config, 1U);
    ok &= check(write_bytes(raw_path, QByteArrayLiteral("abcd")),
        "partial marker test creates a raw segment");
    ok &= check(write_bytes(
            raw_path + QStringLiteral(".finalized"),
            QByteArrayLiteral("vnm-terminal-output-capture-final")),
        "partial marker test creates an interrupted marker");

    const Backend_output_capture_recovery recovery =
        vnm_terminal::recover_backend_output_capture(config);
    ok &= check(recovery.status == Backend_output_capture_status::INCOMPLETE,
        "an interrupted known marker is recovered as incomplete");
    ok &= check(recovered_bytes(recovery) == QByteArrayLiteral("abcd"),
        "partial marker recovery preserves the flushed raw segment");
    ok &= check(!QFile::exists(raw_path + QStringLiteral(".finalized")),
        "partial marker recovery removes the interrupted marker under the writer lock");

    return ok;
}

bool test_partial_completion_manifest_is_recoverable_as_incomplete()
{
    bool ok = true;

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(), "partial completion temp dir is valid");
    const Backend_output_capture_config config{
        temp_dir.filePath(QStringLiteral("partial-completion")),
        8U,
    };
    const QString completion_path = config.base_path + QStringLiteral(".vnm-complete");
    ok &= check(write_bytes(
            completion_path,
            QByteArrayLiteral("vnm-terminal-output-capture-complete")),
        "partial completion test creates an interrupted manifest");

    const Backend_output_capture_recovery recovery =
        vnm_terminal::recover_backend_output_capture(config);
    ok &= check(recovery.status == Backend_output_capture_status::INCOMPLETE,
        "an interrupted completion manifest is recovered as incomplete");
    ok &= check(!QFile::exists(completion_path),
        "recovery removes the interrupted completion manifest under the writer lock");

    return ok;
}

bool test_explicit_finalize_controls_capture_completion()
{
    bool ok = true;

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(), "explicit completion temp dir is valid");
    const Backend_output_capture_config config{
        temp_dir.filePath(QStringLiteral("explicit-completion")),
        8U,
    };

    {
        term::Backend_output_capture_writer writer(config);
        ok &= check(writer.finalize().accepted,
            "explicit finalize completes an empty capture");
    }
    ok &= check(vnm_terminal::recover_backend_output_capture(config).status ==
        Backend_output_capture_status::FINALIZED,
        "empty capture completion is represented by its manifest");

    {
        term::Backend_output_capture_writer writer(config);
        ok &= check(writer.append(QByteArrayLiteral("resume")).accepted,
            "append resumes a completed capture");
    }
    const Backend_output_capture_recovery recovery =
        vnm_terminal::recover_backend_output_capture(config);
    ok &= check(recovery.status == Backend_output_capture_status::INCOMPLETE,
        "append invalidates the prior completion manifest before mutation");
    ok &= check(recovered_bytes(recovery) == QByteArrayLiteral("resume"),
        "resumed capture preserves appended raw bytes");

    return ok;
}

bool test_public_artifact_inspection_owns_complete_capture_schema()
{
    bool ok = true;

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(), "artifact inspection temp dir is valid");
    const Backend_output_capture_config config{
        temp_dir.filePath(QStringLiteral("artifact-inspection")),
        8U,
    };

    term::Backend_output_capture_writer writer(config);
    ok &= check(writer.append(QByteArrayLiteral("abcdefgh")).accepted,
        "artifact inspection capture writes two segments");
    ok &= check(writer.finalize().accepted,
        "artifact inspection capture finalizes");

    std::size_t raw_segment_count = 0U;
    std::size_t finalization_marker_count = 0U;
    std::size_t completion_manifest_count = 0U;
    std::size_t writer_lock_count = 0U;
    const QFileInfoList entries = QDir(temp_dir.path()).entryInfoList(
        QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDir::Name);
    for (const QFileInfo& entry : entries) {
        const vnm_terminal::Backend_output_capture_artifact_inspection inspection =
            vnm_terminal::inspect_backend_output_capture_artifact(
                config.base_path,
                entry.absoluteFilePath());
        ok &= check(inspection.valid(),
            "every artifact emitted by a finalized writer is publicly recognized and valid");
        switch (inspection.kind) {
            case Backend_output_capture_artifact_kind::RAW_SEGMENT:
                ++raw_segment_count;
                break;
            case Backend_output_capture_artifact_kind::SEGMENT_FINALIZATION_MARKER:
                ++finalization_marker_count;
                break;
            case Backend_output_capture_artifact_kind::COMPLETION_MANIFEST:
                ++completion_manifest_count;
                break;
            case Backend_output_capture_artifact_kind::WRITER_LOCK:
                ++writer_lock_count;
                break;
            case Backend_output_capture_artifact_kind::UNRECOGNIZED:
            default:
                break;
        }
    }
    ok &= check(raw_segment_count == 2U && finalization_marker_count == 2U,
        "artifact inspection recognizes both retained raw segments and finalization markers");
    ok &= check(completion_manifest_count == 1U,
        "artifact inspection recognizes the capture-level completion manifest");
    ok &= check(writer_lock_count == 1U,
        "artifact inspection recognizes the live cross-process writer lock");

    const QString foreign_path = temp_dir.filePath(QStringLiteral("foreign.bin"));
    ok &= check(write_bytes(foreign_path, QByteArrayLiteral("foreign")),
        "artifact inspection creates a foreign file");
    const vnm_terminal::Backend_output_capture_artifact_inspection foreign =
        vnm_terminal::inspect_backend_output_capture_artifact(
            config.base_path,
            foreign_path);
    ok &= check(!foreign.recognized() && !foreign.valid(),
        "artifact inspection does not claim foreign sibling files");

    const QString completion_path =
        config.base_path + QStringLiteral(".vnm-complete");
    ok &= check(QFile::remove(completion_path),
        "artifact inspection removes the valid completion manifest for corruption test");
    ok &= check(write_bytes(completion_path, QByteArrayLiteral("invalid-completion")),
        "artifact inspection creates a corrupt completion manifest");
    const vnm_terminal::Backend_output_capture_artifact_inspection corrupt_completion =
        vnm_terminal::inspect_backend_output_capture_artifact(
            config.base_path,
            completion_path);
    ok &= check(corrupt_completion.recognized() && !corrupt_completion.valid(),
        "artifact inspection recognizes but rejects a corrupt capture-owned marker");
    ok &= check(corrupt_completion.kind ==
        Backend_output_capture_artifact_kind::COMPLETION_MANIFEST,
        "artifact inspection preserves the kind of an invalid capture-owned marker");

    return ok;
}

bool test_finalize_does_not_mask_recovered_incomplete_segment()
{
    bool ok = true;

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(), "recovered incomplete temp dir is valid");
    const Backend_output_capture_config config{
        temp_dir.filePath(QStringLiteral("recovered-incomplete")),
        8U,
    };

    {
        term::Backend_output_capture_writer interrupted_writer(config);
        ok &= check(interrupted_writer.append(QByteArrayLiteral("abcd")).accepted,
            "recovered incomplete seed bytes are persisted");
    }

    term::Backend_output_capture_writer resumed_writer(config);
    ok &= check(resumed_writer.append(QByteArrayLiteral("ef")).accepted,
        "recovered incomplete capture accepts later diagnostic bytes");
    const term::Backend_output_capture_writer_result finalize_result =
        resumed_writer.finalize();
    const Backend_output_capture_recovery recovery =
        vnm_terminal::recover_backend_output_capture(config);

    ok &= check(!finalize_result.accepted,
        "explicit finalize rejects while the retained suffix contains crash-incomplete bytes");
    ok &= check(recovery.status == Backend_output_capture_status::INCOMPLETE,
        "rejected finalize preserves incomplete recovery truth");
    ok &= check(recovered_bytes(recovery) == QByteArrayLiteral("abcdef"),
        "rejected finalize preserves the full persisted diagnostic suffix");

    return ok;
}

bool test_writer_is_poisoned_after_injected_io_failure(
    Backend_output_capture_test_fault fault,
    const QString&                    prefix)
{
    bool ok = true;

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(),
        qPrintable(prefix + QStringLiteral(" temp dir is valid")));
    const Backend_output_capture_config config{
        temp_dir.filePath(prefix),
        8U,
    };

    term::Backend_output_capture_writer writer(config);
    writer.set_test_fault(fault);
    const term::Backend_output_capture_writer_result first_result =
        writer.append(QByteArrayLiteral("abcd"));
    ok &= check(!first_result.accepted,
        qPrintable(prefix + QStringLiteral(" first write is rejected")));

    const Backend_output_capture_recovery before_retry =
        vnm_terminal::recover_backend_output_capture(config);
    const QByteArray bytes_before_retry = recovered_bytes(before_retry);
    const QStringList names_before_retry = QDir(temp_dir.path()).entryList(
        QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDir::Name);

    const term::Backend_output_capture_writer_result retry_result =
        writer.append(QByteArrayLiteral("efgh"));
    const term::Backend_output_capture_writer_result finalize_result = writer.finalize();
    const Backend_output_capture_recovery after_retry =
        vnm_terminal::recover_backend_output_capture(config);
    const QStringList names_after_retry = QDir(temp_dir.path()).entryList(
        QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDir::Name);

    ok &= check(!retry_result.accepted && !finalize_result.accepted,
        qPrintable(prefix + QStringLiteral(" permanently disables the writer")));
    ok &= check(retry_result.error == first_result.error &&
        finalize_result.error == first_result.error,
        qPrintable(prefix + QStringLiteral(" retains the first failure cause")));
    ok &= check(names_after_retry == names_before_retry &&
        recovered_bytes(after_retry) == bytes_before_retry,
        qPrintable(prefix + QStringLiteral(" performs no mutation after failure")));
    ok &= check(after_retry.status == Backend_output_capture_status::INCOMPLETE &&
        after_retry.retained_bytes <= config.max_bytes,
        qPrintable(prefix + QStringLiteral(" remains bounded and recoverable")));

    return ok;
}

bool test_writer_is_poisoned_after_write_or_flush_failure()
{
    bool ok = true;
    ok &= test_writer_is_poisoned_after_injected_io_failure(
        Backend_output_capture_test_fault::PARTIAL_WRITE,
        QStringLiteral("partial-write"));
    ok &= test_writer_is_poisoned_after_injected_io_failure(
        Backend_output_capture_test_fault::FLUSH_AFTER_WRITE,
        QStringLiteral("flush-after-write"));
    return ok;
}

bool test_capture_error_is_reported_without_failing_backend_output()
{
    bool ok = true;

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(), "capture error temp dir is valid");

    term::Terminal_session_config config;
    config.backend_output_capture_config = Backend_output_capture_config{
        temp_dir.path(),
        8U,
    };
    config.trace_notification_limit = 8U;
    config.trace_output_chunk_limit = 2U;

    auto backend = std::make_unique<Output_backend>();
    Output_backend* backend_ptr = backend.get();
    term::Terminal_session session(std::move(backend), config);
    ok &= check(session.start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "capture error session starts");
    ok &= check(backend_ptr->emit_output(QByteArrayLiteral("survives-capture-error")),
        "capture error session accepts output callback");
    ok &= check(backend_ptr->emit_output(QByteArrayLiteral("after-capture-error")),
        "capture error session keeps accepting output callbacks");
    ok &= check(session.process_state() == term::Terminal_process_state::RUNNING,
        "capture error does not fail the terminal backend");
    ok &= check(session.output_chunks() ==
        std::vector<QByteArray>{
            QByteArrayLiteral("survives-capture-error"),
            QByteArrayLiteral("after-capture-error")},
        "capture error does not drop terminal output bytes");

    std::size_t capture_error_count = 0U;
    for (const term::Terminal_session_notification& notification : session.notifications()) {
        if (notification.backend_error.has_value() &&
            notification.backend_error->code == term::Terminal_backend_error_code::WRITE_FAILED &&
            notification.message.contains(QStringLiteral("backend output capture")))
        {
            ++capture_error_count;
        }
    }
    ok &= check(capture_error_count == 1U,
        "capture errors are coalesced to one nonfatal backend notification");

    return ok;
}

bool test_capture_error_does_not_consume_callback_queue_capacity()
{
    bool ok = true;

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(), "capture queue isolation temp dir is valid");

    term::Terminal_session_config config;
    config.backend_output_capture_config = Backend_output_capture_config{
        temp_dir.path(),
        8U,
    };
    config.backend_event_notifier                  = [] {};
    config.output_queue_limits.high_water_bytes    = 8U;
    config.output_queue_limits.hard_limit_bytes    = 64U;
    config.output_queue_limits.high_water_commands = 0U;
    config.output_queue_limits.hard_limit_commands = 1U;
    config.trace_notification_limit                = 8U;
    config.trace_output_chunk_limit                = 2U;

    auto backend = std::make_unique<Output_backend>();
    Output_backend* backend_ptr = backend.get();
    term::Terminal_session session(std::move(backend), config);
    ok &= check(session.start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "capture queue isolation session starts");
    ok &= check(backend_ptr->emit_output(QByteArrayLiteral("survives")),
        "capture queue isolation accepts output callback");

    session.process_backend_callback_events();
    ok &= check(session.process_state() == term::Terminal_process_state::RUNNING,
        "capture diagnostic does not stop backend callbacks");
    ok &= check(session.output_chunks() ==
        std::vector<QByteArray>{QByteArrayLiteral("survives")},
        "capture diagnostic does not consume the sole output queue slot");

    return ok;
}

bool test_capture_failure_is_not_reported_again_at_process_exit()
{
    bool ok = true;

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(), "capture exit error temp dir is valid");

    term::Terminal_session_config config;
    config.backend_output_capture_config = Backend_output_capture_config{
        temp_dir.path(),
        8U,
    };
    config.trace_notification_limit = 8U;

    auto backend = std::make_unique<Output_backend>();
    Output_backend* backend_ptr = backend.get();
    term::Terminal_session session(std::move(backend), config);
    ok &= check(session.start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "capture exit error session starts");
    ok &= check(backend_ptr->emit_output(QByteArrayLiteral("capture-fails")),
        "capture exit error session accepts normal terminal output");
    ok &= check(backend_ptr->terminate().code == term::Terminal_backend_result_code::ACCEPTED,
        "capture exit error session reports process exit");

    std::size_t capture_error_count = 0U;
    for (const term::Terminal_session_notification& notification : session.notifications()) {
        if (notification.backend_error.has_value() &&
            notification.backend_error->code == term::Terminal_backend_error_code::WRITE_FAILED &&
            notification.message.contains(QStringLiteral("backend output capture")))
        {
            ++capture_error_count;
        }
    }
    ok &= check(capture_error_count == 1U,
        "one capture persistence failure produces one backend notification across process exit");

    return ok;
}

bool test_capture_failure_survives_stopped_callback_queue()
{
    bool ok = true;

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(), "stopped callback capture temp dir is valid");
    const Backend_output_capture_config capture_config{
        temp_dir.filePath(QStringLiteral("stopped-callback")),
        8U,
    };

    term::Terminal_session_config config;
    config.backend_output_capture_config             = capture_config;
    config.backend_event_notifier                    = [] {};
    config.output_queue_limits.high_water_bytes      = 64U;
    config.output_queue_limits.hard_limit_bytes      = 64U;
    config.output_queue_limits.high_water_commands   = 0U;
    config.output_queue_limits.hard_limit_commands   = 1U;
    config.trace_notification_limit                  = 8U;

    auto backend = std::make_unique<Output_backend>();
    Output_backend* backend_ptr = backend.get();
    term::Terminal_session session(std::move(backend), config);
    ok &= check(session.start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "stopped callback capture session starts");
    ok &= check(backend_ptr->emit_output(QByteArrayLiteral("abcd")),
        "stopped callback capture persists the first segment");
    ok &= check(backend_ptr->emit_error(QStringLiteral("fill callback command queue")),
        "stopped callback capture reaches callback hard-limit handling");
    ok &= check(write_bytes(segment_path(capture_config, 2U), QByteArrayLiteral("xx")),
        "stopped callback capture creates a deterministic rollover collision");
    ok &= check(backend_ptr->emit_in_flight_output(QByteArrayLiteral("ef")),
        "stopped callback capture receives in-flight output after callback shutdown");
    session.process_backend_callback_events();
    ok &= check(backend_ptr->terminate().code == term::Terminal_backend_result_code::ACCEPTED,
        "stopped callback capture reports process exit");
    session.process_backend_callback_events();

    std::size_t capture_error_count = 0U;
    for (const term::Terminal_session_notification& notification : session.notifications()) {
        if (notification.backend_error.has_value() &&
            notification.backend_error->code == term::Terminal_backend_error_code::WRITE_FAILED &&
            notification.message.contains(QStringLiteral("backend output capture")))
        {
            ++capture_error_count;
        }
    }
    ok &= check(capture_error_count == 1U,
        "out-of-band capture failure survives a stopped bounded callback queue");

    return ok;
}

int run_forced_termination_child(const QString& base_path)
{
    const Backend_output_capture_config config{base_path, 8U};
    term::Backend_output_capture_writer writer(config);
    if (!writer.append(QByteArrayLiteral("abcdefghij")).accepted) {
        return 2;
    }

    std::cout << "READY\n" << std::flush;
    for (;;) {
        QThread::sleep(60U);
    }
}

int run_writer_lock_child(const QString& base_path)
{
    const Backend_output_capture_config config{base_path, 8U};
    term::Backend_output_capture_writer writer(config);
    if (!writer.append(QByteArrayLiteral("abcdefghij")).accepted) {
        return 2;
    }

    std::cout << "READY\n" << std::flush;
    for (;;) {
        QThread::sleep(60U);
    }
}

int run_rollover_boundary_child(const QString& base_path)
{
    const Backend_output_capture_config config{base_path, 8U};
    term::Backend_output_capture_writer writer(config);
    writer.set_test_fault(
        Backend_output_capture_test_fault::FAIL_AFTER_SEGMENT_FINALIZATION);
    if (writer.append(QByteArrayLiteral("abcde")).accepted) {
        return 2;
    }

    std::cout << "READY\n" << std::flush;
    for (;;) {
        QThread::sleep(60U);
    }
}

bool test_same_prefix_writer_is_rejected_before_mutation()
{
    bool ok = true;

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(), "writer lock temp dir is valid");
    const Backend_output_capture_config config{
        temp_dir.filePath(QStringLiteral("writer-lock")),
        8U,
    };

    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(
        QCoreApplication::applicationFilePath(),
        {QStringLiteral("--capture-writer-lock-child"), config.base_path});
    ok &= check(process.waitForStarted(5000),
        "writer lock child starts");
    ok &= check(process.waitForReadyRead(5000) &&
        process.readAllStandardOutput().contains("READY"),
        "writer lock child owns a live capture");

    const QStringList names_before = QDir(temp_dir.path()).entryList(
        QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDir::Name);
    term::Backend_output_capture_writer competing_writer(config);
    const term::Backend_output_capture_writer_result competing_result =
        competing_writer.append(QByteArrayLiteral("klmnop"));
    const QStringList names_after = QDir(temp_dir.path()).entryList(
        QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDir::Name);
    ok &= check(!competing_result.accepted,
        "a second same-prefix writer is rejected");
    ok &= check(names_after == names_before,
        "a rejected same-prefix writer performs no capture mutation");

    process.kill();
    ok &= check(process.waitForFinished(5000),
        "writer lock child is reaped");

    return ok;
}

bool test_termination_at_rollover_boundary_is_incomplete()
{
    bool ok = true;

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(), "rollover boundary temp dir is valid");
    const Backend_output_capture_config config{
        temp_dir.filePath(QStringLiteral("rollover-boundary")),
        8U,
    };

    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(
        QCoreApplication::applicationFilePath(),
        {QStringLiteral("--capture-rollover-boundary-child"), config.base_path});
    ok &= check(process.waitForStarted(5000),
        "rollover boundary child starts");
    ok &= check(process.waitForReadyRead(5000) &&
        process.readAllStandardOutput().contains("READY"),
        "rollover boundary child stops after segment marker publication");
    process.kill();
    ok &= check(process.waitForFinished(5000),
        "rollover boundary child is reaped");

    const Backend_output_capture_recovery recovery =
        vnm_terminal::recover_backend_output_capture(config);
    ok &= check(recovery.status == Backend_output_capture_status::INCOMPLETE,
        "rollover-boundary termination is not mistaken for capture completion");
    ok &= check(recovery.segments.size() == 1U &&
        recovery.segments.front().finalized &&
        recovered_bytes(recovery) == QByteArrayLiteral("abcd"),
        "rollover-boundary recovery retains the finalized segment without a successor");

    return ok;
}

bool test_forced_termination_recovers_flushed_incomplete_suffix()
{
    bool ok = true;

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(), "forced termination temp dir is valid");
    const Backend_output_capture_config config{
        temp_dir.filePath(QStringLiteral("forced-termination")),
        8U,
    };

    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(
        QCoreApplication::applicationFilePath(),
        {QStringLiteral("--capture-crash-child"), config.base_path});
    ok &= check(process.waitForStarted(5000),
        "forced termination child starts");
    ok &= check(process.waitForReadyRead(5000) &&
        process.readAllStandardOutput().contains("READY"),
        "forced termination child flushes its accepted chunk");
    process.kill();
    ok &= check(process.waitForFinished(5000),
        "forced termination child is reaped");
    ok &= check(process.exitStatus() == QProcess::CrashExit,
        "forced termination child exits without clean finalization");

    const Backend_output_capture_recovery recovery =
        vnm_terminal::recover_backend_output_capture(config);
    ok &= check(recovery.status == Backend_output_capture_status::INCOMPLETE,
        "forced termination recovery reports incomplete capture status");
    ok &= check(recovery.retained_bytes <= config.max_bytes,
        "forced termination recovery preserves the hard aggregate bound");
    ok &= check(recovered_bytes(recovery) == QByteArrayLiteral("cdefghij"),
        "forced termination recovery sees the exact flushed raw suffix");

    term::Backend_output_capture_writer resumed_writer(config);
    ok &= check(resumed_writer.append(QByteArrayLiteral("kl")).accepted,
        "a dead writer lock is recoverable immediately after forced termination");

    return ok;
}

}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() == 3 && arguments.at(1) == QStringLiteral("--capture-crash-child")) {
        return run_forced_termination_child(arguments.at(2));
    }
    if (arguments.size() == 3 &&
        arguments.at(1) == QStringLiteral("--capture-writer-lock-child"))
    {
        return run_writer_lock_child(arguments.at(2));
    }
    if (arguments.size() == 3 &&
        arguments.at(1) == QStringLiteral("--capture-rollover-boundary-child"))
    {
        return run_rollover_boundary_child(arguments.at(2));
    }

    bool ok = true;
    ok &= test_capture_is_disabled_by_default();
    ok &= test_capture_location_validation_rejects_unsafe_inputs();
    ok &= test_rollover_retains_exact_bounded_suffix();
    ok &= test_oversized_chunk_and_one_byte_bound();
    ok &= test_recovery_prunes_extra_crash_left_segment();
    ok &= test_segment_markers_do_not_imply_capture_completion();
    ok &= test_partial_finalized_marker_is_recoverable_as_incomplete();
    ok &= test_partial_completion_manifest_is_recoverable_as_incomplete();
    ok &= test_explicit_finalize_controls_capture_completion();
    ok &= test_public_artifact_inspection_owns_complete_capture_schema();
    ok &= test_finalize_does_not_mask_recovered_incomplete_segment();
    ok &= test_writer_is_poisoned_after_write_or_flush_failure();
    ok &= test_capture_error_is_reported_without_failing_backend_output();
    ok &= test_capture_error_does_not_consume_callback_queue_capacity();
    ok &= test_capture_failure_is_not_reported_again_at_process_exit();
    ok &= test_capture_failure_survives_stopped_callback_queue();
    ok &= test_same_prefix_writer_is_rejected_before_mutation();
    ok &= test_termination_at_rollover_boundary_is_incomplete();
    ok &= test_forced_termination_recovers_flushed_incomplete_suffix();

    if (ok) {
        std::cout << "backend output capture tests passed\n";
        return 0;
    }
    return 1;
}
