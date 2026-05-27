#include "helpers/test_check.h"
#include "vnm_terminal/internal/terminal_session.h"
#include "vnm_terminal/internal/terminal_transcript.h"

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <algorithm>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

constexpr int k_surface_default_scrollback_limit = 10000;

term::Terminal_launch_config valid_launch_config()
{
    term::Terminal_launch_config config;
    config.argv              = {QStringLiteral("transcript-fixture"), QStringLiteral("--manual")};
    config.working_directory = QStringLiteral("C:/workspace");
    config.initial_grid_size = term::terminal_grid_size_t{24, 80};
    return config;
}

void insert_u64(QJsonObject& object, const QString& name, std::uint64_t value)
{
    object.insert(name, static_cast<qint64>(value));
}

QByteArray json_line(QJsonObject object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + QByteArrayLiteral("\n");
}

QJsonObject valid_header_object(std::uint64_t event_index = 0U)
{
    QJsonObject object;
    object.insert(QStringLiteral("kind"), QStringLiteral("header"));
    object.insert(QStringLiteral("schema"), QStringLiteral("vnm_terminal.session_surface_transcript"));
    object.insert(QStringLiteral("schema_version"), 1);
    object.insert(QStringLiteral("byte_encoding"), QStringLiteral("base64"));
    object.insert(QStringLiteral("snapshot_diagnostics"), false);
    insert_u64(object, QStringLiteral("event_index"), event_index);
    return object;
}

QJsonObject valid_session_start_object(std::uint64_t event_index)
{
    QJsonObject grid_size;
    grid_size.insert(QStringLiteral("rows"), 3);
    grid_size.insert(QStringLiteral("columns"), 12);

    QJsonObject object;
    object.insert(QStringLiteral("kind"), QStringLiteral("session.start"));
    insert_u64(object, QStringLiteral("event_index"), event_index);
    insert_u64(object, QStringLiteral("session_sequence"), 1U);
    object.insert(QStringLiteral("argv"), QJsonArray{QStringLiteral("fixture")});
    object.insert(QStringLiteral("working_directory"), QStringLiteral("C:/workspace"));
    object.insert(QStringLiteral("initial_grid_size"), grid_size);
    return object;
}

QJsonObject valid_compact_snapshot_object(std::uint64_t event_index)
{
    QJsonObject grid_size;
    grid_size.insert(QStringLiteral("rows"), 1);
    grid_size.insert(QStringLiteral("columns"), 4);

    QJsonObject viewport;
    viewport.insert(QStringLiteral("active_buffer"), QStringLiteral("primary"));
    viewport.insert(QStringLiteral("scrollback_rows"), 0);
    viewport.insert(QStringLiteral("visible_rows"), 1);
    viewport.insert(QStringLiteral("offset_from_tail"), 0);
    viewport.insert(QStringLiteral("follow_tail"), true);
    viewport.insert(
        QStringLiteral("alternate_screen_scroll_policy"),
        QStringLiteral("keep_at_tail"));

    QJsonObject cursor;
    cursor.insert(QStringLiteral("visible"), true);
    cursor.insert(QStringLiteral("position"), QJsonObject{
        {QStringLiteral("row"),    0},
        {QStringLiteral("column"), 0},
    });

    QJsonObject object;
    object.insert(QStringLiteral("kind"), QStringLiteral("snapshot"));
    insert_u64(object, QStringLiteral("event_index"), event_index);
    insert_u64(object, QStringLiteral("session_sequence"), 1U);
    object.insert(QStringLiteral("reason"), QStringLiteral("fixture"));
    object.insert(QStringLiteral("mode"), QStringLiteral("compact"));
    object.insert(QStringLiteral("grid_size"), grid_size);
    object.insert(QStringLiteral("viewport"), viewport);
    object.insert(QStringLiteral("cursor"), cursor);
    insert_u64(object, QStringLiteral("snapshot_sequence"), 1U);
    insert_u64(object, QStringLiteral("row_origin_generation"), 1U);
    object.insert(QStringLiteral("cell_count"), 0);
    object.insert(QStringLiteral("dirty_row_range_count"), 1);
    object.insert(QStringLiteral("selection_span_count"), 0);
    object.insert(QStringLiteral("backend_geometry_in_sync"), true);
    object.insert(QStringLiteral("visible_rows"), QJsonArray{
        QJsonObject{
            {QStringLiteral("row"),    0},
            {QStringLiteral("text"),   QStringLiteral("test")},
            {QStringLiteral("hash64"), QStringLiteral("fixture-visible-hash")},
        },
    });
    object.insert(QStringLiteral("row_provenance"), QJsonArray{
        QJsonObject{
            {QStringLiteral("row"),                0},
            {QStringLiteral("logical_row"),        0},
            {QStringLiteral("retained_line_id"),   QStringLiteral("1")},
            {QStringLiteral("content_generation"), QStringLiteral("1")},
        },
    });
    object.insert(QStringLiteral("dirty_row_ranges"), QJsonArray{
        QJsonObject{
            {QStringLiteral("first_row"), 0},
            {QStringLiteral("row_count"), 1},
        },
    });
    object.insert(QStringLiteral("selection_spans"), QJsonArray{});
    object.insert(QStringLiteral("selected_text"), QJsonObject{
        {QStringLiteral("available"), false},
        {QStringLiteral("result"),    QStringLiteral("no_selection")},
    });
    return object;
}

QByteArray event_bytes(const term::Terminal_transcript_event& event)
{
    const QByteArray::FromBase64Result decoded = QByteArray::fromBase64Encoding(
        event.object.value(QStringLiteral("bytes_base64")).toString().toLatin1(),
        QByteArray::AbortOnBase64DecodingErrors);
    return decoded.decoded;
}

std::optional<bool> focus_state_from_host_write_bytes(const QByteArray& bytes)
{
    if (bytes == QByteArrayLiteral("\x1b[I")) {
        return true;
    }
    if (bytes == QByteArrayLiteral("\x1b[O")) {
        return false;
    }
    return std::nullopt;
}

std::optional<term::Terminal_transcript_event> first_event(
    const std::vector<term::Terminal_transcript_event>& events,
    const QString&                                      kind)
{
    const auto it = std::find_if(
        events.begin(),
        events.end(),
        [&kind](const term::Terminal_transcript_event& event) {
            return event.kind == kind;
        });
    if (it == events.end()) {
        return std::nullopt;
    }

    return *it;
}

std::optional<std::size_t> first_event_position(
    const std::vector<term::Terminal_transcript_event>& events,
    const QString&                                      kind)
{
    for (std::size_t i = 0U; i < events.size(); ++i) {
        if (events[i].kind == kind) {
            return i;
        }
    }

    return std::nullopt;
}

std::optional<std::size_t> first_event_position_after(
    const std::vector<term::Terminal_transcript_event>& events,
    const QString&                                      kind,
    std::size_t                                         first_index)
{
    for (std::size_t i = first_index; i < events.size(); ++i) {
        if (events[i].kind == kind) {
            return i;
        }
    }

    return std::nullopt;
}

std::optional<std::size_t> first_event_position_with_bytes(
    const std::vector<term::Terminal_transcript_event>& events,
    const QString&                                      kind,
    const QByteArray&                                   bytes)
{
    for (std::size_t i = 0U; i < events.size(); ++i) {
        if (events[i].kind == kind && event_bytes(events[i]) == bytes) {
            return i;
        }
    }

    return std::nullopt;
}

int event_count(
    const std::vector<term::Terminal_transcript_event>& events,
    const QString&                                      kind)
{
    return static_cast<int>(
        std::count_if(
            events.begin(),
            events.end(),
            [&kind](const term::Terminal_transcript_event& event) {
                return event.kind == kind;
            }));
}

bool write_transcript_lines(
    const QString&                 path,
    const std::vector<QByteArray>& lines)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }

    for (const QByteArray& line : lines) {
        if (file.write(line) != line.size()) {
            return false;
        }
    }

    return true;
}

struct Replay_tool_process_result
{
    bool                 finished    = false;
    QProcess::ExitStatus exit_status = QProcess::CrashExit;
    int                  exit_code   = -1;
    QByteArray           stdout_text;
    QByteArray           stderr_text;
};

Replay_tool_process_result run_replay_tool(
    const QString& replay_tool_path,
    const QString& transcript_path)
{
    Replay_tool_process_result result;
    QProcess process;
    process.start(replay_tool_path, {transcript_path});
    result.finished = process.waitForFinished(10000);
    if (!result.finished) {
        process.kill();
        (void)process.waitForFinished(1000);
    }
    result.exit_status = process.exitStatus();
    result.exit_code   = process.exitCode();
    result.stdout_text = process.readAllStandardOutput();
    result.stderr_text = process.readAllStandardError();
    return result;
}

void print_replay_tool_output(const Replay_tool_process_result& result)
{
    std::cerr << result.stdout_text.constData() << result.stderr_text.constData();
}

bool rewrite_first_snapshot_dirty_ranges(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    bool rewritten = false;
    std::vector<QByteArray> lines;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine();
        QJsonParseError parse_error;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
            return false;
        }

        QJsonObject object = document.object();
        if (!rewritten && object.value(QStringLiteral("kind")).toString() ==
            QStringLiteral("snapshot"))
        {
            object.insert(QStringLiteral("dirty_row_range_count"), 1);
            object.insert(QStringLiteral("dirty_row_ranges"), QJsonArray{
                QJsonObject{
                    {QStringLiteral("first_row"), 1},
                    {QStringLiteral("row_count"), 1},
                },
            });
            rewritten = true;
        }
        lines.push_back(json_line(object));
    }
    file.close();

    return rewritten && write_transcript_lines(path, lines);
}

