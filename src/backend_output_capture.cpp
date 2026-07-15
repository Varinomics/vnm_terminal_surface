#include "vnm_terminal/internal/backend_output_capture_writer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>
#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#if defined(Q_OS_WIN)
#include <io.h>
#include <qt_windows.h>
#endif

namespace vnm_terminal {
namespace {

constexpr auto k_segment_infix       = ".vnm-segment-";
constexpr auto k_segment_suffix      = ".raw";
constexpr auto k_finalized_suffix    = ".finalized";
constexpr auto k_finalized_marker    = "vnm-terminal-output-capture-finalized-v1\n";
constexpr auto k_completion_suffix   = ".vnm-complete";
constexpr auto k_completion_marker   = "vnm-terminal-output-capture-complete-v1\n";
constexpr auto k_lock_suffix         = ".vnm-lock";

struct Capture_layout
{
    Backend_output_capture_config           config;
    QString                                 parent_path;
    QString                                 base_name;
    QString                                 completion_path;
    QString                                 lock_path;
};

struct Disk_segment
{
    Backend_output_capture_segment          public_segment;
    QString                                 finalized_path;
};

struct Capture_scan
{
    Backend_output_capture_recovery         recovery;
    std::vector<Disk_segment>               segments;
};

struct Capture_artifact_identity
{
    Backend_output_capture_artifact_kind    kind =
        Backend_output_capture_artifact_kind::UNRECOGNIZED;
    std::uint64_t                           sequence = 0U;
    bool                                    sequence_valid = true;
};

enum class Marker_state : std::uint8_t
{
    ABSENT,
    VALID,
    PARTIAL,
    INVALID,
};

QString segment_path(
    const Capture_layout& layout,
    std::uint64_t         sequence)
{
    const QString file_name = QStringLiteral("%1%2%3%4")
        .arg(layout.base_name, QString::fromLatin1(k_segment_infix))
        .arg(sequence, 20, 10, QLatin1Char('0'))
        .arg(QString::fromLatin1(k_segment_suffix));
    return QDir(layout.parent_path).filePath(file_name);
}

QString finalized_path(const QString& raw_path)
{
    return raw_path + QString::fromLatin1(k_finalized_suffix);
}

Capture_artifact_identity classify_capture_artifact_name(
    const Capture_layout& layout,
    const QString&        name)
{
    if (name == layout.base_name + QString::fromLatin1(k_completion_suffix)) {
        return {Backend_output_capture_artifact_kind::COMPLETION_MANIFEST};
    }
    if (name == layout.base_name + QString::fromLatin1(k_lock_suffix)) {
        return {Backend_output_capture_artifact_kind::WRITER_LOCK};
    }

    const QString escaped_prefix = QRegularExpression::escape(
        layout.base_name + QString::fromLatin1(k_segment_infix));
    const QRegularExpression segment_pattern(
        QStringLiteral("^%1([0-9]{20})\\.raw(\\.finalized)?$")
            .arg(escaped_prefix));
    const QRegularExpressionMatch match = segment_pattern.match(name);
    if (!match.hasMatch()) {
        return {};
    }

    bool sequence_ok = false;
    const std::uint64_t sequence = match.captured(1).toULongLong(&sequence_ok);
    return {
        match.captured(2).isEmpty()
            ? Backend_output_capture_artifact_kind::RAW_SEGMENT
            : Backend_output_capture_artifact_kind::SEGMENT_FINALIZATION_MARKER,
        sequence,
        sequence_ok && sequence != 0U,
    };
}

std::size_t retained_segment_limit(std::size_t max_bytes)
{
    return max_bytes == 1U ? 1U : 2U;
}

std::size_t segment_capacity(
    std::size_t   max_bytes,
    std::uint64_t sequence)
{
    if (max_bytes == 1U) {
        return 1U;
    }

    const std::size_t lower_capacity = max_bytes / 2U;
    const std::size_t upper_capacity = max_bytes - lower_capacity;
    return sequence % 2U == 1U ? lower_capacity : upper_capacity;
}

bool is_reparse_point(const QString& path)
{
    const QFileInfo info(path);
    if (info.isSymLink()) {
        return true;
    }

#if defined(Q_OS_WIN)
    const QString native_path = QDir::toNativeSeparators(info.absoluteFilePath());
    const DWORD attributes = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(native_path.utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
#else
    return false;
#endif
}

bool parent_path_is_safe(
    const QString& parent_path,
    QString&       error)
{
    const QString absolute_path = QDir::cleanPath(QFileInfo(parent_path).absoluteFilePath());

#if defined(Q_OS_WIN)
    const QString native_path = QDir::toNativeSeparators(absolute_path);
    if (native_path.startsWith(QStringLiteral("\\\\"))) {
        error = QStringLiteral(
            "backend output capture does not accept UNC parent paths: \"%1\"")
            .arg(absolute_path);
        return false;
    }
    if (native_path.size() < 3 || native_path.at(1) != QLatin1Char(':')) {
        error = QStringLiteral(
            "backend output capture parent path is not an absolute drive path: \"%1\"")
            .arg(absolute_path);
        return false;
    }

    const QString drive_root = native_path.left(3);
    const UINT drive_type = GetDriveTypeW(
        reinterpret_cast<LPCWSTR>(drive_root.utf16()));
    if (drive_type == DRIVE_REMOTE) {
        error = QStringLiteral(
            "backend output capture does not accept remote drive parent paths: \"%1\"")
            .arg(absolute_path);
        return false;
    }

    QString current_path = native_path.left(3);
    const QStringList components = native_path.mid(3).split(
        QLatin1Char('\\'),
        Qt::SkipEmptyParts);
#else
    QString current_path = QDir::rootPath();
    const QStringList components = QDir::fromNativeSeparators(absolute_path).split(
        QLatin1Char('/'),
        Qt::SkipEmptyParts);
#endif

    for (const QString& component : components) {
        current_path = QDir(current_path).filePath(component);
        const QFileInfo info(current_path);
        if (!info.exists() || !info.isDir()) {
            error = QStringLiteral(
                "backend output capture parent component is not an existing directory: \"%1\"")
                .arg(current_path);
            return false;
        }
        if (is_reparse_point(current_path)) {
            error = QStringLiteral(
                "backend output capture refuses reparse traversal through \"%1\"")
                .arg(current_path);
            return false;
        }
    }

    return true;
}

bool opened_file_is_safe(
    QFile&         file,
    const QString& expected_path,
    QString&       error)
{
#if defined(Q_OS_WIN)
    const qintptr file_descriptor = file.handle();
    const intptr_t native_handle = file_descriptor >= 0
        ? _get_osfhandle(static_cast<int>(file_descriptor))
        : -1;
    if (native_handle == -1) {
        error = QStringLiteral(
            "backend output capture could not inspect opened file \"%1\"")
            .arg(expected_path);
        return false;
    }

    const HANDLE handle = reinterpret_cast<HANDLE>(native_handle);
    FILE_ATTRIBUTE_TAG_INFO tag_info{};
    if (!GetFileInformationByHandleEx(
            handle,
            FileAttributeTagInfo,
            &tag_info,
            sizeof(tag_info)))
    {
        error = QStringLiteral(
            "backend output capture could not inspect opened file attributes for \"%1\"")
            .arg(expected_path);
        return false;
    }
    if ((tag_info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        error = QStringLiteral(
            "backend output capture opened a reparse point for \"%1\"")
            .arg(expected_path);
        return false;
    }

    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD required_size = GetFinalPathNameByHandleW(handle, nullptr, 0U, flags);
    if (required_size == 0U) {
        error = QStringLiteral(
            "backend output capture could not resolve opened file \"%1\"")
            .arg(expected_path);
        return false;
    }

    std::vector<wchar_t> buffer(static_cast<std::size_t>(required_size) + 1U);
    const DWORD actual_size = GetFinalPathNameByHandleW(
        handle,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        flags);
    if (actual_size == 0U || actual_size >= buffer.size()) {
        error = QStringLiteral(
            "backend output capture could not resolve opened file \"%1\"")
            .arg(expected_path);
        return false;
    }

    QString opened_path = QString::fromWCharArray(
        buffer.data(),
        static_cast<qsizetype>(actual_size));
    if (opened_path.startsWith(QStringLiteral("\\\\?\\UNC\\"), Qt::CaseInsensitive)) {
        opened_path = QStringLiteral("\\\\") + opened_path.mid(8);
    }
    else
    if (opened_path.startsWith(QStringLiteral("\\\\?\\"))) {
        opened_path.remove(0, 4);
    }

    const QString normalized_opened = QDir::cleanPath(
        QDir::fromNativeSeparators(opened_path));
    const QString normalized_expected = QDir::cleanPath(
        QFileInfo(expected_path).absoluteFilePath());
    if (normalized_opened.compare(normalized_expected, Qt::CaseInsensitive) != 0) {
        error = QStringLiteral(
            "backend output capture opened an unexpected object for \"%1\": \"%2\"")
            .arg(normalized_expected, normalized_opened);
        return false;
    }
#else
    Q_UNUSED(file);
    Q_UNUSED(expected_path);
    Q_UNUSED(error);
#endif
    return true;
}

std::optional<Capture_layout> capture_layout(
    const Backend_output_capture_config& config,
    QString&                             error)
{
    if (config.base_path.isEmpty()) {
        error = QStringLiteral("backend output capture base path must be non-empty");
        return std::nullopt;
    }
    if (config.max_bytes == 0U ||
        config.max_bytes > static_cast<std::size_t>(std::numeric_limits<qint64>::max()))
    {
        error = QStringLiteral(
            "backend output capture max_bytes must be between 1 and %1")
            .arg(std::numeric_limits<qint64>::max());
        return std::nullopt;
    }

    const QFileInfo base_info(config.base_path);
    if (base_info.exists()) {
        error = QStringLiteral(
            "backend output capture base path must be an unused file-name prefix: \"%1\"")
            .arg(base_info.absoluteFilePath());
        return std::nullopt;
    }

    const QString base_name   = base_info.fileName();
    const QString parent_path = base_info.absolutePath();
    if (base_name.isEmpty()) {
        error = QStringLiteral(
            "backend output capture base path has no file-name prefix: \"%1\"")
            .arg(config.base_path);
        return std::nullopt;
    }
    if (!parent_path_is_safe(parent_path, error)) {
        return std::nullopt;
    }

    Capture_layout layout;
    layout.config.base_path = QDir(parent_path).filePath(base_name);
    layout.config.max_bytes = config.max_bytes;
    layout.parent_path      = parent_path;
    layout.base_name        = base_name;
    layout.completion_path  = layout.config.base_path + QString::fromLatin1(k_completion_suffix);
    layout.lock_path        = layout.config.base_path + QString::fromLatin1(k_lock_suffix);
    return layout;
}

bool remove_capture_file(
    const QString& path,
    QString&       error)
{
    if (!QFileInfo::exists(path)) {
        return true;
    }
    if (is_reparse_point(path)) {
        error = QStringLiteral(
            "backend output capture refuses to remove reparse point \"%1\"")
            .arg(path);
        return false;
    }
    if (!QFile::remove(path)) {
        error = QStringLiteral(
            "backend output capture could not prune \"%1\"")
            .arg(path);
        return false;
    }
    return true;
}

bool prune_segment(
    const Disk_segment& segment,
    QString&            error)
{
    // Removing the marker first means interruption can only leave an
    // incomplete raw segment, which is a valid recoverable state.
    return remove_capture_file(segment.finalized_path, error) &&
        remove_capture_file(segment.public_segment.path, error);
}

Marker_state inspect_marker(
    const QString&   path,
    const QByteArray& expected_bytes,
    const QString&   description,
    QString&         error)
{
    const QFileInfo path_info(path);
    if (!path_info.exists() && !path_info.isSymLink()) {
        return Marker_state::ABSENT;
    }
    if (is_reparse_point(path)) {
        error = QStringLiteral(
            "backend output capture %1 is a reparse point: \"%2\"")
            .arg(description, path);
        return Marker_state::INVALID;
    }

    QFile file(path);
    if (!path_info.isFile()) {
        error = QStringLiteral(
            "backend output capture %1 is not a regular file: \"%2\"")
            .arg(description, path);
        return Marker_state::INVALID;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral(
            "backend output capture could not read %1 \"%2\": %3")
            .arg(description, path, file.errorString());
        return Marker_state::INVALID;
    }
    if (!opened_file_is_safe(file, path, error)) {
        return Marker_state::INVALID;
    }

    const QByteArray bytes = file.readAll();
    if (bytes == expected_bytes) {
        return Marker_state::VALID;
    }
    if (bytes.size() < expected_bytes.size() && expected_bytes.startsWith(bytes)) {
        return Marker_state::PARTIAL;
    }

    error = QStringLiteral(
        "backend output capture %1 is invalid: \"%2\"")
        .arg(description, path);
    return Marker_state::INVALID;
}

bool clear_partial_marker(
    const QString& path,
    bool           allow_mutation,
    QString&       error)
{
    if (!allow_mutation) {
        return true;
    }
    if (!remove_capture_file(path, error)) {
        error = QStringLiteral(
            "backend output capture could not clear interrupted marker \"%1\": %2")
            .arg(path, error);
        return false;
    }
    return true;
}

Capture_scan scan_capture(
    const Capture_layout& layout,
    bool                  allow_mutation)
{
    Capture_scan scan;
    const Backend_output_capture_config& config = layout.config;

    const Marker_state completion_state = inspect_marker(
        layout.completion_path,
        QByteArray(k_completion_marker),
        QStringLiteral("completion manifest"),
        scan.recovery.error);
    if (completion_state == Marker_state::INVALID) {
        return scan;
    }
    if (completion_state == Marker_state::PARTIAL &&
        !clear_partial_marker(
            layout.completion_path,
            allow_mutation,
            scan.recovery.error))
    {
        return scan;
    }

    struct Candidate
    {
        QString raw_path;
        QString marker_path;
    };
    std::map<std::uint64_t, Candidate> candidates;
    const QStringList names = QDir(layout.parent_path).entryList(
        QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDir::Name);
    for (const QString& name : names) {
        const Capture_artifact_identity identity =
            classify_capture_artifact_name(layout, name);
        const bool is_raw = identity.kind ==
            Backend_output_capture_artifact_kind::RAW_SEGMENT;
        const bool is_finalization_marker = identity.kind ==
            Backend_output_capture_artifact_kind::SEGMENT_FINALIZATION_MARKER;
        if (!is_raw && !is_finalization_marker) {
            continue;
        }
        if (!identity.sequence_valid) {
            scan.recovery.error = QStringLiteral(
                "backend output capture has an invalid segment sequence: \"%1\"")
                .arg(name);
            return scan;
        }

        Candidate& candidate = candidates[identity.sequence];
        const QString path = QDir(layout.parent_path).filePath(name);
        if (is_raw) {
            candidate.raw_path = path;
        }
        else {
            candidate.marker_path = path;
        }
    }

    bool has_incomplete_artifact = completion_state == Marker_state::PARTIAL;
    for (const auto& [sequence, candidate] : candidates) {
        if (candidate.raw_path.isEmpty()) {
            const Marker_state marker_state = inspect_marker(
                candidate.marker_path,
                QByteArray(k_finalized_marker),
                QStringLiteral("finalized marker"),
                scan.recovery.error);
            if (marker_state == Marker_state::PARTIAL) {
                has_incomplete_artifact = true;
                if (!clear_partial_marker(
                        candidate.marker_path,
                        allow_mutation,
                        scan.recovery.error))
                {
                    return scan;
                }
                continue;
            }
            scan.recovery.error = QStringLiteral(
                "backend output capture has an orphan finalized marker: \"%1\"")
                .arg(candidate.marker_path);
            return scan;
        }
        if (is_reparse_point(candidate.raw_path)) {
            scan.recovery.error = QStringLiteral(
                "backend output capture raw segment is a reparse point: \"%1\"")
                .arg(candidate.raw_path);
            return scan;
        }

        QFile raw_file(candidate.raw_path);
        if (!QFileInfo(candidate.raw_path).isFile()) {
            scan.recovery.error = QStringLiteral(
                "backend output capture raw segment is not a regular file: \"%1\"")
                .arg(candidate.raw_path);
            return scan;
        }
        if (!raw_file.open(QIODevice::ReadOnly)) {
            scan.recovery.error = QStringLiteral(
                "backend output capture could not read raw segment \"%1\": %2")
                .arg(candidate.raw_path, raw_file.errorString());
            return scan;
        }
        if (!opened_file_is_safe(raw_file, candidate.raw_path, scan.recovery.error)) {
            return scan;
        }
        const qint64 byte_count = raw_file.size();

        Marker_state marker_state = Marker_state::ABSENT;
        if (!candidate.marker_path.isEmpty()) {
            marker_state = inspect_marker(
                candidate.marker_path,
                QByteArray(k_finalized_marker),
                QStringLiteral("finalized marker"),
                scan.recovery.error);
            if (marker_state == Marker_state::INVALID) {
                return scan;
            }
            if (marker_state == Marker_state::PARTIAL) {
                has_incomplete_artifact = true;
                if (!clear_partial_marker(
                        candidate.marker_path,
                        allow_mutation,
                        scan.recovery.error))
                {
                    return scan;
                }
                marker_state = Marker_state::ABSENT;
            }
        }

        if (byte_count == 0 && marker_state != Marker_state::VALID) {
            has_incomplete_artifact = true;
            raw_file.close();
            if (allow_mutation &&
                !remove_capture_file(candidate.raw_path, scan.recovery.error))
            {
                return scan;
            }
            continue;
        }
        const std::size_t capacity = segment_capacity(config.max_bytes, sequence);
        if (byte_count <= 0 || static_cast<std::size_t>(byte_count) > capacity) {
            scan.recovery.error = QStringLiteral(
                "backend output capture segment \"%1\" has %2 bytes; capacity is %3")
                .arg(candidate.raw_path)
                .arg(byte_count)
                .arg(capacity);
            return scan;
        }

        const bool finalized = marker_state == Marker_state::VALID;

        Disk_segment segment;
        segment.public_segment.path        = candidate.raw_path;
        segment.public_segment.sequence    = sequence;
        segment.public_segment.byte_count  = static_cast<std::size_t>(byte_count);
        segment.public_segment.finalized   = finalized;
        segment.finalized_path             = finalized_path(candidate.raw_path);
        scan.segments.push_back(std::move(segment));
    }

    for (std::size_t index = 1U; index < scan.segments.size(); ++index) {
        const std::uint64_t previous = scan.segments[index - 1U].public_segment.sequence;
        if (previous == std::numeric_limits<std::uint64_t>::max() ||
            scan.segments[index].public_segment.sequence != previous + 1U)
        {
            scan.recovery.error = QStringLiteral(
                "backend output capture segment sequences are not consecutive");
            return scan;
        }
    }

    const std::size_t keep_count = retained_segment_limit(config.max_bytes);
    while (scan.segments.size() > keep_count) {
        if (allow_mutation &&
            !prune_segment(scan.segments.front(), scan.recovery.error))
        {
            return scan;
        }
        scan.segments.erase(scan.segments.begin());
    }

    bool all_segments_finalized = true;
    for (const Disk_segment& segment : scan.segments) {
        scan.recovery.segments.push_back(segment.public_segment);
        scan.recovery.retained_bytes += segment.public_segment.byte_count;
        if (!segment.public_segment.finalized) {
            all_segments_finalized = false;
        }
    }

    if (completion_state == Marker_state::VALID) {
        if (!all_segments_finalized) {
            scan.recovery.status = Backend_output_capture_status::INVALID;
            scan.recovery.error = QStringLiteral(
                "backend output capture completion manifest conflicts with an incomplete segment");
            scan.recovery.segments.clear();
            scan.recovery.retained_bytes = 0U;
            return scan;
        }
        scan.recovery.status = Backend_output_capture_status::FINALIZED;
    }
    else
    if (!scan.segments.empty() || has_incomplete_artifact) {
        scan.recovery.status = Backend_output_capture_status::INCOMPLETE;
    }
    else {
        scan.recovery.status = Backend_output_capture_status::EMPTY;
    }

    return scan;
}

bool publish_marker(
    const QString&   path,
    const QByteArray& marker_bytes,
    const QString&   description,
    QString&         error)
{
    const Marker_state current_state = inspect_marker(
        path,
        marker_bytes,
        description,
        error);
    if (current_state == Marker_state::VALID) {
        return true;
    }
    if (current_state == Marker_state::INVALID) {
        return false;
    }
    if (current_state == Marker_state::PARTIAL &&
        !remove_capture_file(path, error))
    {
        return false;
    }

    QSaveFile marker(path);
    marker.setDirectWriteFallback(false);
    if (!marker.open(QIODevice::WriteOnly)) {
        error = QStringLiteral(
            "backend output capture could not create %1 \"%2\": %3")
            .arg(description, path, marker.errorString());
        return false;
    }
    if (marker.write(marker_bytes) != marker_bytes.size() || !marker.commit()) {
        error = QStringLiteral(
            "backend output capture could not publish %1 \"%2\": %3")
            .arg(description, path, marker.errorString());
        return false;
    }

    QString validation_error;
    const Marker_state published_state = inspect_marker(
        path,
        marker_bytes,
        description,
        validation_error);
    if (published_state != Marker_state::VALID) {
        error = validation_error.isEmpty()
            ? QStringLiteral(
                "backend output capture could not validate published %1 \"%2\"")
                .arg(description, path)
            : validation_error;
        return false;
    }
    return true;
}

bool write_finalized_marker(
    const QString& raw_path,
    QString&       error)
{
    return publish_marker(
        finalized_path(raw_path),
        QByteArray(k_finalized_marker),
        QStringLiteral("finalized marker"),
        error);
}

}

Backend_output_capture_artifact_inspection inspect_backend_output_capture_artifact(
    const QString& base_path,
    const QString& artifact_path)
{
    Backend_output_capture_artifact_inspection inspection;

    QString layout_error;
    const std::optional<Capture_layout> layout = capture_layout(
        Backend_output_capture_config{base_path, 1U},
        layout_error);
    if (!layout.has_value()) {
        inspection.error = std::move(layout_error);
        return inspection;
    }

    const QFileInfo artifact_info(artifact_path);
    const QString artifact_name = artifact_info.fileName();
    const QString normalized_artifact_path = QDir::cleanPath(
        artifact_info.absoluteFilePath());
    const QString expected_path = QDir::cleanPath(
        QDir(layout->parent_path).filePath(artifact_name));
#if defined(Q_OS_WIN)
    constexpr Qt::CaseSensitivity k_path_case_sensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity k_path_case_sensitivity = Qt::CaseSensitive;
#endif
    if (normalized_artifact_path.compare(expected_path, k_path_case_sensitivity) != 0) {
        return inspection;
    }

    const Capture_artifact_identity identity =
        classify_capture_artifact_name(*layout, artifact_name);
    inspection.kind     = identity.kind;
    inspection.sequence = identity.sequence;
    if (!inspection.recognized()) {
        return inspection;
    }
    if (!identity.sequence_valid) {
        inspection.error = QStringLiteral(
            "backend output capture artifact has an invalid segment sequence: \"%1\"")
            .arg(artifact_path);
        return inspection;
    }

    const bool marker = identity.kind ==
            Backend_output_capture_artifact_kind::SEGMENT_FINALIZATION_MARKER ||
        identity.kind == Backend_output_capture_artifact_kind::COMPLETION_MANIFEST;
    if (marker) {
        const QByteArray marker_bytes = identity.kind ==
                Backend_output_capture_artifact_kind::SEGMENT_FINALIZATION_MARKER
            ? QByteArray(k_finalized_marker)
            : QByteArray(k_completion_marker);
        const QString description = identity.kind ==
                Backend_output_capture_artifact_kind::SEGMENT_FINALIZATION_MARKER
            ? QStringLiteral("finalized marker")
            : QStringLiteral("completion manifest");
        const Marker_state marker_state = inspect_marker(
            normalized_artifact_path,
            marker_bytes,
            description,
            inspection.error);
        if (marker_state != Marker_state::VALID) {
            if (inspection.error.isEmpty()) {
                inspection.error = marker_state == Marker_state::PARTIAL
                    ? QStringLiteral(
                        "backend output capture %1 is incomplete: \"%2\"")
                        .arg(description, normalized_artifact_path)
                    : QStringLiteral(
                        "backend output capture %1 does not exist: \"%2\"")
                        .arg(description, normalized_artifact_path);
            }
            return inspection;
        }
    }
    else {
        if (!artifact_info.exists() || !artifact_info.isFile()) {
            inspection.error = QStringLiteral(
                "backend output capture artifact is not a regular file: \"%1\"")
                .arg(normalized_artifact_path);
            return inspection;
        }
        if (is_reparse_point(normalized_artifact_path)) {
            inspection.error = QStringLiteral(
                "backend output capture artifact is a reparse point: \"%1\"")
                .arg(normalized_artifact_path);
            return inspection;
        }

        QFile file(normalized_artifact_path);
        if (!file.open(QIODevice::ReadOnly)) {
            inspection.error = QStringLiteral(
                "backend output capture could not read artifact \"%1\": %2")
                .arg(normalized_artifact_path, file.errorString());
            return inspection;
        }
        if (!opened_file_is_safe(file, normalized_artifact_path, inspection.error)) {
            return inspection;
        }
    }

    const qint64 byte_count = QFileInfo(normalized_artifact_path).size();
    if (byte_count < 0) {
        inspection.error = QStringLiteral(
            "backend output capture could not determine artifact size: \"%1\"")
            .arg(normalized_artifact_path);
        return inspection;
    }
    inspection.byte_count = static_cast<std::uint64_t>(byte_count);
    return inspection;
}

Backend_output_capture_recovery recover_backend_output_capture(
    const Backend_output_capture_config& config)
{
    QString error;
    const std::optional<Capture_layout> layout = capture_layout(config, error);
    if (!layout.has_value()) {
        Backend_output_capture_recovery recovery;
        recovery.error = std::move(error);
        return recovery;
    }

    QLockFile lock(layout->lock_path);
    const bool owns_lock = lock.tryLock(0);
    return scan_capture(*layout, owns_lock).recovery;
}

}

namespace vnm_terminal::internal {

class Backend_output_capture_writer::State
{
public:
    explicit State(Backend_output_capture_config config)
    :
        m_config(std::move(config))
    {}

