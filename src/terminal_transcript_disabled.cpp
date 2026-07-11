#include "vnm_terminal/internal/terminal_transcript.h"

#include <utility>

namespace vnm_terminal::internal {

void insert_wheel_trace_scroll_publication_fields(
    QJsonObject& object,
    bool         local_scroll_applied,
    bool         render_publication_blocked,
    std::optional<bool> visible_scroll_applied,
    bool         deferred_intent_recorded)
{
    object.insert(QStringLiteral("local_scroll_applied"), local_scroll_applied);
    // Public-projection scroll publication passes an explicit visible result.
    object.insert(
        QStringLiteral("visible_scroll_applied"),
        visible_scroll_applied.value_or(
            local_scroll_applied && !render_publication_blocked));
    object.insert(QStringLiteral("deferred_intent_recorded"), deferred_intent_recorded);
}

namespace {

QString transcript_disabled_message()
{
    return QStringLiteral("transcript capture/replay is disabled in this build");
}

}

std::shared_ptr<Terminal_transcript_recorder> Terminal_transcript_recorder::create(
    const QString&,
    bool,
    bool,
    QString* out_error)
{
    if (out_error != nullptr) {
        *out_error = transcript_disabled_message();
    }
    return nullptr;
}

std::shared_ptr<Terminal_transcript_recorder> Terminal_transcript_recorder::create(
    const QString& path,
    bool           snapshot_diagnostics,
    QString*       out_error)
{
    return create(path, snapshot_diagnostics, false, out_error);
}

Terminal_transcript_recorder::Terminal_transcript_recorder(
    QString path,
    bool    snapshot_diagnostics,
    bool    timing_diagnostics)
:
    m_path(std::move(path)),
    m_error_message(transcript_disabled_message()),
    m_snapshot_diagnostics(snapshot_diagnostics),
    m_timing_diagnostics(timing_diagnostics),
    m_failed(true)
{}

Terminal_transcript_recorder::~Terminal_transcript_recorder() = default;

QString Terminal_transcript_recorder::path() const
{
    return m_path;
}

bool Terminal_transcript_recorder::snapshot_diagnostics_enabled() const
{
    return false;
}

bool Terminal_transcript_recorder::timing_diagnostics_enabled() const
{
    return false;
}

bool Terminal_transcript_recorder::failed() const
{
    return true;
}

QString Terminal_transcript_recorder::error_message() const
{
    return m_error_message;
}

bool Terminal_transcript_recorder::record_session_start(
    std::uint64_t,
    const Terminal_launch_config&,
    const Terminal_session_config&)
{
    return false;
}

bool Terminal_transcript_recorder::record_backend_output(
    std::uint64_t,
    QByteArrayView)
{
    return false;
}

bool Terminal_transcript_recorder::record_host_write(
    std::uint64_t,
    const QString&,
    QByteArrayView)
{
    return false;
}

bool Terminal_transcript_recorder::record_session_resize(
    std::uint64_t,
    const Terminal_resize_transaction&)
{
    return false;
}

bool Terminal_transcript_recorder::record_session_resize_request(
    std::uint64_t,
    const Terminal_resize_transaction&)
{
    return false;
}

bool Terminal_transcript_recorder::record_text_area_resize_request(
    std::uint64_t,
    terminal_grid_size_t)
{
    return false;
}

bool Terminal_transcript_recorder::record_session_process_exit(
    std::uint64_t,
    const Terminal_backend_exit&)
{
    return false;
}

bool Terminal_transcript_recorder::record_session_backend_error(
    std::uint64_t,
    const Terminal_backend_error&)
{
    return false;
}

bool Terminal_transcript_recorder::record_surface_scroll(
    const Terminal_transcript_surface_scroll_event&)
{
    return false;
}

bool Terminal_transcript_recorder::record_surface_scroll_intent(
    const Terminal_transcript_surface_scroll_intent_event&)
{
    return false;
}

bool Terminal_transcript_recorder::record_surface_wheel_trace(QJsonObject)
{
    return false;
}

bool Terminal_transcript_recorder::record_surface_wheel_ingress(QJsonObject)
{
    return false;
}

bool Terminal_transcript_recorder::record_surface_selection_drag(
    const Terminal_transcript_surface_selection_drag_event&)
{
    return false;
}

bool Terminal_transcript_recorder::record_snapshot(
    std::uint64_t,
    const QString&,
    const Terminal_render_snapshot&)
{
    return false;
}

bool Terminal_transcript_recorder::open(QString*)
{
    return false;
}

bool Terminal_transcript_recorder::record_header()
{
    return false;
}

bool Terminal_transcript_recorder::write_event(QJsonObject)
{
    return false;
}

std::optional<std::vector<Terminal_transcript_event>> read_terminal_transcript(
    const QString&,
    QString* out_error)
{
    if (out_error != nullptr) {
        *out_error = transcript_disabled_message();
    }
    return std::nullopt;
}

}
