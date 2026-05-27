#pragma once

#include "vnm_terminal/internal/render_snapshot.h"
#include "vnm_terminal/internal/session_contract.h"
#include <QByteArray>
#include <QFile>
#include <QJsonObject>
#include <QString>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <cstdint>

namespace vnm_terminal::internal {

struct Terminal_transcript_event
{
    std::uint64_t event_index = 0U;
    QString       kind;
    QJsonObject   object;
};

struct Terminal_transcript_surface_scroll_event
{
    QString                         source;
    int                             requested_line_delta = 0;
    std::optional<int>              requested_offset_from_tail;
    Terminal_viewport_scroll_result result;
    Terminal_viewport_state         viewport_before;
    Terminal_viewport_state         viewport_after;
};

struct Terminal_transcript_surface_scroll_intent_event
{
    QString                         source;
    int                             requested_line_delta = 0;
    std::optional<int>              requested_offset_from_tail;
    Terminal_viewport_state         viewport_before;
};

struct Terminal_transcript_surface_selection_drag_event
{
    QString                                 phase;
    std::optional<terminal_grid_position_t> anchor;
    std::optional<terminal_grid_position_t> focus;
    std::optional<Terminal_selection_range> range;
    bool                                    moved = false;
};

void insert_wheel_trace_scroll_publication_fields(
    QJsonObject& object,
    bool         local_scroll_applied,
    bool         render_publication_blocked);

class Terminal_transcript_recorder
{
public:
    struct Timing_context;
    struct Write_timing;

    static std::shared_ptr<Terminal_transcript_recorder> create(
        const QString& path,
        bool           snapshot_diagnostics,
        bool           timing_diagnostics,
        QString*       out_error);
    static std::shared_ptr<Terminal_transcript_recorder> create(
        const QString& path,
        bool           snapshot_diagnostics,
        QString*       out_error);

    ~Terminal_transcript_recorder();

    Terminal_transcript_recorder(const Terminal_transcript_recorder&)            = delete;
    Terminal_transcript_recorder& operator=(const Terminal_transcript_recorder&) = delete;

    QString path() const;
    bool snapshot_diagnostics_enabled() const;
    bool timing_diagnostics_enabled() const;
    bool failed() const;
    QString error_message() const;

    bool record_session_start(
        std::uint64_t                  session_sequence,
        const Terminal_launch_config&  launch_config,
        const Terminal_session_config& session_config);

    bool record_backend_output(
        std::uint64_t  session_sequence,
        QByteArrayView bytes);

    bool record_host_write(
        std::uint64_t  session_sequence,
        const QString& source,
        QByteArrayView bytes);

    bool record_session_resize(
        std::uint64_t                      session_sequence,
        const Terminal_resize_transaction& resize);

    bool record_session_resize_request(
        std::uint64_t                      session_sequence,
        const Terminal_resize_transaction& resize);

    bool record_text_area_resize_request(
        std::uint64_t        session_sequence,
        terminal_grid_size_t grid_size);

    bool record_session_process_exit(
        std::uint64_t                session_sequence,
        const Terminal_backend_exit& exit);

    bool record_session_backend_error(
        std::uint64_t                 session_sequence,
        const Terminal_backend_error& error);

    bool record_surface_scroll(
        const Terminal_transcript_surface_scroll_event& event);

    bool record_surface_scroll_intent(
        const Terminal_transcript_surface_scroll_intent_event& event);

    bool record_surface_wheel_trace(
        QJsonObject object);

    bool record_surface_wheel_ingress(
        QJsonObject object);

    bool record_surface_selection_drag(
        const Terminal_transcript_surface_selection_drag_event& event);

    bool record_snapshot(
        std::uint64_t                   session_sequence,
        const QString&                  reason,
        const Terminal_render_snapshot& snapshot);

private:
    Terminal_transcript_recorder(
        QString    path,
        bool       snapshot_diagnostics,
        bool       timing_diagnostics);

    bool open(QString* out_error);
    bool record_header();
    bool write_event(QJsonObject object);
    bool write_event(QJsonObject object, const Timing_context& timing_context);
    bool write_event_locked(QJsonObject object, Write_timing* timing);

    mutable std::mutex m_mutex;
    QString            m_path;
    QFile              m_file;
    QString            m_error_message;
    std::uint64_t      m_next_event_index     = 0U;
    bool               m_snapshot_diagnostics = false;
    bool               m_timing_diagnostics   = false;
    bool               m_failed               = false;
};

std::optional<std::vector<Terminal_transcript_event>> read_terminal_transcript(
    const QString& path,
    QString*       out_error);

}