    Backend_output_capture_writer_result append(QByteArrayView bytes)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_poisoned) {
            return reject(m_error);
        }
        if (m_finalized) {
            return reject(QStringLiteral(
                "backend output capture cannot append after finalization"));
        }
        if (bytes.isEmpty()) {
            return accept();
        }
        if (!ensure_initialized()) {
            return poison_and_reject();
        }
        if (!invalidate_completion_manifest()) {
            return poison_and_reject();
        }

        if (static_cast<std::size_t>(bytes.size()) >= m_config.max_bytes) {
            const qsizetype suffix_size = static_cast<qsizetype>(m_config.max_bytes);
            const Backend_output_capture_writer_result result =
                replace_with_suffix(bytes.sliced(bytes.size() - suffix_size));
            return result.accepted ? result : poison_and_reject();
        }

        qsizetype offset = 0;
        while (offset < bytes.size()) {
            if (!m_current_file) {
                const std::size_t capacity = segment_capacity(m_config.max_bytes, m_next_sequence);
                const qsizetype write_size = std::min(
                    bytes.size() - offset,
                    static_cast<qsizetype>(capacity));
                if (!open_and_write_new_segment(bytes.sliced(offset, write_size))) {
                    return poison_and_reject();
                }
                offset += write_size;
                if (!prune_retained_segments()) {
                    return poison_and_reject();
                }
                continue;
            }

            Backend_output_capture_segment& current = m_segments.back().public_segment;
            const std::size_t capacity = segment_capacity(m_config.max_bytes, current.sequence);
            const std::size_t remaining_capacity = capacity - current.byte_count;
            if (remaining_capacity == 0U) {
                if (!finalize_current_segment()) {
                    return poison_and_reject();
                }
                continue;
            }

            const qsizetype write_size = std::min(
                bytes.size() - offset,
                static_cast<qsizetype>(remaining_capacity));
            if (!write_and_flush(*m_current_file, bytes.sliced(offset, write_size))) {
                return poison_and_reject();
            }
            current.byte_count += static_cast<std::size_t>(write_size);
            offset             += write_size;
        }