QJsonObject transcript_timing_diagnostic_object(
    const QString& record_kind,
    const QString& operation,
    std::uint64_t  record_event_index)
{
    QJsonObject object;
    object.insert(QStringLiteral("kind"), QStringLiteral("transcript.timing"));
    object.insert(QStringLiteral("operation"), operation);
    object.insert(QStringLiteral("record_kind"), record_kind);
    insert_u64(object, QStringLiteral("record_event_index"), record_event_index);
    object.insert(QStringLiteral("object_construction_elapsed_ns"), 0);
    object.insert(QStringLiteral("json_serialization_elapsed_ns"), 0);
    object.insert(QStringLiteral("ndjson_append_elapsed_ns"), 0);
    object.insert(QStringLiteral("file_write_elapsed_ns"), 0);
    object.insert(QStringLiteral("file_flush_elapsed_ns"), 0);
    object.insert(QStringLiteral("write_event_elapsed_ns"), 0);
    object.insert(QStringLiteral("total_elapsed_ns"), 0);
    object.insert(QStringLiteral("ndjson_byte_count"), 0);
    return object;
}

QJsonObject wheel_ingress_diagnostic_object()
{
    QJsonObject object;
    object.insert(QStringLiteral("kind"), QStringLiteral("surface.wheel_ingress"));
    object.insert(QStringLiteral("source"), QStringLiteral("app.scrollbar"));
    object.insert(QStringLiteral("phase"), QStringLiteral("ingress"));
    object.insert(QStringLiteral("accepted_on_entry"), true);
    object.insert(QStringLiteral("angle_delta_x"), 0);
    object.insert(QStringLiteral("angle_delta_y"), -120);
    object.insert(QStringLiteral("pixel_delta_x"), 0);
    object.insert(QStringLiteral("pixel_delta_y"), 0);
    object.insert(QStringLiteral("modifiers"), 0);
    object.insert(QStringLiteral("position_x"), 4.0);
    object.insert(QStringLiteral("position_y"), 8.0);
    return object;
}

bool insert_replay_transparent_diagnostics_around_first_snapshot(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    bool inserted_pre_start_diagnostic = false;
    bool inserted_snapshot_diagnostics = false;
    std::vector<QJsonObject> objects;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine();
        QJsonParseError parse_error;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
            return false;
        }

        QJsonObject object = document.object();
        const QString kind = object.value(QStringLiteral("kind")).toString();
        if (!inserted_pre_start_diagnostic && kind == QStringLiteral("session.start")) {
            objects.push_back(transcript_timing_diagnostic_object(
                QStringLiteral("header"),
                QStringLiteral("event_write"),
                0U));
            inserted_pre_start_diagnostic = true;
        }

        if (!inserted_snapshot_diagnostics && kind ==
            QStringLiteral("snapshot"))
        {
            QJsonObject intermediate = object;
            QJsonArray visible_rows =
                intermediate.value(QStringLiteral("visible_rows")).toArray();
            if (visible_rows.isEmpty()) {
                return false;
            }

            QJsonObject first_row = visible_rows.at(0).toObject();
            first_row.insert(QStringLiteral("text"), QStringLiteral("intermediate"));
            first_row.insert(QStringLiteral("hash64"), QStringLiteral("intermediate-diagnostic-hash"));
            visible_rows.replace(0, first_row);
            intermediate.insert(QStringLiteral("visible_rows"), visible_rows);
            objects.push_back(std::move(intermediate));
            objects.push_back(transcript_timing_diagnostic_object(
                QStringLiteral("snapshot"),
                QStringLiteral("snapshot_diagnostic_emit"),
                0U));
            objects.push_back(wheel_ingress_diagnostic_object());
            inserted_snapshot_diagnostics = true;
        }
        objects.push_back(std::move(object));
    }
    file.close();

    std::vector<QByteArray> lines;
    lines.reserve(objects.size());
    for (std::size_t index = 0U; index < objects.size(); ++index) {
        objects[index].insert(
            QStringLiteral("event_index"),
            static_cast<qint64>(index));
        lines.push_back(json_line(std::move(objects[index])));
    }

    return
        inserted_pre_start_diagnostic &&
        inserted_snapshot_diagnostics &&
        write_transcript_lines(path, lines);
}

bool expect_reader_failure(
    QTemporaryDir&                 temp_dir,
    const QString&                 file_name,
    const std::vector<QByteArray>& lines,
    const QString&                 expected_error_text)
{
    const QString path = temp_dir.filePath(file_name);
    bool ok = true;
    ok &= check(write_transcript_lines(path, lines), "malformed transcript fixture writes");

    QString error;
    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(path, &error);
    ok &= check(!events.has_value(), "malformed transcript is rejected");
    ok &= check(error.contains(expected_error_text), "malformed transcript reports expected error");
    if (events.has_value() || !error.contains(expected_error_text)) {
        std::cerr << error.toStdString() << '\n';
    }
    return ok;
}

term::terminal_grid_size_t grid_size_from_object(
    const QJsonObject& object,
    const QString&     field_name)
{
    const QJsonObject grid_size = object.value(field_name).toObject();
    return {
        grid_size.value(QStringLiteral("rows")).toInt(),
        grid_size.value(QStringLiteral("columns")).toInt(),
    };
}

term::terminal_grid_position_t position_from_object(const QJsonObject& object)
{
    return {
        object.value(QStringLiteral("row")).toInt(),
        object.value(QStringLiteral("column")).toInt(),
    };
}

term::Terminal_selection_mode selection_mode_from_name(const QString& mode)
{
    if (mode == QStringLiteral("none")) {
        return term::Terminal_selection_mode::NONE;
    }
    if (mode == QStringLiteral("word")) {
        return term::Terminal_selection_mode::WORD;
    }
    if (mode == QStringLiteral("line")) {
        return term::Terminal_selection_mode::LINE;
    }

    return term::Terminal_selection_mode::NORMAL;
}

term::Terminal_selection_range selection_range_from_object(const QJsonObject& object)
{
    term::Terminal_selection_range range;
    range.start = position_from_object(object.value(QStringLiteral("start")).toObject());
    range.end   = position_from_object(object.value(QStringLiteral("end")).toObject());
    range.mode  = selection_mode_from_name(object.value(QStringLiteral("mode")).toString());
    return range;
}

std::optional<term::Terminal_selection_range> selection_range_from_event(
    const term::Terminal_transcript_event& event)
{
    if (!event.object.contains(QStringLiteral("range"))) {
        return std::nullopt;
    }

    return selection_range_from_object(event.object.value(QStringLiteral("range")).toObject());
}
term::Terminal_launch_config launch_config_from_event(
    const term::Terminal_transcript_event& event)
{
    term::Terminal_launch_config config;
    const QJsonArray argv = event.object.value(QStringLiteral("argv")).toArray();
    for (const QJsonValue& argument : argv) {
        config.argv.push_back(argument.toString());
    }
    config.working_directory = event.object.value(QStringLiteral("working_directory")).toString();
    if (event.object.contains(QStringLiteral("initial_grid_size"))) {
        config.initial_grid_size = grid_size_from_object(event.object, QStringLiteral("initial_grid_size"));
    }
    return config;
}

QSizeF source_geometry_from_event(const term::Terminal_transcript_event& event)
{
    return QSizeF(
        event.object.value(QStringLiteral("source_width")).toDouble(),
        event.object.value(QStringLiteral("source_height")).toDouble());
}

QString scroll_action_name(term::Terminal_viewport_scroll_action action)
{
    switch (action) {
        case term::Terminal_viewport_scroll_action::VIEWPORT_MOVED:
            return QStringLiteral("viewport_moved");
        case term::Terminal_viewport_scroll_action::AT_BOUNDARY:
            return QStringLiteral("at_boundary");
        case term::Terminal_viewport_scroll_action::TERMINAL_INPUT:
            return QStringLiteral("terminal_input");
    }

    return QStringLiteral("unknown");
}

QString snapshot_row_text(const term::Terminal_render_snapshot& snapshot, int row)
{
    const std::vector<const term::Terminal_render_cell*> cells_by_position =
        term::render_snapshot_cells_by_position(snapshot);
    return term::selected_text_from_render_snapshot_row(
        snapshot,
        cells_by_position,
        row,
        0,
        snapshot.grid_size.columns,
        true);
}

QStringList snapshot_visible_rows(const term::Terminal_render_snapshot& snapshot)
{
    QStringList rows;
    for (int row = 0; row < snapshot.grid_size.rows; ++row) {
        rows.push_back(snapshot_row_text(snapshot, row));
    }
    return rows;
}

class Scripted_backend final : public term::Terminal_backend
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

        running     = true;
        m_callbacks = std::move(callbacks);
        return term::backend_accept();
    }

    term::Terminal_backend_result write(QByteArray bytes) override
    {
        if (!running) {
            return term::backend_reject(
                term::Terminal_backend_error_code::WRITE_FAILED,
                QStringLiteral("scripted backend is not running"));
        }

        writes.push_back(std::move(bytes));
        if (!output_on_write.isEmpty()) {
            emit_output(output_on_write);
        }
        return term::backend_accept();
    }

    term::Terminal_backend_result resize(term::Terminal_backend_resize_request request) override
    {
        resizes.push_back(request);
        if (!output_on_resize.isEmpty()) {
            emit_output(output_on_resize);
        }
        return term::backend_accept();
    }

    term::Terminal_backend_result set_output_paused(bool) override { return term::backend_accept(); }
    term::Terminal_backend_result interrupt() override             { return term::backend_accept(); }
    term::Terminal_backend_result terminate() override             { return term::backend_accept(); }

    void emit_output(QByteArray bytes)
    {
        m_callbacks.output_received(std::move(bytes));
    }

    void emit_error(term::Terminal_backend_error error)
    {
        m_callbacks.error_reported(std::move(error));
    }

    void emit_exit(term::Terminal_backend_exit exit)
    {
        running = false;
        m_callbacks.process_exited(exit);
    }

    bool running = false;
    QByteArray output_on_write;
    QByteArray output_on_resize;
    std::vector<QByteArray> writes;
    std::vector<term::Terminal_backend_resize_request> resizes;

