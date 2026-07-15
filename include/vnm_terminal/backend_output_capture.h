#pragma once

#include <QString>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vnm_terminal {

/**
 * Bounded raw backend-output capture configuration.
 *
 * `base_path` is a file-name prefix in an existing trusted local directory.
 * UNC paths and Windows drives reported as remote are rejected. Capture files
 * are direct siblings of that prefix; the prefix itself is never opened.
 * `max_bytes` is a hard aggregate bound across the retained raw segments and
 * must be positive. A capture normally retains the newest two consecutive
 * segments. Their concatenation is the exact retained suffix of accepted raw
 * backend bytes. Keep `max_bytes` stable while files for a base prefix remain.
 *
 * An accepted chunk is flushed through Qt's file API before the writer returns.
 * This makes the chunk visible to another process and recoverable after process
 * termination. It is not a power-loss durability guarantee.
 */
struct Backend_output_capture_config
{
    QString                     base_path;
    std::size_t                 max_bytes = 0U;

    bool operator==(const Backend_output_capture_config&) const = default;
};

enum class Backend_output_capture_status : std::uint8_t
{
    EMPTY,
    FINALIZED,
    INCOMPLETE,
    INVALID,
};

struct Backend_output_capture_segment
{
    QString                     path;
    std::uint64_t               sequence    = 0U;
    std::size_t                 byte_count  = 0U;
    bool                        finalized   = false;
};

struct Backend_output_capture_recovery
{
    Backend_output_capture_status            status = Backend_output_capture_status::INVALID;
    std::vector<Backend_output_capture_segment>
                                             segments;
    std::size_t                              retained_bytes = 0U;
    QString                                  error;

    bool valid() const
    {
        return status != Backend_output_capture_status::INVALID;
    }
};

enum class Backend_output_capture_artifact_kind : std::uint8_t
{
    UNRECOGNIZED,
    RAW_SEGMENT,
    SEGMENT_FINALIZATION_MARKER,
    COMPLETION_MANIFEST,
    WRITER_LOCK,
};

struct Backend_output_capture_artifact_inspection
{
    Backend_output_capture_artifact_kind kind =
        Backend_output_capture_artifact_kind::UNRECOGNIZED;
    std::uint64_t                       sequence   = 0U;
    std::uint64_t                       byte_count = 0U;
    QString                             error;

    bool recognized() const
    {
        return kind != Backend_output_capture_artifact_kind::UNRECOGNIZED;
    }

    bool valid() const
    {
        return recognized() && error.isEmpty();
    }
};

/**
 * Classifies and validates one existing capture-owned artifact.
 *
 * `artifact_path` must be a direct sibling of `base_path`. Known raw segments
 * and the writer lock must be safe regular files. Segment-finalization and
 * capture-completion markers must additionally contain the exact marker bytes
 * owned by this library. Unrecognized names are not capture-owned and return
 * `UNRECOGNIZED` without an error.
 */
Backend_output_capture_artifact_inspection inspect_backend_output_capture_artifact(
    const QString& base_path,
    const QString& artifact_path);

/**
 * Validates and recovers a capture in sequence order.
 *
 * A crash may leave one extra oldest segment after newer data was flushed and
 * before pruning completed. Recovery removes such extra segments when it can
 * acquire the capture lock and returns at most the newest two (one when
 * `max_bytes == 1`). Parent components, recognized files, and opened Windows
 * objects are checked for symbolic-link/reparse redirection. Callers must keep
 * the trusted parent directory protected from hostile rename/replacement races.
 */
Backend_output_capture_recovery recover_backend_output_capture(
    const Backend_output_capture_config& config);

}