        return accept();
    }

    Backend_output_capture_writer_result finalize()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_poisoned) {
            return reject(m_error);
        }
        if (m_finalized) {
            return accept();
        }
        if (!ensure_initialized()) {
            return poison_and_reject();
        }
        if (m_current_file && !finalize_current_segment()) {
            return poison_and_reject();
        }
        const auto incomplete = std::find_if(
            m_segments.begin(),
            m_segments.end(),
            [](const Disk_segment& segment) {
                return !segment.public_segment.finalized;
            });
        if (incomplete != m_segments.end()) {
            m_error = QStringLiteral(
                "backend output capture cannot finalize while the retained suffix "
                "contains an incomplete segment");
            return poison_and_reject();
        }
        if (!parent_path_is_safe(m_layout.parent_path, m_error)) {
            return poison_and_reject();
        }
        if (!publish_marker(
                m_layout.completion_path,
                QByteArray(k_completion_marker),
                QStringLiteral("completion manifest"),
                m_error))
        {
            return poison_and_reject();
        }
        m_finalized = true;
        return accept();
    }

#if defined(VNM_TERMINAL_CAPTURE_TEST_HOOKS)
    void set_test_fault(Backend_output_capture_test_fault fault)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_test_fault = fault;
    }
#endif

private:
    Backend_output_capture_writer_result accept() const
    {
        return {true, {}};
    }

    Backend_output_capture_writer_result reject(QString error) const
    {
        return {false, std::move(error)};
    }

    Backend_output_capture_writer_result poison_and_reject()
    {
        if (m_error.isEmpty()) {
            m_error = QStringLiteral("backend output capture writer failed");
        }
        if (m_current_file) {
            m_current_file->close();
            m_current_file.reset();
        }
        m_poisoned = true;
        return reject(m_error);
    }

    bool ensure_initialized()
    {
        if (m_initialized) {
            return true;
        }

        QString layout_error;
        const std::optional<Capture_layout> layout = capture_layout(m_config, layout_error);
        if (!layout.has_value()) {
            m_error = std::move(layout_error);
            return false;
        }

        const QFileInfo lock_info(layout->lock_path);
        if ((lock_info.exists() || lock_info.isSymLink()) &&
            is_reparse_point(layout->lock_path))
        {
            m_error = QStringLiteral(
                "backend output capture lock path is a reparse point: \"%1\"")
                .arg(layout->lock_path);
            return false;
        }
        m_writer_lock = std::make_unique<QLockFile>(layout->lock_path);
        if (!m_writer_lock->tryLock(0)) {
            m_error = QStringLiteral(
                "backend output capture already has an active writer for \"%1\"")
                .arg(layout->config.base_path);
            return false;
        }
        if (!parent_path_is_safe(layout->parent_path, m_error) ||
            is_reparse_point(layout->lock_path))
        {
            if (m_error.isEmpty()) {
                m_error = QStringLiteral(
                    "backend output capture lock path became a reparse point: \"%1\"")
                    .arg(layout->lock_path);
            }
            return false;
        }

        const Capture_scan scan = scan_capture(*layout, true);
        if (!scan.recovery.valid()) {
            m_error = scan.recovery.error;
            return false;
        }

        m_layout   = *layout;
        m_segments = scan.segments;
        if (!m_segments.empty()) {
            const std::uint64_t last_sequence = m_segments.back().public_segment.sequence;
            if (last_sequence == std::numeric_limits<std::uint64_t>::max()) {
                m_error = QStringLiteral(
                    "backend output capture segment sequence is exhausted");
                return false;
            }
            m_next_sequence = last_sequence + 1U;
        }
        m_initialized = true;
        return true;
    }

    bool invalidate_completion_manifest()
    {
        if (m_completion_invalidated) {
            return true;
        }
        if (!remove_capture_file(m_layout.completion_path, m_error)) {
            return false;
        }
        m_completion_invalidated = true;
        return true;
    }

    bool write_and_flush(
        QFile&         file,
        QByteArrayView bytes)
    {
        const qint64 requested_bytes = static_cast<qint64>(bytes.size());
#if defined(VNM_TERMINAL_CAPTURE_TEST_HOOKS)
        if (m_test_fault == Backend_output_capture_test_fault::PARTIAL_WRITE) {
            m_test_fault = Backend_output_capture_test_fault::NONE;
            const qint64 partial_bytes = std::max<qint64>(1, requested_bytes / 2);
            const qint64 written_bytes = file.write(bytes.data(), partial_bytes);
            (void)file.flush();
            m_error = QStringLiteral(
                "backend output capture injected partial write failure for \"%1\": "
                "wrote %2 of %3 bytes")
                .arg(file.fileName())
                .arg(written_bytes)
                .arg(requested_bytes);
            return false;
        }
#endif
        const qint64 written_bytes = file.write(bytes.data(), requested_bytes);
        if (written_bytes != requested_bytes) {
            m_error = QStringLiteral(
                "backend output capture write failed for \"%1\": wrote %2 of %3 bytes: %4")
                .arg(file.fileName())
                .arg(written_bytes)
                .arg(requested_bytes)
                .arg(file.errorString());
            return false;
        }
        const bool flushed = file.flush();
#if defined(VNM_TERMINAL_CAPTURE_TEST_HOOKS)
        if (m_test_fault == Backend_output_capture_test_fault::FLUSH_AFTER_WRITE) {
            m_test_fault = Backend_output_capture_test_fault::NONE;
            m_error = QStringLiteral(
                "backend output capture injected flush failure for \"%1\"")
                .arg(file.fileName());
            return false;
        }
#endif
        if (!flushed) {
            m_error = QStringLiteral(
                "backend output capture flush failed for \"%1\": %2")
                .arg(file.fileName(), file.errorString());
            return false;
        }
        return true;
    }

    bool open_and_write_new_segment(QByteArrayView bytes)
    {
        if (m_next_sequence == std::numeric_limits<std::uint64_t>::max()) {
            m_error = QStringLiteral(
                "backend output capture segment sequence is exhausted");
            return false;
        }

        const QString path = segment_path(m_layout, m_next_sequence);
        auto file = std::make_unique<QFile>(path);
        if (!file->open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            m_error = QStringLiteral(
                "backend output capture open failed for \"%1\": %2")
                .arg(path, file->errorString());
            return false;
        }
        if (!opened_file_is_safe(*file, path, m_error)) {
            return false;
        }
        if (!write_and_flush(*file, bytes)) {
            return false;
        }

        Disk_segment segment;
        segment.public_segment.path       = path;
        segment.public_segment.sequence   = m_next_sequence;
        segment.public_segment.byte_count = static_cast<std::size_t>(bytes.size());
        segment.finalized_path            = finalized_path(path);
        m_segments.push_back(std::move(segment));
        m_current_file = std::move(file);
        ++m_next_sequence;
        return true;
    }

    bool finalize_current_segment()
    {
        if (!m_current_file) {
            return true;
        }
        if (!m_current_file->flush()) {
            m_error = QStringLiteral(
                "backend output capture flush failed for \"%1\": %2")
                .arg(m_current_file->fileName(), m_current_file->errorString());
            return false;
        }
        m_current_file->close();
        if (!parent_path_is_safe(m_layout.parent_path, m_error)) {
            return false;
        }
        if (!write_finalized_marker(m_segments.back().public_segment.path, m_error)) {
            return false;
        }
        m_segments.back().public_segment.finalized = true;
        m_current_file.reset();
#if defined(VNM_TERMINAL_CAPTURE_TEST_HOOKS)
        if (m_test_fault ==
            Backend_output_capture_test_fault::FAIL_AFTER_SEGMENT_FINALIZATION)
        {
            m_test_fault = Backend_output_capture_test_fault::NONE;
            m_error = QStringLiteral(
                "backend output capture injected rollover-boundary failure");
            return false;
        }
#endif
        return true;
    }

    bool prune_retained_segments()
    {
        const std::size_t keep_count = retained_segment_limit(m_config.max_bytes);
        while (m_segments.size() > keep_count) {
            if (!prune_segment(m_segments.front(), m_error)) {
                return false;
            }
            m_segments.erase(m_segments.begin());
        }
        return true;
    }

    Backend_output_capture_writer_result replace_with_suffix(QByteArrayView suffix)
    {
        if (m_current_file) {
            m_current_file->close();
            m_current_file.reset();
        }

        qsizetype offset = 0;
        while (offset < suffix.size()) {
            const std::size_t capacity = segment_capacity(m_config.max_bytes, m_next_sequence);
            const qsizetype write_size = std::min(
                suffix.size() - offset,
                static_cast<qsizetype>(capacity));
            if (!open_and_write_new_segment(suffix.sliced(offset, write_size))) {
                return reject(m_error);
            }
            offset += write_size;
            if (offset < suffix.size() && !finalize_current_segment()) {
                return reject(m_error);
            }
        }

        if (!prune_retained_segments()) {
            return reject(m_error);
        }
        return accept();
    }

    std::mutex                          m_mutex;
    Backend_output_capture_config       m_config;
    Capture_layout                      m_layout;
    std::vector<Disk_segment>           m_segments;
    std::unique_ptr<QFile>              m_current_file;
    std::unique_ptr<QLockFile>           m_writer_lock;
    QString                             m_error;
    std::uint64_t                       m_next_sequence = 1U;
    bool                                m_initialized   = false;
    bool                                m_finalized     = false;
    bool                                m_completion_invalidated = false;
    bool                                m_poisoned = false;
#if defined(VNM_TERMINAL_CAPTURE_TEST_HOOKS)
    Backend_output_capture_test_fault   m_test_fault =
        Backend_output_capture_test_fault::NONE;
#endif
};

Backend_output_capture_writer::Backend_output_capture_writer(
    Backend_output_capture_config config)
:
    m_state(std::make_unique<State>(std::move(config)))
{}

Backend_output_capture_writer::~Backend_output_capture_writer()
= default;

Backend_output_capture_writer_result Backend_output_capture_writer::append(
    QByteArrayView bytes)
{
    return m_state->append(bytes);
}

Backend_output_capture_writer_result Backend_output_capture_writer::finalize()
{
    return m_state->finalize();
}

#if defined(VNM_TERMINAL_CAPTURE_TEST_HOOKS)
void Backend_output_capture_writer::set_test_fault(
    Backend_output_capture_test_fault fault)
{
    m_state->set_test_fault(fault);
}
#endif

}