private:
    term::Terminal_backend_callbacks m_callbacks;
};

struct Replay_harness
{
    QString error;
    std::vector<QByteArray> host_writes;
    int skipped_terminal_reply_host_writes = 0;
    std::optional<term::Terminal_render_snapshot> snapshot;
    term::Terminal_viewport_state viewport;
    term::Terminal_selection_result selected_text;
};

int max_recorded_scrollback_rows(const std::vector<term::Terminal_transcript_event>& events)
{
    int max_scrollback_rows = 0;
    for (const term::Terminal_transcript_event& event : events) {
        if (event.kind != QStringLiteral("snapshot")) {
            continue;
        }

        const QJsonObject viewport = event.object.value(QStringLiteral("viewport")).toObject();
        max_scrollback_rows = std::max(
            max_scrollback_rows,
            viewport.value(QStringLiteral("scrollback_rows")).toInt());
    }
    return max_scrollback_rows;
}

term::Terminal_session_config replay_session_config(
    const std::vector<term::Terminal_transcript_event>& events)
{
    term::Terminal_session_config config;
    const auto start_it = std::find_if(
        events.begin(),
        events.end(),
        [](const term::Terminal_transcript_event& event) {
            return event.kind == QStringLiteral("session.start");
        });

    if (start_it != events.end() && start_it->object.contains(QStringLiteral("session_config"))) {
        const QJsonObject session_config =
            start_it->object.value(QStringLiteral("session_config")).toObject();
        config.scrollback_limit =
            session_config.contains(QStringLiteral("effective_scrollback_limit"))
                ? session_config.value(QStringLiteral("effective_scrollback_limit")).toInt()
                : session_config.value(QStringLiteral("scrollback_limit")).toInt();
        config.recover_scrollback_from_primary_repaints =
            session_config.value(QStringLiteral("recover_scrollback_from_primary_repaints")).toBool();
        return config;
    }

    const int recorded_scrollback_rows = max_recorded_scrollback_rows(events);
    config.scrollback_limit =
        recorded_scrollback_rows > 0 ? recorded_scrollback_rows : k_surface_default_scrollback_limit;
    config.recover_scrollback_from_primary_repaints = true;
    return config;
}

struct Pending_scroll_intent
{
    QString                               source;
    int                                   requested_line_delta = 0;
    std::optional<int>                    requested_offset_from_tail;
    term::Terminal_viewport_scroll_result result;
};

Pending_scroll_intent pending_scroll_intent_from_event(
    const term::Terminal_transcript_event&       event,
    const term::Terminal_viewport_scroll_result& result)
{
    Pending_scroll_intent pending;
    pending.source               = event.object.value(QStringLiteral("source")).toString();
    pending.requested_line_delta = event.object.value(QStringLiteral("requested_line_delta")).toInt();
    pending.result               = result;
    if (event.object.contains(QStringLiteral("requested_offset_from_tail"))) {
        pending.requested_offset_from_tail =
            event.object.value(QStringLiteral("requested_offset_from_tail")).toInt();
    }
    return pending;
}

bool scroll_event_matches_pending_intent(
    const term::Terminal_transcript_event& event,
    const Pending_scroll_intent&           pending)
{
    if (event.object.value(QStringLiteral("source")).toString() != pending.source ||
        event.object.value(QStringLiteral("requested_line_delta")).toInt() !=
            pending.requested_line_delta ||
        event.object.value(QStringLiteral("applied_line_delta")).toInt() !=
            pending.result.applied_line_delta ||
        event.object.value(QStringLiteral("action")).toString() !=
            scroll_action_name(pending.result.action))
    {
        return false;
    }

    const bool event_has_offset =
        event.object.contains(QStringLiteral("requested_offset_from_tail"));
    if (event_has_offset != pending.requested_offset_from_tail.has_value()) {
        return false;
    }
    if (pending.requested_offset_from_tail.has_value() &&
        event.object.value(QStringLiteral("requested_offset_from_tail")).toInt() !=
            *pending.requested_offset_from_tail)
    {
        return false;
    }
    return true;
}

bool viewport_offset_matches_recorded_after(
    const term::Terminal_viewport_state&   viewport,
    const term::Terminal_transcript_event& event)
{
    const QJsonObject viewport_after =
        event.object.value(QStringLiteral("viewport_after")).toObject();
    return viewport.offset_from_tail ==
        viewport_after.value(QStringLiteral("offset_from_tail")).toInt();
}

bool apply_replay_scroll_event(
    term::Terminal_session&                session,
    const term::Terminal_transcript_event& event,
    term::Terminal_viewport_scroll_result* out_result,
    QString*                               out_error)
{
    term::Terminal_viewport_scroll_result result;
    if (event.object.contains(QStringLiteral("requested_offset_from_tail"))) {
        result = session.scroll_published_viewport_to_offset_from_tail(
            event.object.value(QStringLiteral("requested_offset_from_tail")).toInt());
    }
    else {
        result = session.scroll_published_viewport_lines(
            event.object.value(QStringLiteral("requested_line_delta")).toInt());
    }

    if (out_result != nullptr) {
        *out_result = result;
    }
    if (result.action == term::Terminal_viewport_scroll_action::TERMINAL_INPUT) {
        *out_error = QStringLiteral("replay scroll requested terminal input");
        return false;
    }
    return true;
}

Replay_harness replay_events(const std::vector<term::Terminal_transcript_event>& events)
{
    Replay_harness replay;
    auto backend = std::make_unique<Scripted_backend>();
    Scripted_backend* backend_ptr = backend.get();
    term::Terminal_session session(std::move(backend), replay_session_config(events));

    const bool has_resize_request = std::any_of(
        events.begin(),
        events.end(),
        [](const term::Terminal_transcript_event& event) {
            return event.kind == QStringLiteral("session.resize_request");
        });

    bool started = false;
    std::optional<Pending_scroll_intent> pending_scroll_intent;
    for (const term::Terminal_transcript_event& event : events) {
        if (event.kind == QStringLiteral("header")) {
            continue;
        }

        if (event.kind == QStringLiteral("transcript.timing") ||
            event.kind == QStringLiteral("surface.wheel_ingress") ||
            event.kind == QStringLiteral("surface.wheel_trace"))
        {
            continue;
        }

        if (event.kind == QStringLiteral("session.start")) {
            const term::Terminal_session_result result =
                session.start(launch_config_from_event(event));
            if (result.code != term::Terminal_session_result_code::ACCEPTED) {
                replay.error = QStringLiteral("replay session.start was rejected");
                return replay;
            }
            started = true;
            continue;
        }

        if (!started) {
            replay.error = QStringLiteral("replay event appeared before session.start");
            return replay;
        }

        if (event.kind == QStringLiteral("backend.output")) {
            backend_ptr->emit_output(event_bytes(event));
            session.process_backend_callback_events();
        }
        else
        if (event.kind == QStringLiteral("host.write")) {
            if (event.object.value(QStringLiteral("source")).toString() ==
                QStringLiteral("terminal_reply"))
            {
                ++replay.skipped_terminal_reply_host_writes;
                continue;
            }

            const QByteArray bytes = event_bytes(event);
            const std::optional<bool> focused = focus_state_from_host_write_bytes(bytes);
            const term::Terminal_session_result result =
                focused.has_value()
                    ? session.write_focus_event(*focused).result
                    : session.write_user_bytes(bytes);
            if (result.code != term::Terminal_session_result_code::ACCEPTED) {
                replay.error = QStringLiteral("replay host.write was rejected");
                return replay;
            }
        }
        else
        if (event.kind == QStringLiteral("session.resize_request") ||
            (event.kind == QStringLiteral("session.resize") && !has_resize_request))
        {
            const term::Terminal_session_result result = session.resize(
                source_geometry_from_event(event),
                grid_size_from_object(event.object, QStringLiteral("target_grid_size")));
            if (result.code != term::Terminal_session_result_code::ACCEPTED) {
                replay.error = QStringLiteral("replay session.resize was rejected");
                return replay;
            }
        }
        else
        if (event.kind == QStringLiteral("surface.scroll_intent")) {
            term::Terminal_viewport_scroll_result result;
            if (!apply_replay_scroll_event(session, event, &result, &replay.error)) {
                return replay;
            }
            pending_scroll_intent.reset();
            if (result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED) {
                pending_scroll_intent = pending_scroll_intent_from_event(event, result);
            }
        }
        else
        if (event.kind == QStringLiteral("surface.scroll")) {
            const bool pending_result_already_applied =
                pending_scroll_intent.has_value() &&
                scroll_event_matches_pending_intent(event, *pending_scroll_intent) &&
                viewport_offset_matches_recorded_after(session.viewport_state(), event);
            pending_scroll_intent.reset();
            if (!pending_result_already_applied) {
                term::Terminal_viewport_scroll_result result;
                if (!apply_replay_scroll_event(session, event, &result, &replay.error)) {
                    return replay;
                }
            }
        }
        else
        if (event.kind == QStringLiteral("surface.selection_drag")) {
            const QString phase = event.object.value(QStringLiteral("phase")).toString();
            const bool moved = event.object.value(QStringLiteral("moved")).toBool();
            const std::optional<term::Terminal_selection_range> range =
                selection_range_from_event(event);
            if (phase == QStringLiteral("start") ||
                phase == QStringLiteral("clear") ||
                phase == QStringLiteral("cancel"))
            {
                session.clear_selection();
            }
            if ((phase == QStringLiteral("update") || phase == QStringLiteral("finish")) &&
                moved && range.has_value())
            {
                session.set_selection_range(*range);
            }
            else
            if (phase == QStringLiteral("finish") && !moved) {
                session.clear_selection();
            }
        }
    }

    replay.host_writes   = backend_ptr->writes;
    replay.snapshot      = session.latest_render_snapshot();
    replay.viewport      = session.viewport_state();
    replay.selected_text = session.selected_text();
    return replay;
}
bool test_writer_reader_schema_roundtrip()
{
    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary transcript directory is valid")) {
        return false;
    }

    const QString path = temp_dir.filePath(QStringLiteral("roundtrip.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, true, &error);
    bool ok = true;
    ok &= check(recorder != nullptr, "transcript recorder opens");
    if (recorder == nullptr) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    term::Terminal_launch_config launch_config = valid_launch_config();
    term::Terminal_session_config session_config;
    session_config.scrollback_limit = 32;
    session_config.recover_scrollback_from_primary_repaints = true;
    ok &= check(recorder->record_session_start(1U, launch_config, session_config), "session.start writes");
    ok &= check(recorder->record_backend_output(2U, QByteArrayLiteral("out")), "backend.output writes");
    ok &= check(recorder->record_host_write(3U, QStringLiteral("user"), QByteArrayLiteral("in")),
        "host.write writes");

    term::Terminal_resize_transaction resize;
    resize.id                       = 7U;
    resize.target_grid_size         = {30, 100};
    resize.snapshot_grid_size       = {30, 100};
    resize.source_geometry          = QSizeF(900.0, 600.0);
    resize.backend_result           = term::Terminal_backend_resize_result::APPLIED;
    resize.model_result             = term::Terminal_model_resize_result::APPLIED;
    resize.backend_geometry_in_sync = true;
    ok &= check(recorder->record_session_resize_request(4U, resize),
        "session.resize_request writes");
    ok &= check(recorder->record_session_resize(4U, resize), "session.resize writes");

    ok &= check(
        recorder->record_session_backend_error(
            5U,
            {term::Terminal_backend_error_code::READ_FAILED, QStringLiteral("read failed")}),
        "session.backend_error writes");
    ok &= check(
        recorder->record_session_process_exit(
            6U,
            {term::Terminal_exit_reason::EXITED, 0}),
        "session.process_exit writes");
    ok &= check(recorder->record_text_area_resize_request(7U, {40, 120}),
        "session.text_area_resize_request writes");

    term::Terminal_viewport_scroll_result scroll_result;
    scroll_result.action             = term::Terminal_viewport_scroll_action::VIEWPORT_MOVED;
    scroll_result.applied_line_delta = 3;
    term::Terminal_viewport_state viewport_before;
    viewport_before.scrollback_rows = 10;
    viewport_before.visible_rows    = 4;
    viewport_before.alternate_screen_scroll_policy =
        term::Terminal_alternate_screen_scroll_policy::WHEEL_TO_TERMINAL_INPUT;
    term::Terminal_viewport_state viewport_after = viewport_before;
    viewport_after.offset_from_tail = 3;
    viewport_after.follow_tail      = false;
    ok &= check(
        recorder->record_surface_scroll_intent({
            QStringLiteral("test"),
            3,
            std::nullopt,
            viewport_before,
        }),
        "surface.scroll_intent writes");
    ok &= check(
        recorder->record_surface_scroll({
            QStringLiteral("test"),
            3,
            std::nullopt,
            scroll_result,
            viewport_before,
            viewport_after,
        }),
        "surface.scroll writes");

    QJsonObject visible_scroll_fields;
    term::insert_wheel_trace_scroll_publication_fields(
        visible_scroll_fields,
        true,
        false);
    ok &= check(
        visible_scroll_fields.value(QStringLiteral("visible_scroll_applied")).toBool(),
        "wheel trace publication fields mark visible local scroll");

    QJsonObject wheel_trace;
    wheel_trace.insert(QStringLiteral("source"), QStringLiteral("surface.text_area"));
    wheel_trace.insert(QStringLiteral("route"), QStringLiteral("local_scroll"));
    wheel_trace.insert(QStringLiteral("outcome"), QStringLiteral("local_scroll_publication_deferred"));
    wheel_trace.insert(QStringLiteral("accepted"), true);
    wheel_trace.insert(QStringLiteral("angle_delta_x"), 0);
    wheel_trace.insert(QStringLiteral("angle_delta_y"), 120);
    wheel_trace.insert(QStringLiteral("pixel_delta_x"), 0);
    wheel_trace.insert(QStringLiteral("pixel_delta_y"), 0);
    wheel_trace.insert(QStringLiteral("modifiers"), 0);
    wheel_trace.insert(QStringLiteral("wheel_steps"), 1);
    wheel_trace.insert(QStringLiteral("effective_line_delta"), 3);
    wheel_trace.insert(QStringLiteral("angle_remainder"), 0.0);
    wheel_trace.insert(QStringLiteral("pixel_remainder"), 0.0);
    wheel_trace.insert(QStringLiteral("session_present"), true);
    wheel_trace.insert(QStringLiteral("render_publication_blocked"), true);
    wheel_trace.insert(QStringLiteral("published_synchronized_output"), false);
    wheel_trace.insert(QStringLiteral("alternate_screen"), false);
    wheel_trace.insert(QStringLiteral("local_scroll_attempted"), true);
    wheel_trace.insert(QStringLiteral("local_scroll_intent_recorded"), true);
    term::insert_wheel_trace_scroll_publication_fields(
        wheel_trace,
        true,
        true);
    wheel_trace.insert(QStringLiteral("live_sgr_mouse_reporting"), false);
    wheel_trace.insert(QStringLiteral("published_sgr_mouse_reporting"), false);
    wheel_trace.insert(QStringLiteral("published_mouse_tracking"), false);
    wheel_trace.insert(QStringLiteral("backend_drain_calls"), 1);
    wheel_trace.insert(QStringLiteral("backend_drain_elapsed_ns"), 0);
    wheel_trace.insert(QStringLiteral("scroll_action"), QStringLiteral("viewport_moved"));
    wheel_trace.insert(QStringLiteral("applied_line_delta"), 3);
    ok &= check(recorder->record_surface_wheel_trace(std::move(wheel_trace)),
        "surface.wheel_trace writes");

    term::Terminal_selection_range range;
    range.start = {1, 2};
    range.end   = {2, 4};
    ok &= check(
        recorder->record_surface_selection_drag({
            QStringLiteral("finish"),
            range.start,
            range.end,
            range,
            true,
        }),
        "surface.selection_drag writes");
    ok &= check(
        recorder->record_surface_selection_drag({
            QStringLiteral("clear"),
            std::nullopt,
            std::nullopt,
            std::nullopt,
            false,
        }),
        "surface.selection_drag clear writes");

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({4, 12}, viewport_after, 8U);
    ok &= check(recorder->record_snapshot(8U, QStringLiteral("test snapshot"), snapshot),
        "compact snapshot writes when enabled");

    recorder.reset();

    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(path, &error);
    ok &= check(events.has_value(), "transcript reader parses written NDJSON");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    ok &= check(events->front().kind == QStringLiteral("header"), "first event is header");
    for (std::size_t i = 0U; i < events->size(); ++i) {
        ok &= check((*events)[i].event_index == i, "event_index is contiguous in writer output");
    }

    const std::optional<term::Terminal_transcript_event> backend_output =
        first_event(*events, QStringLiteral("backend.output"));
    ok &= check(backend_output.has_value(), "backend.output event is present");
    if (backend_output.has_value()) {
        ok &= check(event_bytes(*backend_output) == QByteArrayLiteral("out"),
            "backend.output bytes roundtrip through base64");
    }

    const std::optional<term::Terminal_transcript_event> scroll =
        first_event(*events, QStringLiteral("surface.scroll"));
    ok &= check(scroll.has_value(), "surface.scroll event is present");
    if (scroll.has_value()) {
        const QJsonObject after_viewport =
            scroll->object.value(QStringLiteral("viewport_after")).toObject();
        ok &= check(scroll->object.contains(QStringLiteral("viewport_before")),
            "surface.scroll records viewport_before");
        ok &= check(!scroll->object.contains(QStringLiteral("viewport")),
            "surface.scroll omits duplicate viewport");
        ok &= check(
            after_viewport.value(QStringLiteral("alternate_screen_scroll_policy")).toString() ==
                QStringLiteral("wheel_to_terminal_input"),
            "viewport diagnostics include alternate_screen_scroll_policy");
    }

    ok &= check(first_event(*events, QStringLiteral("session.resize_request")).has_value(),
        "session.resize_request event is present");
    ok &= check(first_event(*events, QStringLiteral("session.text_area_resize_request")).has_value(),
        "session.text_area_resize_request event is present");
    ok &= check(first_event(*events, QStringLiteral("surface.scroll_intent")).has_value(),
        "surface.scroll_intent event is present");
    const std::optional<term::Terminal_transcript_event> wheel_trace_event =
        first_event(*events, QStringLiteral("surface.wheel_trace"));
    ok &= check(wheel_trace_event.has_value(), "surface.wheel_trace event is present");
    if (wheel_trace_event.has_value()) {
        ok &= check(
            wheel_trace_event->object.value(QStringLiteral("outcome")).toString() ==
                QStringLiteral("local_scroll_publication_deferred"),
            "surface.wheel_trace records deferred publication outcome");
        ok &= check(
            wheel_trace_event->object.contains(QStringLiteral("visible_scroll_applied")),
            "surface.wheel_trace includes visible scroll application field");
        ok &= check(
            !wheel_trace_event->object.value(QStringLiteral("visible_scroll_applied")).toBool(),
            "surface.wheel_trace records visible scroll application state");
        ok &= check(
            wheel_trace_event->object.value(QStringLiteral("backend_drain_calls")).toInt() == 1,
            "surface.wheel_trace records backend drain call count");
    }
    ok &= check(first_event(*events, QStringLiteral("snapshot")).has_value(),
        "snapshot compact event is present");
    const std::optional<term::Terminal_transcript_event> snapshot_event =
        first_event(*events, QStringLiteral("snapshot"));
    if (snapshot_event.has_value()) {
        ok &= check(snapshot_event->object.contains(QStringLiteral("visible_rows")),
            "snapshot diagnostics include visible rows");
        ok &= check(snapshot_event->object.contains(QStringLiteral("row_provenance")),
            "snapshot diagnostics include row provenance");
        ok &= check(snapshot_event->object.contains(QStringLiteral("dirty_row_ranges")),
            "snapshot diagnostics include dirty row ranges");
        ok &= check(snapshot_event->object.contains(QStringLiteral("selection_spans")),
            "snapshot diagnostics include selection spans");
    }
    return ok;
}

bool test_snapshot_diagnostics_flag_controls_snapshot_event()
{
    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary snapshot diagnostics directory is valid")) {
        return false;
    }

    const QString path = temp_dir.filePath(QStringLiteral("snapshot-off.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, false, &error);
    bool ok = true;
    ok &= check(recorder != nullptr, "snapshot-off transcript recorder opens");
    if (recorder == nullptr) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    term::Terminal_viewport_state viewport;
    term::Terminal_render_snapshot snapshot = term::make_empty_render_snapshot({3, 12}, viewport, 1U);
    ok &= check(recorder->record_snapshot(1U, QStringLiteral("not written"), snapshot),
        "record_snapshot succeeds when diagnostics are disabled");
    recorder.reset();

    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(path, &error);
    ok &= check(events.has_value(), "snapshot-off transcript parses");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    ok &= check(!first_event(*events, QStringLiteral("snapshot")).has_value(),
        "snapshot diagnostics disabled omits snapshot events");
    return ok;
}

bool test_snapshot_timing_diagnostics_include_snapshot_metadata()
{
    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary snapshot timing directory is valid")) {
        return false;
    }

    const QString path = temp_dir.filePath(QStringLiteral("snapshot-timing.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, true, true, &error);
    bool ok = true;
    ok &= check(recorder != nullptr, "snapshot timing transcript recorder opens");
    if (recorder == nullptr) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    const QString snapshot_reason = QStringLiteral("timed snapshot diagnostics");
    const term::terminal_grid_size_t grid_size{512, 512};
    term::Terminal_session_config session_config;
    term::Terminal_viewport_state viewport;
    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot(grid_size, viewport, 17U);
    ok &= check(recorder->record_session_start(1U, valid_launch_config(), session_config),
        "snapshot timing session.start writes");
    ok &= check(recorder->record_snapshot(1U, snapshot_reason, snapshot),
        "snapshot timing snapshot writes");
    recorder.reset();

    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(path, &error);
    ok &= check(events.has_value(), "snapshot timing transcript parses");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    std::optional<term::Terminal_transcript_event> snapshot_timing;
    for (const term::Terminal_transcript_event& event : *events) {
        if (event.kind == QStringLiteral("transcript.timing") &&
            event.object.value(QStringLiteral("record_kind")).toString() ==
                QStringLiteral("snapshot"))
        {
            snapshot_timing = event;
            break;
        }
    }

    ok &= check(snapshot_timing.has_value(),
        "snapshot timing diagnostics include snapshot record");
    if (snapshot_timing.has_value()) {
        const QJsonObject object = snapshot_timing->object;
        ok &= check(
            object.value(QStringLiteral("operation")).toString() ==
                QStringLiteral("snapshot_diagnostic_emit"),
            "snapshot timing operation labels snapshot diagnostic emission");
        ok &= check(
            object.value(QStringLiteral("snapshot_reason")).toString() == snapshot_reason,
            "snapshot timing records snapshot reason");
        ok &= check(
            static_cast<qint64>(
                object.value(QStringLiteral("snapshot_sequence")).toDouble(-1.0)) ==
                static_cast<qint64>(snapshot.metadata.sequence),
            "snapshot timing records snapshot sequence");
        ok &= check(
            static_cast<qint64>(
                object.value(QStringLiteral("viewport_cell_count")).toDouble(-1.0)) ==
                static_cast<qint64>(grid_size.rows) * static_cast<qint64>(grid_size.columns),
            "snapshot timing records viewport cell count");
        ok &= check(
            static_cast<qint64>(
                object.value(QStringLiteral("snapshot_cell_count")).toDouble(-1.0)) ==
                static_cast<qint64>(snapshot.cells.size()),
            "snapshot timing records snapshot cell count");
    }
    return ok;
}

bool test_reader_rejects_malformed_transcripts()
{
    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary malformed transcript directory is valid")) {
        return false;
    }

    bool ok = true;

    QJsonObject schema_mismatch = valid_header_object();
    schema_mismatch.insert(QStringLiteral("schema_version"), 2);
    ok &= expect_reader_failure(
        temp_dir,
        QStringLiteral("schema-mismatch.ndjson"),
        {json_line(schema_mismatch)},
        QStringLiteral("schema_version"));

    ok &= expect_reader_failure(
        temp_dir,
        QStringLiteral("non-contiguous.ndjson"),
        {json_line(valid_header_object()), json_line(valid_session_start_object(2U))},
        QStringLiteral("not contiguous"));

    QJsonObject bad_base64;
    bad_base64.insert(QStringLiteral("kind"), QStringLiteral("backend.output"));
    insert_u64(bad_base64, QStringLiteral("event_index"), 1U);
    insert_u64(bad_base64, QStringLiteral("session_sequence"), 1U);
    bad_base64.insert(QStringLiteral("encoding"), QStringLiteral("base64"));
    bad_base64.insert(QStringLiteral("bytes_base64"), QStringLiteral("%%%%"));
    bad_base64.insert(QStringLiteral("byte_count"), 3);
    ok &= expect_reader_failure(
        temp_dir,
        QStringLiteral("bad-base64.ndjson"),
        {json_line(valid_header_object()), json_line(bad_base64)},
        QStringLiteral("base64"));

    QJsonObject bad_byte_count = bad_base64;
    bad_byte_count.insert(QStringLiteral("bytes_base64"), QStringLiteral("YQ=="));
    bad_byte_count.insert(QStringLiteral("byte_count"), 2);
    ok &= expect_reader_failure(
        temp_dir,
        QStringLiteral("bad-byte-count.ndjson"),
        {json_line(valid_header_object()), json_line(bad_byte_count)},
        QStringLiteral("byte_count"));

    QJsonObject empty_argv = valid_session_start_object(1U);
    empty_argv.insert(QStringLiteral("argv"), QJsonArray{});
    ok &= expect_reader_failure(
        temp_dir,
        QStringLiteral("empty-argv.ndjson"),
        {json_line(valid_header_object()), json_line(empty_argv)},
        QStringLiteral("argv must not be empty"));

    QJsonObject negative_grid = valid_session_start_object(1U);
    QJsonObject grid_size = negative_grid.value(QStringLiteral("initial_grid_size")).toObject();
    grid_size.insert(QStringLiteral("rows"), -1);
    negative_grid.insert(QStringLiteral("initial_grid_size"), grid_size);
    ok &= expect_reader_failure(
        temp_dir,
        QStringLiteral("negative-grid.ndjson"),
        {json_line(valid_header_object()), json_line(negative_grid)},
        QStringLiteral("negative integer field rows"));

    QJsonObject invalid_phase;
    invalid_phase.insert(QStringLiteral("kind"), QStringLiteral("surface.selection_drag"));
    insert_u64(invalid_phase, QStringLiteral("event_index"), 1U);
    invalid_phase.insert(QStringLiteral("phase"), QStringLiteral("bogus"));
    invalid_phase.insert(QStringLiteral("moved"), false);
    ok &= expect_reader_failure(
        temp_dir,
        QStringLiteral("invalid-phase.ndjson"),
        {json_line(valid_header_object()), json_line(invalid_phase)},
        QStringLiteral("unsupported value bogus"));

    QJsonObject negative_position = invalid_phase;
    negative_position.insert(QStringLiteral("phase"), QStringLiteral("start"));
    negative_position.insert(QStringLiteral("anchor"), QJsonObject{
        {QStringLiteral("row"), -1},
        {QStringLiteral("column"), 0},
    });
    ok &= expect_reader_failure(
        temp_dir,
        QStringLiteral("negative-position.ndjson"),
        {json_line(valid_header_object()), json_line(negative_position)},
        QStringLiteral("negative integer field row"));

    QJsonObject invalid_range = invalid_phase;
    invalid_range.insert(QStringLiteral("phase"), QStringLiteral("finish"));
    invalid_range.insert(QStringLiteral("moved"), true);
    invalid_range.insert(QStringLiteral("range"), QJsonObject{
        {QStringLiteral("start"), QJsonObject{
            {QStringLiteral("row"), 0},
            {QStringLiteral("column"), 0},
        }},
        {QStringLiteral("end"), QJsonObject{
            {QStringLiteral("row"), 0},
            {QStringLiteral("column"), 1},
        }},
        {QStringLiteral("mode"), QStringLiteral("none")},
    });
    ok &= expect_reader_failure(
        temp_dir,
        QStringLiteral("invalid-selection-range.ndjson"),
        {json_line(valid_header_object()), json_line(invalid_range)},
        QStringLiteral("selection range cannot use mode none"));

    return ok;
}

bool test_reader_rejects_compact_snapshots_missing_required_diagnostics()
{
    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary compact-snapshot malformed directory is valid")) {
        return false;
    }

    const QStringList required_fields = {
        QStringLiteral("dirty_row_ranges"),
        QStringLiteral("visible_rows"),
        QStringLiteral("row_provenance"),
        QStringLiteral("selection_spans"),
        QStringLiteral("selected_text"),
    };

    bool ok = true;
    for (const QString& field : required_fields) {
        QJsonObject snapshot = valid_compact_snapshot_object(1U);
        snapshot.remove(field);
        ok &= expect_reader_failure(
            temp_dir,
            QStringLiteral("compact-snapshot-missing-%1.ndjson").arg(field),
            {json_line(valid_header_object()), json_line(snapshot)},
            field);
    }
    return ok;
}

bool test_transcript_orders_host_and_resize_requests_before_synchronous_output()
{
    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary ordering transcript directory is valid")) {
        return false;
    }

    const QString path = temp_dir.filePath(QStringLiteral("ordering.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, false, &error);
    bool ok = true;
    ok &= check(recorder != nullptr, "ordering transcript recorder opens");
    if (recorder == nullptr) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    auto backend = std::make_unique<Scripted_backend>();
    Scripted_backend* backend_ptr = backend.get();

    term::Terminal_session_config config;
    config.transcript_recorder = recorder;
    term::Terminal_session session(std::move(backend), config);

    term::Terminal_launch_config launch_config = valid_launch_config();
    launch_config.initial_grid_size = term::terminal_grid_size_t{3, 12};
    ok &= check(session.start(launch_config).code == term::Terminal_session_result_code::ACCEPTED,
        "ordering session starts");

    backend_ptr->output_on_write = QByteArrayLiteral("write-sync");
    ok &= check(session.write_user_bytes(QByteArrayLiteral("manual-input")).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "ordering host write succeeds");

    backend_ptr->output_on_write.clear();
    backend_ptr->output_on_resize = QByteArrayLiteral("resize-sync");
    ok &= check(session.resize(QSizeF(120.0, 36.0), {4, 12}).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "ordering resize succeeds");

    recorder.reset();

    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(path, &error);
    ok &= check(events.has_value(), "ordering transcript parses");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    const std::optional<std::size_t> host_write_position =
        first_event_position(*events, QStringLiteral("host.write"));
    const std::optional<std::size_t> write_output_position =
        first_event_position_with_bytes(*events, QStringLiteral("backend.output"), QByteArrayLiteral("write-sync"));
    ok &= check(host_write_position.has_value() && write_output_position.has_value() &&
        *host_write_position < *write_output_position,
        "host.write is recorded before synchronous backend output");

    const std::optional<std::size_t> resize_request_position =
        first_event_position(*events, QStringLiteral("session.resize_request"));
    const std::optional<std::size_t> resize_output_position =
        first_event_position_with_bytes(*events, QStringLiteral("backend.output"), QByteArrayLiteral("resize-sync"));
    ok &= check(resize_request_position.has_value() && resize_output_position.has_value() &&
        *resize_request_position < *resize_output_position,
        "session.resize_request is recorded before synchronous backend output");
    return ok;
}

bool test_text_area_resize_requests_record_from_session()
{
    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary text-area resize transcript directory is valid")) {
        return false;
    }

    const QString path = temp_dir.filePath(QStringLiteral("text-area-resize.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, false, &error);
    bool ok = true;
    ok &= check(recorder != nullptr, "text-area resize transcript recorder opens");
    if (recorder == nullptr) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    auto backend = std::make_unique<Scripted_backend>();
    Scripted_backend* backend_ptr = backend.get();

    term::Terminal_session_config config;
    config.transcript_recorder = recorder;
    term::Terminal_session session(std::move(backend), config);

    term::Terminal_launch_config launch_config = valid_launch_config();
    launch_config.initial_grid_size = term::terminal_grid_size_t{3, 12};
    ok &= check(session.start(launch_config).code == term::Terminal_session_result_code::ACCEPTED,
        "text-area resize session starts");

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[8;4;12t\x1b[8;5;12t"));
    recorder.reset();

    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(path, &error);
    ok &= check(events.has_value(), "text-area resize transcript parses");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    ok &= check(
        event_count(*events, QStringLiteral("session.text_area_resize_request")) == 2,
        "text-area resize requests are recorded before notification coalescing can drop one");
    return ok;
}

bool test_terminal_reply_replay_does_not_duplicate_host_write()
{
    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary terminal-reply transcript directory is valid")) {
        return false;
    }

    const QString path = temp_dir.filePath(QStringLiteral("terminal-reply.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, false, &error);
    bool ok = true;
    ok &= check(recorder != nullptr, "terminal-reply transcript recorder opens");
    if (recorder == nullptr) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    auto backend = std::make_unique<Scripted_backend>();
    Scripted_backend* backend_ptr = backend.get();

    term::Terminal_session_config config;
    config.transcript_recorder = recorder;
    term::Terminal_session session(std::move(backend), config);

    term::Terminal_launch_config launch_config = valid_launch_config();
    launch_config.initial_grid_size = term::terminal_grid_size_t{3, 12};
    ok &= check(session.start(launch_config).code == term::Terminal_session_result_code::ACCEPTED,
        "terminal-reply session starts");
    backend_ptr->emit_output(QByteArrayLiteral("ab\x1b[6n"));
    recorder.reset();

    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(path, &error);
    ok &= check(events.has_value(), "terminal-reply transcript parses");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    const std::optional<term::Terminal_transcript_event> host_write =
        first_event(*events, QStringLiteral("host.write"));
    ok &= check(host_write.has_value() &&
        host_write->object.value(QStringLiteral("source")).toString() ==
            QStringLiteral("terminal_reply"),
        "terminal-reply host.write event is captured");

    const Replay_harness replay = replay_events(*events);
    ok &= check(replay.error.isEmpty(), "terminal-reply replay succeeds");
    ok &= check(replay.skipped_terminal_reply_host_writes == 1,
        "terminal-reply host.write event is skipped during replay injection");
    ok &= check(replay.host_writes.size() == 1U,
        "terminal-reply replay has one backend write generated by backend output");
    if (host_write.has_value() && replay.host_writes.size() == 1U) {
        ok &= check(replay.host_writes.front() == event_bytes(*host_write),
            "terminal-reply replay generated the recorded reply bytes once");
    }
    return ok;
}

bool test_scroll_intent_orders_before_snapshot_diagnostics()
{
    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary scroll ordering transcript directory is valid")) {
        return false;
    }

    const QString path = temp_dir.filePath(QStringLiteral("scroll-order.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, true, true, &error);
    bool ok = true;
    ok &= check(recorder != nullptr, "scroll ordering transcript recorder opens");
    if (recorder == nullptr) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    auto backend = std::make_unique<Scripted_backend>();
    Scripted_backend* backend_ptr = backend.get();

    term::Terminal_session_config config;
    config.transcript_recorder = recorder;
    term::Terminal_session session(std::move(backend), config);

    term::Terminal_launch_config launch_config = valid_launch_config();
    launch_config.initial_grid_size = term::terminal_grid_size_t{3, 12};
    ok &= check(session.start(launch_config).code == term::Terminal_session_result_code::ACCEPTED,
        "scroll ordering session starts");
    backend_ptr->emit_output(QByteArrayLiteral("one\r\ntwo\r\nthree\r\nfour\r\nfive"));

    const term::Terminal_viewport_state viewport_before = session.viewport_state();
    ok &= check(
        recorder->record_surface_scroll_intent({
            QStringLiteral("test.offset"),
            1,
            1,
            viewport_before,
        }),
        "scroll ordering intent writes before mutation");
    const term::Terminal_viewport_scroll_result scroll_result =
        session.scroll_published_viewport_to_offset_from_tail(1);
    ok &= check(scroll_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "scroll ordering session scrolls");
    ok &= check(
        recorder->record_surface_scroll({
            QStringLiteral("test.offset"),
            scroll_result.applied_line_delta,
            1,
            scroll_result,
            viewport_before,
            session.viewport_state(),
        }),
        "scroll ordering result writes after mutation");
    recorder.reset();

    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(path, &error);
    ok &= check(events.has_value(), "scroll ordering transcript parses");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    const std::optional<std::size_t> intent_position =
        first_event_position(*events, QStringLiteral("surface.scroll_intent"));
    const std::optional<std::size_t> snapshot_after_intent =
        intent_position.has_value()
            ? first_event_position_after(*events, QStringLiteral("snapshot"), *intent_position + 1U)
            : std::nullopt;
    const std::optional<std::size_t> scroll_position =
        first_event_position(*events, QStringLiteral("surface.scroll"));
    ok &= check(
        intent_position.has_value() &&
            snapshot_after_intent.has_value() &&
            scroll_position.has_value() &&
            *intent_position < *snapshot_after_intent &&
            *snapshot_after_intent < *scroll_position,
        "surface.scroll_intent is recorded before scroll snapshot and result");
    return ok;
}

bool test_replay_noop_scroll_intent_does_not_consume_next_scroll()
{
    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary scroll replay transcript directory is valid")) {
        return false;
    }

    const QString path = temp_dir.filePath(QStringLiteral("scroll-replay.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, false, &error);
    bool ok = true;
    ok &= check(recorder != nullptr, "scroll replay transcript recorder opens");
    if (recorder == nullptr) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    auto backend = std::make_unique<Scripted_backend>();
    Scripted_backend* backend_ptr = backend.get();

    term::Terminal_session_config config;
    config.transcript_recorder = recorder;
    term::Terminal_session session(std::move(backend), config);

    term::Terminal_launch_config launch_config = valid_launch_config();
    launch_config.initial_grid_size = term::terminal_grid_size_t{3, 12};
    ok &= check(session.start(launch_config).code == term::Terminal_session_result_code::ACCEPTED,
        "scroll replay session starts");
    backend_ptr->emit_output(QByteArrayLiteral("one\r\ntwo\r\nthree\r\nfour\r\nfive"));

    const term::Terminal_viewport_state viewport_before_noop = session.viewport_state();
    ok &= check(
        recorder->record_surface_scroll_intent({
            QStringLiteral("test.offset"),
            0,
            0,
            viewport_before_noop,
        }),
        "scroll replay no-op intent writes");
    const term::Terminal_viewport_scroll_result noop_scroll_result =
        session.scroll_published_viewport_to_offset_from_tail(0);
    ok &= check(noop_scroll_result.action == term::Terminal_viewport_scroll_action::AT_BOUNDARY,
        "scroll replay no-op intent leaves viewport at boundary");

    const term::Terminal_viewport_state viewport_before_scroll = session.viewport_state();
    const term::Terminal_viewport_scroll_result scroll_result =
        session.scroll_published_viewport_to_offset_from_tail(2);
    ok &= check(scroll_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "scroll replay fixture has a real scroll after no-op intent");
    ok &= check(
        recorder->record_surface_scroll({
            QStringLiteral("test.offset"),
            scroll_result.applied_line_delta,
            2,
            scroll_result,
            viewport_before_scroll,
            session.viewport_state(),
        }),
        "scroll replay real scroll writes after no-op intent");
    recorder.reset();

    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(path, &error);
    ok &= check(events.has_value(), "scroll replay transcript parses");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    const Replay_harness replay = replay_events(*events);
    ok &= check(replay.error.isEmpty(), "scroll replay succeeds");
    ok &= check(replay.viewport.offset_from_tail == 2,
        "scroll replay applies real scroll after no-op intent");
    return ok;
}

bool test_replay_drives_terminal_state()
{
    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary replay directory is valid")) {
        return false;
    }

    const QString path = temp_dir.filePath(QStringLiteral("session.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, false, &error);
    bool ok = true;
    ok &= check(recorder != nullptr, "replay transcript recorder opens");
    if (recorder == nullptr) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    auto backend = std::make_unique<Scripted_backend>();
    Scripted_backend* backend_ptr = backend.get();

    term::Terminal_session_config config;
    config.transcript_recorder = recorder;
    term::Terminal_session session(std::move(backend), config);

    term::Terminal_launch_config launch_config = valid_launch_config();
    launch_config.initial_grid_size = term::terminal_grid_size_t{3, 10};
    ok &= check(session.start(launch_config).code == term::Terminal_session_result_code::ACCEPTED,
        "captured replay session starts");
    ok &= check(session.resize(QSizeF(120.0, 36.0), {3, 12}).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "captured replay resize succeeds");

    backend_ptr->emit_output(QByteArrayLiteral("alpha\r\nbravo\r\ncharlie\r\ndelta\r\necho"));

    const term::Terminal_viewport_state viewport_before_scroll = session.viewport_state();
    ok &= check(
        recorder->record_surface_scroll_intent({
            QStringLiteral("test.offset"),
            2,
            2,
            viewport_before_scroll,
        }),
        "captured replay surface scroll intent writes");
    const term::Terminal_viewport_scroll_result scroll_result =
        session.scroll_published_viewport_to_offset_from_tail(2);
    ok &= check(scroll_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "captured replay session creates scrollback to replay");
    ok &= check(
        recorder->record_surface_scroll({
            QStringLiteral("test.offset"),
            scroll_result.applied_line_delta,
            2,
            scroll_result,
            viewport_before_scroll,
            session.viewport_state(),
        }),
        "captured replay surface scroll writes");

    term::Terminal_selection_range selection;
    selection.start = {0, 0};
    selection.end   = {0, 5};
    ok &= check(
        recorder->record_surface_selection_drag({
            QStringLiteral("finish"),
            selection.start,
            selection.end,
            selection,
            true,
        }),
        "captured replay selection drag writes");
    session.set_selection_range(selection);

    backend_ptr->emit_error({
        term::Terminal_backend_error_code::READ_FAILED,
        QStringLiteral("diagnostic error"),
    });
    backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 0});

    recorder.reset();

    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(path, &error);
    ok &= check(events.has_value(), "captured replay transcript parses");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    const Replay_harness replay = replay_events(*events);
    ok &= check(replay.error.isEmpty(), "headless replay applies transcript events without error");
    ok &= check(replay.snapshot.has_value(), "headless replay publishes a final render snapshot");
    if (replay.snapshot.has_value()) {
        const QStringList rows = snapshot_visible_rows(*replay.snapshot);
        ok &= check(replay.snapshot->grid_size.rows == 3 && replay.snapshot->grid_size.columns == 12,
            "headless replay applies session.resize grid");
        ok &= check(rows.size() == 3 &&
            rows[0] == QStringLiteral("alpha") &&
            rows[1] == QStringLiteral("bravo") &&
            rows[2] == QStringLiteral("charlie"),
            "headless replayed backend.output bytes produce expected visible text");
        ok &= check(!replay.snapshot->selection_spans.empty(),
            "headless replay applies selection range to final snapshot spans");
    }
    ok &= check(replay.viewport.scrollback_rows == 2 && replay.viewport.offset_from_tail == 2,
        "headless replay applies surface.scroll viewport state");
    ok &= check(replay.selected_text.code == term::Terminal_selection_result_code::OK &&
        replay.selected_text.text == QStringLiteral("alpha"),
        "headless replay selection resolves expected selected text");
    ok &= check(first_event(*events, QStringLiteral("session.backend_error")).has_value(),
        "captured backend error parses");
    ok &= check(first_event(*events, QStringLiteral("session.process_exit")).has_value(),
        "captured process exit parses");
    return ok;
}

bool test_replay_honors_primary_repaint_recovery_session_config()
{
    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary repaint recovery transcript directory is valid")) {
        return false;
    }

    const QString path = temp_dir.filePath(QStringLiteral("repaint-recovery.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, true, &error);
    bool ok = true;
    ok &= check(recorder != nullptr, "repaint recovery transcript recorder opens");
    if (recorder == nullptr) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    auto backend = std::make_unique<Scripted_backend>();
    Scripted_backend* backend_ptr = backend.get();

    term::Terminal_session_config config;
    config.transcript_recorder = recorder;
    config.scrollback_limit = 1;
    config.recover_scrollback_from_primary_repaints = true;
    term::Terminal_session session(std::move(backend), config);

    term::Terminal_launch_config launch_config = valid_launch_config();
    launch_config.initial_grid_size = term::terminal_grid_size_t{3, 4};
    ok &= check(session.start(launch_config).code == term::Terminal_session_result_code::ACCEPTED,
        "repaint recovery captured session starts");
    backend_ptr->emit_output(QByteArrayLiteral("aaa\r\nbbb\r\nccc\x1b[?25l"));
    backend_ptr->emit_output(QByteArrayLiteral("\x1b[H\x1b[2Jbbb\r\nccc\r\nddd\x1b[?25h"));
    recorder.reset();

    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(path, &error);
    ok &= check(events.has_value(), "repaint recovery transcript parses");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    const std::optional<term::Terminal_transcript_event> session_start =
        first_event(*events, QStringLiteral("session.start"));
    const QJsonObject recorded_config =
        session_start.has_value()
            ? session_start->object.value(QStringLiteral("session_config")).toObject()
            : QJsonObject{};
    ok &= check(
        recorded_config.value(QStringLiteral("recover_scrollback_from_primary_repaints")).toBool(),
        "session.start records primary repaint recovery config");
    ok &= check(
        recorded_config.value(QStringLiteral("scrollback_limit")).toInt() == 1 &&
            recorded_config.value(QStringLiteral("effective_scrollback_limit")).toInt() == 1,
        "session.start records effective scrollback limit");

    const Replay_harness replay = replay_events(*events);
    ok &= check(replay.error.isEmpty(), "repaint recovery replay succeeds with recorded config");
    ok &= check(
        replay.viewport.scrollback_rows == 1 &&
            replay.viewport.offset_from_tail == 0,
        "repaint recovery replay keeps recovered scrollback");

    std::vector<term::Terminal_transcript_event> events_without_config = *events;
    for (term::Terminal_transcript_event& event : events_without_config) {
        if (event.kind == QStringLiteral("session.start")) {
            event.object.remove(QStringLiteral("session_config"));
            break;
        }
    }
    const Replay_harness inferred_replay = replay_events(events_without_config);
    ok &= check(inferred_replay.error.isEmpty(), "older repaint recovery replay succeeds");
    ok &= check(
        inferred_replay.viewport.scrollback_rows == 1 &&
            inferred_replay.viewport.offset_from_tail == 0,
        "older repaint recovery replay infers nonzero recovered scrollback");
    return ok;
}

bool test_replay_tool_treats_dirty_only_snapshot_mismatch_as_diagnostic(
    const QString& replay_tool_path)
{
    bool ok = true;
    ok &= check(!replay_tool_path.isEmpty(), "dirty-only replay tool path is passed");
    if (replay_tool_path.isEmpty()) {
        return false;
    }

    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary dirty-only replay directory is valid")) {
        return false;
    }

    const QString path = temp_dir.filePath(QStringLiteral("dirty-only.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, true, &error);
    ok &= check(recorder != nullptr, "dirty-only replay transcript recorder opens");
    if (recorder == nullptr) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    auto backend = std::make_unique<Scripted_backend>();
    Scripted_backend* backend_ptr = backend.get();

    term::Terminal_session_config config;
    config.transcript_recorder = recorder;
    term::Terminal_session session(std::move(backend), config);

    term::Terminal_launch_config launch_config = valid_launch_config();
    launch_config.initial_grid_size = term::terminal_grid_size_t{3, 12};
    ok &= check(session.start(launch_config).code == term::Terminal_session_result_code::ACCEPTED,
        "dirty-only replay captured session starts");
    backend_ptr->emit_output(QByteArrayLiteral("alpha\r\nbravo\r\ncharlie"));
    recorder.reset();

    ok &= check(rewrite_first_snapshot_dirty_ranges(path),
        "dirty-only replay fixture rewrites dirty diagnostics");

    const Replay_tool_process_result replay = run_replay_tool(replay_tool_path, path);
    ok &= check(replay.finished, "dirty-only replay tool finishes");
    ok &= check(replay.exit_status == QProcess::NormalExit && replay.exit_code == 0,
        "dirty-only replay tool exits successfully");
    ok &= check(replay.stdout_text.contains("divergent_snapshot_events=0"),
        "dirty-only replay has no model divergence");
    ok &= check(replay.stdout_text.contains("dirty_mismatch_snapshot_events=1"),
        "dirty-only replay reports dirty mismatch diagnostically");
    if (!ok) {
        print_replay_tool_output(replay);
    }
    return ok;
}

bool test_replay_tool_applies_post_tail_scroll_snapshots(const QString& replay_tool_path)
{
    bool ok = true;
    ok &= check(!replay_tool_path.isEmpty(), "post-tail scroll replay tool path is passed");
    if (replay_tool_path.isEmpty()) {
        return false;
    }

    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary post-tail scroll replay directory is valid")) {
        return false;
    }

    const QString path = temp_dir.filePath(QStringLiteral("post-tail-scroll.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, true, &error);
    ok &= check(recorder != nullptr, "post-tail scroll replay transcript recorder opens");
    if (recorder == nullptr) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    auto backend = std::make_unique<Scripted_backend>();
    Scripted_backend* backend_ptr = backend.get();

    term::Terminal_session_config config;
    config.transcript_recorder = recorder;
    config.scrollback_limit = 1;
    config.recover_scrollback_from_primary_repaints = true;
    term::Terminal_session session(std::move(backend), config);

    term::Terminal_launch_config launch_config = valid_launch_config();
    launch_config.initial_grid_size = term::terminal_grid_size_t{3, 4};
    ok &= check(session.start(launch_config).code == term::Terminal_session_result_code::ACCEPTED,
        "post-tail scroll replay captured session starts");
    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?1004h"));
    backend_ptr->emit_output(QByteArrayLiteral("aaa\r\nbbb\r\nccc\x1b[?25l"));
    backend_ptr->emit_output(QByteArrayLiteral("\x1b[H\x1b[2Jbbb\r\nccc\r\nddd\x1b[?25h"));

    const term::Terminal_viewport_state viewport_before_scroll = session.viewport_state();
    ok &= check(
        recorder->record_surface_scroll_intent({
            QStringLiteral("test.post_tail"),
            1,
            1,
            viewport_before_scroll,
        }),
        "post-tail scroll replay intent writes after tail snapshot");
    const term::Terminal_viewport_scroll_result scroll_result =
        session.scroll_published_viewport_to_offset_from_tail(1);
    ok &= check(scroll_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "post-tail scroll replay fixture scrolls away from tail");
    ok &= check(
        recorder->record_surface_scroll({
            QStringLiteral("test.post_tail"),
            scroll_result.applied_line_delta,
            1,
            scroll_result,
            viewport_before_scroll,
            session.viewport_state(),
        }),
        "post-tail scroll replay result writes after scrolled snapshot");
    const term::Terminal_focus_event_result focus_out_result = session.write_focus_event(false);
    ok &= check(
        focus_out_result.handled &&
            focus_out_result.result.code == term::Terminal_session_result_code::ACCEPTED,
        "post-tail scroll replay focus-out report writes without returning to tail");
    const term::Terminal_focus_event_result focus_in_result = session.write_focus_event(true);
    ok &= check(
        focus_in_result.handled &&
            focus_in_result.result.code == term::Terminal_session_result_code::ACCEPTED,
        "post-tail scroll replay focus-in report writes without returning to tail");
    recorder.reset();

    const Replay_tool_process_result replay = run_replay_tool(replay_tool_path, path);
    ok &= check(replay.finished, "post-tail scroll replay tool finishes");
    ok &= check(replay.exit_status == QProcess::NormalExit && replay.exit_code == 0,
        "post-tail scroll replay tool exits successfully");
    ok &= check(replay.stdout_text.contains("divergent_snapshot_events=0"),
        "post-tail scroll replay has no model divergence");
    ok &= check(replay.stdout_text.contains("viewport.scrollback_rows=1"),
        "post-tail scroll replay keeps recovered scrollback rows");
    ok &= check(replay.stdout_text.contains("viewport.offset_from_tail=1"),
        "post-tail scroll replay publishes final scrolled offset");
    if (!ok) {
        print_replay_tool_output(replay);
    }
    return ok;
}

bool test_replay_tool_compares_only_final_snapshot_in_contiguous_run(
    const QString& replay_tool_path)
{
    bool ok = true;
    ok &= check(!replay_tool_path.isEmpty(), "snapshot-run replay tool path is passed");
    if (replay_tool_path.isEmpty()) {
        return false;
    }

    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary snapshot-run replay directory is valid")) {
        return false;
    }

    const QString path = temp_dir.filePath(QStringLiteral("snapshot-run.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, true, &error);
    ok &= check(recorder != nullptr, "snapshot-run replay transcript recorder opens");
    if (recorder == nullptr) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    auto backend = std::make_unique<Scripted_backend>();
    Scripted_backend* backend_ptr = backend.get();

    term::Terminal_session_config config;
    config.transcript_recorder = recorder;
    term::Terminal_session session(std::move(backend), config);

    term::Terminal_launch_config launch_config = valid_launch_config();
    launch_config.initial_grid_size = term::terminal_grid_size_t{3, 12};
    ok &= check(session.start(launch_config).code == term::Terminal_session_result_code::ACCEPTED,
        "snapshot-run replay captured session starts");
    backend_ptr->emit_output(QByteArrayLiteral("final"));
    ok &= check(session.write_user_bytes(QByteArrayLiteral("x")).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "snapshot-run replay writes following input event");
    recorder.reset();

    ok &= check(insert_replay_transparent_diagnostics_around_first_snapshot(path),
        "snapshot-run replay fixture inserts replay-transparent diagnostics");

    const Replay_tool_process_result replay = run_replay_tool(replay_tool_path, path);
    ok &= check(replay.finished, "snapshot-run replay tool finishes");
    ok &= check(replay.exit_status == QProcess::NormalExit && replay.exit_code == 0,
        "snapshot-run replay tool exits successfully");
    ok &= check(replay.stdout_text.contains("recorded_snapshot_events=2"),
        "snapshot-run replay sees both recorded snapshots");
    ok &= check(replay.stdout_text.contains("intermediate_snapshot_events=1"),
        "snapshot-run replay classifies the earlier snapshot as intermediate");
    ok &= check(replay.stdout_text.contains("matching_snapshot_events=1"),
        "snapshot-run replay still compares the final snapshot");
    ok &= check(replay.stdout_text.contains("divergent_snapshot_events=0"),
        "snapshot-run replay has no model divergence");
    if (!ok) {
        print_replay_tool_output(replay);
    }
    return ok;
}

bool test_replay_tool_prints_diagnostics(const QString& replay_tool_path)
{
    bool ok = true;
    ok &= check(!replay_tool_path.isEmpty(), "replay tool path is passed to transcript tests");
    if (replay_tool_path.isEmpty()) {
        return false;
    }

    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary replay tool directory is valid")) {
        return false;
    }

    const QString path = temp_dir.filePath(QStringLiteral("tool-session.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, true, &error);
    ok &= check(recorder != nullptr, "replay tool transcript recorder opens");
    if (recorder == nullptr) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    auto backend = std::make_unique<Scripted_backend>();
    Scripted_backend* backend_ptr = backend.get();

    term::Terminal_session_config config;
    config.transcript_recorder = recorder;
    term::Terminal_session session(std::move(backend), config);

    term::Terminal_launch_config launch_config = valid_launch_config();
    launch_config.initial_grid_size = term::terminal_grid_size_t{3, 12};
    ok &= check(session.start(launch_config).code == term::Terminal_session_result_code::ACCEPTED,
        "replay tool captured session starts");
    backend_ptr->emit_output(QByteArrayLiteral("alpha\r\nbravo\r\ncharlie"));

    term::Terminal_selection_range selection;
    selection.start = {0, 0};
    selection.end   = {0, 5};
    ok &= check(
        recorder->record_surface_selection_drag({
            QStringLiteral("finish"),
            selection.start,
            selection.end,
            selection,
            true,
        }),
        "replay tool selection drag writes");
    session.set_selection_range(selection);
    recorder.reset();

    const Replay_tool_process_result replay = run_replay_tool(replay_tool_path, path);
    ok &= check(replay.finished, "replay tool finishes");
    ok &= check(replay.exit_status == QProcess::NormalExit && replay.exit_code == 0,
        "replay tool exits successfully");
    ok &= check(replay.stdout_text.contains("grid=3x12"), "replay tool prints grid diagnostics");
    ok &= check(replay.stdout_text.contains("visible_row[0]"), "replay tool prints visible rows");
    ok &= check(replay.stdout_text.contains("selected_text=\"alpha\""),
        "replay tool prints selected text diagnostics");
    ok &= check(
        replay.stdout_text.contains("selection_replay=semantic_range_from_surface_selection_drag"),
        "replay tool labels semantic selection replay");
    if (!ok) {
        print_replay_tool_output(replay);
    }
    return ok;
}

}

int main(int argc, char** argv)
{
    bool ok = true;
    ok &= test_writer_reader_schema_roundtrip();
    ok &= test_snapshot_diagnostics_flag_controls_snapshot_event();
    ok &= test_snapshot_timing_diagnostics_include_snapshot_metadata();
    ok &= test_reader_rejects_malformed_transcripts();
    ok &= test_reader_rejects_compact_snapshots_missing_required_diagnostics();
    ok &= test_transcript_orders_host_and_resize_requests_before_synchronous_output();
    ok &= test_text_area_resize_requests_record_from_session();
    ok &= test_terminal_reply_replay_does_not_duplicate_host_write();
    ok &= test_scroll_intent_orders_before_snapshot_diagnostics();
    ok &= test_replay_noop_scroll_intent_does_not_consume_next_scroll();
    ok &= test_replay_drives_terminal_state();
    ok &= test_replay_honors_primary_repaint_recovery_session_config();
    const QString replay_tool_path =
        argc >= 2 ? QString::fromLocal8Bit(argv[1]) : QString();
    ok &= test_replay_tool_treats_dirty_only_snapshot_mismatch_as_diagnostic(replay_tool_path);
    ok &= test_replay_tool_applies_post_tail_scroll_snapshots(replay_tool_path);
    ok &= test_replay_tool_compares_only_final_snapshot_in_contiguous_run(replay_tool_path);
    ok &= test_replay_tool_prints_diagnostics(replay_tool_path);
    return ok ? 0 : 1;
}
