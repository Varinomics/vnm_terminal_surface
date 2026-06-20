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
#include <cstdint>
#include <initializer_list>
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

QString hex_u64(std::uint64_t value)
{
    return QStringLiteral("%1").arg(
        static_cast<qulonglong>(value),
        16,
        16,
        QLatin1Char('0'));
}

std::uint64_t fnv1a64(QByteArrayView bytes)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (char byte : bytes) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

QString text_hash64(const QString& text)
{
    const QByteArray bytes = text.toUtf8();
    return hex_u64(fnv1a64(QByteArrayView(bytes)));
}

QByteArray visible_row_write_stream(
    std::initializer_list<QByteArray>  rows,
    bool                               cursor_hidden)
{
    QByteArray stream;
    if (cursor_hidden) {
        stream += QByteArrayLiteral("\x1b[?25l");
    }

    int row_number = 1;
    for (const QByteArray& row : rows) {
        stream += QByteArrayLiteral("\x1b[");
        stream += QByteArray::number(row_number);
        stream += QByteArrayLiteral(";1H");
        stream += row;
        stream += QByteArrayLiteral("\x1b[K");
        ++row_number;
    }
    if (cursor_hidden) {
        stream += QByteArrayLiteral("\x1b[?25h");
    }
    return stream;
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
            {QStringLiteral("source"),             QStringLiteral("terminal_storage")},
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

term::Terminal_public_projection_row public_projection_row_from_text(
    std::int64_t    public_row,
    const QString&  text)
{
    term::Terminal_public_projection_row row;
    row.public_row = public_row;
    row.provenance = {
        public_row,
        static_cast<std::uint64_t>(9000 + public_row),
        1U,
    };

    for (int column = 0; column < text.size(); ++column) {
        term::Terminal_render_cell cell;
        cell.position = {0, column};
        cell.text     = text.mid(column, 1);
        row.cells.push_back(std::move(cell));
    }
    return row;
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

std::optional<term::Terminal_transcript_event> last_event(
    const std::vector<term::Terminal_transcript_event>& events,
    const QString&                                      kind)
{
    const auto it = std::find_if(
        events.rbegin(),
        events.rend(),
        [&kind](const term::Terminal_transcript_event& event) {
            return event.kind == kind;
        });
    if (it == events.rend()) {
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

QJsonObject valid_wheel_trace_object(std::uint64_t event_index)
{
    QJsonObject object;
    object.insert(QStringLiteral("kind"), QStringLiteral("surface.wheel_trace"));
    insert_u64(object, QStringLiteral("event_index"), event_index);
    object.insert(QStringLiteral("source"), QStringLiteral("surface.text_area.wheel"));
    object.insert(QStringLiteral("route"), QStringLiteral("local_scroll"));
    object.insert(QStringLiteral("outcome"), QStringLiteral("local_scroll_publication_deferred"));
    object.insert(QStringLiteral("accepted"), true);
    object.insert(QStringLiteral("angle_delta_x"), 0);
    object.insert(QStringLiteral("angle_delta_y"), 120);
    object.insert(QStringLiteral("pixel_delta_x"), 0);
    object.insert(QStringLiteral("pixel_delta_y"), 0);
    object.insert(QStringLiteral("modifiers"), 0);
    object.insert(QStringLiteral("wheel_steps"), 1);
    object.insert(QStringLiteral("effective_line_delta"), 3);
    object.insert(QStringLiteral("angle_remainder"), 0.0);
    object.insert(QStringLiteral("pixel_remainder"), 0.0);
    object.insert(QStringLiteral("session_present"), true);
    object.insert(QStringLiteral("render_publication_blocked"), true);
    object.insert(QStringLiteral("published_synchronized_output"), false);
    object.insert(QStringLiteral("alternate_screen"), false);
    object.insert(QStringLiteral("local_scroll_attempted"), true);
    object.insert(QStringLiteral("local_scroll_intent_recorded"), true);
    object.insert(QStringLiteral("local_scroll_applied"), true);
    object.insert(QStringLiteral("visible_scroll_applied"), false);
    object.insert(QStringLiteral("live_sgr_mouse_reporting"), false);
    object.insert(QStringLiteral("published_sgr_mouse_reporting"), false);
    object.insert(QStringLiteral("published_mouse_tracking"), false);
    object.insert(QStringLiteral("backend_drain_calls"), 1);
    object.insert(QStringLiteral("backend_drain_elapsed_ns"), 0);
    return object;
}

struct Replay_tool_process_result
{
    bool                 finished    = false;
    QProcess::ExitStatus exit_status = QProcess::CrashExit;
    int                  exit_code   = -1;
    QByteArray           stdout_text;
    QByteArray           stderr_text;
};

Replay_tool_process_result run_replay_tool_with_arguments(
    const QString& replay_tool_path,
    const QStringList& arguments)
{
    Replay_tool_process_result result;
    QProcess process;
    process.start(replay_tool_path, arguments);
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

Replay_tool_process_result run_replay_tool(
    const QString& replay_tool_path,
    const QString& transcript_path)
{
    return run_replay_tool_with_arguments(
        replay_tool_path,
        {QStringLiteral("--strict-all-snapshots"), transcript_path});
}

void print_replay_tool_output(const Replay_tool_process_result& result)
{
    std::cerr << result.stdout_text.constData() << result.stderr_text.constData();
}

std::optional<int> replay_stdout_metric(
    const QByteArray& stdout_text,
    const QByteArray& metric_name)
{
    const QByteArray prefix = metric_name + QByteArrayLiteral("=");
    const QList<QByteArray> lines = stdout_text.split('\n');
    for (const QByteArray& line : lines) {
        const QList<QByteArray> tokens = line.split(' ');
        for (const QByteArray& token : tokens) {
            if (!token.startsWith(prefix)) {
                continue;
            }

            bool ok = false;
            const int value = token.mid(prefix.size()).toInt(&ok);
            if (ok) {
                return value;
            }
        }
    }

    return std::nullopt;
}

QStringList transcript_event_sources(
    const std::vector<term::Terminal_transcript_event>& events,
    const QString&                                      kind)
{
    QStringList sources;
    for (const term::Terminal_transcript_event& event : events) {
        if (event.kind != kind) {
            continue;
        }

        const QString source = event.object.value(QStringLiteral("source")).toString();
        if (!source.isEmpty() && !sources.contains(source)) {
            sources.push_back(source);
        }
    }

    sources.sort();
    return sources;
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

bool rewrite_first_snapshot_reason(const QString& path, const QString& reason)
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
            object.insert(QStringLiteral("reason"), reason);
            rewritten = true;
        }
        lines.push_back(json_line(object));
    }
    file.close();

    return rewritten && write_transcript_lines(path, lines);
}

bool rewrite_first_recovered_row_provenance_source(
    const QString& path,
    const QString& source)
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
            QJsonArray row_provenance =
                object.value(QStringLiteral("row_provenance")).toArray();
            for (int row = 0; row < row_provenance.size(); ++row) {
                QJsonObject provenance = row_provenance.at(row).toObject();
                if (provenance.value(QStringLiteral("source")).toString() ==
                    QStringLiteral("recovered_primary_repaint"))
                {
                    provenance.insert(QStringLiteral("source"), source);
                    row_provenance.replace(row, provenance);
                    object.insert(QStringLiteral("row_provenance"), row_provenance);
                    rewritten = true;
                    break;
                }
            }
        }
        lines.push_back(json_line(object));
    }
    file.close();

    return rewritten && write_transcript_lines(path, lines);
}

bool remove_snapshot_row_provenance_sources(const QString& path)
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
        if (object.value(QStringLiteral("kind")).toString() == QStringLiteral("snapshot")) {
            QJsonArray row_provenance =
                object.value(QStringLiteral("row_provenance")).toArray();
            bool row_provenance_changed = false;
            for (int row = 0; row < row_provenance.size(); ++row) {
                QJsonObject provenance = row_provenance.at(row).toObject();
                if (!provenance.contains(QStringLiteral("source"))) {
                    continue;
                }

                provenance.remove(QStringLiteral("source"));
                row_provenance.replace(row, provenance);
                row_provenance_changed = true;
            }

            if (row_provenance_changed) {
                object.insert(QStringLiteral("row_provenance"), row_provenance);
                rewritten = true;
            }
        }
        lines.push_back(json_line(object));
    }
    file.close();

    return rewritten && write_transcript_lines(path, lines);
}

bool remove_snapshot_public_scroll_schema_fields(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    const QStringList schema_fields = {
        QStringLiteral("snapshot_basis"),
        QStringLiteral("snapshot_purpose"),
        QStringLiteral("effective_synchronized_output_scroll_policy"),
        QStringLiteral("synchronized_output_scroll_policy_change_event"),
        QStringLiteral("diagnostic_reason"),
        QStringLiteral("public_projection_generation"),
        QStringLiteral("public_viewport_before"),
        QStringLiteral("public_viewport_after"),
        QStringLiteral("live_viewport_before_on_release"),
        QStringLiteral("live_viewport_after_on_release"),
        QStringLiteral("visible_scroll_applied"),
        QStringLiteral("live_content_publication_blocked"),
        QStringLiteral("release_reconciliation_result"),
        QStringLiteral("hidden_row_eligibility"),
        QStringLiteral("hidden_row_clamp_reason"),
        QStringLiteral("public_projection_disable_reason"),
    };

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
        if (object.value(QStringLiteral("kind")).toString() == QStringLiteral("snapshot")) {
            for (const QString& field : schema_fields) {
                object.remove(field);
            }
            rewritten = true;
        }
        lines.push_back(json_line(object));
    }
    file.close();

    return rewritten && write_transcript_lines(path, lines);
}

bool corrupt_first_public_projection_scroll_snapshot_to_self_consistent_wrong_content(
    const QString& path)
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
        if (!rewritten &&
            object.value(QStringLiteral("kind")).toString() == QStringLiteral("snapshot") &&
            object.value(QStringLiteral("snapshot_basis")).toString() ==
                QStringLiteral("PUBLIC_PROJECTION") &&
            object.value(QStringLiteral("snapshot_purpose")).toString() == QStringLiteral("SCROLL"))
        {
            QJsonArray visible_rows = object.value(QStringLiteral("visible_rows")).toArray();
            if (!visible_rows.isEmpty()) {
                const QString corrupted_text =
                    QStringLiteral("corrupted-public-row");
                QJsonObject first_row = visible_rows.first().toObject();
                first_row.insert(QStringLiteral("text"), corrupted_text);
                first_row.insert(QStringLiteral("hash64"), text_hash64(corrupted_text));
                visible_rows.replace(0, first_row);
                object.insert(QStringLiteral("visible_rows"), visible_rows);
            }

            object.insert(
                QStringLiteral("public_viewport_after"),
                object.value(QStringLiteral("viewport")).toObject());
            rewritten = true;
        }
        lines.push_back(json_line(object));
    }
    file.close();

    return rewritten && write_transcript_lines(path, lines);
}

int insert_final_snapshot_copy_before_last_snapshot(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return 0;
    }

    std::vector<QJsonObject> objects;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine();
        QJsonParseError parse_error;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
            return 0;
        }

        objects.push_back(document.object());
    }
    file.close();

    std::optional<std::size_t> last_snapshot_index;
    for (std::size_t index = 0U; index < objects.size(); ++index) {
        if (objects[index].value(QStringLiteral("kind")).toString() ==
            QStringLiteral("snapshot"))
        {
            last_snapshot_index = index;
        }
    }

    if (!last_snapshot_index.has_value()) {
        return 0;
    }

    std::vector<QJsonObject> rewritten_objects;
    rewritten_objects.reserve(objects.size() + 1U);
    for (std::size_t index = 0U; index < objects.size(); ++index) {
        if (index == *last_snapshot_index) {
            rewritten_objects.push_back(objects[index]);
        }
        rewritten_objects.push_back(std::move(objects[index]));
    }

    std::vector<QByteArray> lines;
    lines.reserve(rewritten_objects.size());
    for (std::size_t index = 0U; index < rewritten_objects.size(); ++index) {
        rewritten_objects[index].insert(
            QStringLiteral("event_index"),
            static_cast<qint64>(index));
        lines.push_back(json_line(std::move(rewritten_objects[index])));
    }

    return write_transcript_lines(path, lines) ? 1 : 0;
}

int remove_last_snapshot_event(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return 0;
    }

    std::vector<QJsonObject> objects;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine();
        QJsonParseError parse_error;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
            return 0;
        }

        objects.push_back(document.object());
    }
    file.close();

    std::optional<std::size_t> last_snapshot_index;
    for (std::size_t index = 0U; index < objects.size(); ++index) {
        if (objects[index].value(QStringLiteral("kind")).toString() ==
            QStringLiteral("snapshot"))
        {
            last_snapshot_index = index;
        }
    }

    if (!last_snapshot_index.has_value()) {
        return 0;
    }

    const auto erase_offset =
        static_cast<std::vector<QJsonObject>::difference_type>(*last_snapshot_index);
    objects.erase(objects.begin() + erase_offset);

    std::vector<QByteArray> lines;
    lines.reserve(objects.size());
    for (std::size_t index = 0U; index < objects.size(); ++index) {
        objects[index].insert(
            QStringLiteral("event_index"),
            static_cast<qint64>(index));
        lines.push_back(json_line(std::move(objects[index])));
    }

    return write_transcript_lines(path, lines) ? 1 : 0;
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
        case term::Terminal_viewport_scroll_action::DEFERRED_INTENT_RECORDED:
            return QStringLiteral("deferred_intent_recorded");
        case term::Terminal_viewport_scroll_action::TERMINAL_INPUT:
            return QStringLiteral("terminal_input");
    }

    return QStringLiteral("unknown");
}

QString snapshot_row_text(const term::Terminal_render_snapshot& snapshot, int row)
{
    const term::Terminal_render_snapshot_row_content_view rows(snapshot);
    return term::selected_text_from_render_snapshot_row(
        rows.row_at(row),
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

bool transcript_uses_immediate_public_projection(
    const std::vector<term::Terminal_transcript_event>& events)
{
    for (const term::Terminal_transcript_event& event : events) {
        if (event.object.value(QStringLiteral("snapshot_basis")).toString() ==
                QStringLiteral("PUBLIC_PROJECTION") ||
            event.object.value(
                QStringLiteral("effective_synchronized_output_scroll_policy")).toString() ==
                QStringLiteral("IMMEDIATE_PUBLIC_PROJECTION"))
        {
            return true;
        }
    }

    return false;
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
        config.selection_viewport_projection_enabled =
            session_config.value(QStringLiteral("selection_viewport_projection_enabled")).toBool();
        const QJsonValue explicit_scroll_policy =
            session_config.value(QStringLiteral("synchronized_output_scroll_policy"));
        if (explicit_scroll_policy.toString() == QStringLiteral("IMMEDIATE_PUBLIC_PROJECTION"))
        {
            config.synchronized_output_scroll_policy =
                term::Terminal_synchronized_output_scroll_policy::IMMEDIATE_PUBLIC_PROJECTION;
        }
        else
        if (!explicit_scroll_policy.isString() && transcript_uses_immediate_public_projection(events)) {
            config.synchronized_output_scroll_policy =
                term::Terminal_synchronized_output_scroll_policy::IMMEDIATE_PUBLIC_PROJECTION;
        }
        return config;
    }

    const int recorded_scrollback_rows = max_recorded_scrollback_rows(events);
    config.scrollback_limit =
        recorded_scrollback_rows > 0 ? recorded_scrollback_rows : k_surface_default_scrollback_limit;
    config.recover_scrollback_from_primary_repaints = false;
    if (transcript_uses_immediate_public_projection(events)) {
        config.synchronized_output_scroll_policy =
            term::Terminal_synchronized_output_scroll_policy::IMMEDIATE_PUBLIC_PROJECTION;
    }
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

term::Terminal_buffer_id buffer_from_name(const QString& buffer)
{
    return buffer == QStringLiteral("alternate")
        ? term::Terminal_buffer_id::ALTERNATE
        : term::Terminal_buffer_id::PRIMARY;
}

term::Terminal_alternate_screen_scroll_policy alternate_policy_from_name(
    const QString& policy)
{
    return policy == QStringLiteral("wheel_to_terminal_input")
        ? term::Terminal_alternate_screen_scroll_policy::WHEEL_TO_TERMINAL_INPUT
        : term::Terminal_alternate_screen_scroll_policy::KEEP_AT_TAIL;
}

term::Terminal_viewport_state viewport_from_object(const QJsonObject& object)
{
    term::Terminal_viewport_state viewport;
    viewport.active_buffer =
        buffer_from_name(object.value(QStringLiteral("active_buffer")).toString());
    viewport.scrollback_rows =
        object.value(QStringLiteral("scrollback_rows")).toInt();
    viewport.visible_rows =
        object.value(QStringLiteral("visible_rows")).toInt();
    viewport.offset_from_tail =
        object.value(QStringLiteral("offset_from_tail")).toInt();
    viewport.follow_tail =
        object.value(QStringLiteral("follow_tail")).toBool();
    viewport.alternate_screen_scroll_policy =
        alternate_policy_from_name(
            object.value(QStringLiteral("alternate_screen_scroll_policy")).toString());
    return viewport;
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

bool visible_viewport_offset_matches_recorded_after(
    const term::Terminal_session&          session,
    const term::Terminal_transcript_event& event)
{
    const std::optional<term::Terminal_render_snapshot> snapshot =
        session.latest_render_snapshot();
    return snapshot.has_value() &&
        viewport_offset_matches_recorded_after(snapshot->viewport, event);
}

bool scroll_event_uses_published_state_source(
    const term::Terminal_transcript_event& event)
{
    return
        event.object.value(QStringLiteral("source")).toString() ==
            QStringLiteral("surface.text_area.wheel") &&
        event.object.contains(QStringLiteral("viewport_before"));
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
    else
    if (scroll_event_uses_published_state_source(event)) {
        result = session.scroll_viewport_lines_from_published_state(
            event.object.value(QStringLiteral("requested_line_delta")).toInt(),
            viewport_from_object(event.object.value(QStringLiteral("viewport_before")).toObject()));
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
            if (result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED ||
                result.action ==
                    term::Terminal_viewport_scroll_action::DEFERRED_INTENT_RECORDED)
            {
                pending_scroll_intent = pending_scroll_intent_from_event(event, result);
            }
        }
        else
        if (event.kind == QStringLiteral("surface.scroll")) {
            const bool pending_result_already_applied =
                pending_scroll_intent.has_value() &&
                scroll_event_matches_pending_intent(event, *pending_scroll_intent) &&
                (pending_scroll_intent->result.action ==
                    term::Terminal_viewport_scroll_action::DEFERRED_INTENT_RECORDED ||
                    viewport_offset_matches_recorded_after(session.viewport_state(), event) ||
                    visible_viewport_offset_matches_recorded_after(session, event));
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
    QJsonObject explicit_visible_scroll_fields;
    term::insert_wheel_trace_scroll_publication_fields(
        explicit_visible_scroll_fields,
        true,
        true,
        true);
    ok &= check(
        explicit_visible_scroll_fields.value(QStringLiteral("visible_scroll_applied")).toBool(),
        "wheel trace publication fields allow explicit visible public scroll while content is blocked");

    QJsonObject wheel_trace;
    wheel_trace.insert(QStringLiteral("source"), QStringLiteral("surface.text_area.wheel"));
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
    wheel_trace.insert(
        QStringLiteral("effective_synchronized_output_scroll_policy"),
        QStringLiteral("IMMEDIATE_PUBLIC_PROJECTION"));
    wheel_trace.insert(
        QStringLiteral("synchronized_output_scroll_policy_change_event"),
        QStringLiteral("changed_mid_hold"));
    wheel_trace.insert(
        QStringLiteral("diagnostic_reason"),
        QStringLiteral("synchronized_output_scroll_policy_changed_mid_hold"));
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
    QJsonObject explicit_blocked_wheel_trace = wheel_trace;
    explicit_blocked_wheel_trace.insert(
        QStringLiteral("outcome"),
        QStringLiteral("public_projection_scroll_visible"));
    term::insert_wheel_trace_scroll_publication_fields(
        explicit_blocked_wheel_trace,
        true,
        true,
        true);
    ok &= check(recorder->record_surface_wheel_trace(std::move(wheel_trace)),
        "surface.wheel_trace writes");
    ok &= check(
        recorder->record_surface_wheel_trace(std::move(explicit_blocked_wheel_trace)),
        "explicit visible surface.wheel_trace writes while publication is blocked");

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
    snapshot.basis   = term::Terminal_render_snapshot_basis::PUBLIC_PROJECTION;
    snapshot.purpose = term::Terminal_render_snapshot_purpose::SCROLL;
    snapshot.public_scroll_diagnostics.effective_policy =
        term::Terminal_synchronized_output_scroll_policy::IMMEDIATE_PUBLIC_PROJECTION;
    snapshot.public_scroll_diagnostics.policy_change_event =
        term::Terminal_synchronized_output_policy_change_event::CHANGED_MID_HOLD;
    snapshot.public_scroll_diagnostics.diagnostic_reason =
        term::Terminal_public_scroll_diagnostic_reason::
            PUBLIC_PROJECTION_MEMORY_PRESSURE_INVALIDATED;
    snapshot.public_scroll_diagnostics.public_projection_generation = 42U;
    snapshot.public_scroll_diagnostics.public_viewport_before = viewport_before;
    snapshot.public_scroll_diagnostics.public_viewport_after  = viewport_after;
    snapshot.public_scroll_diagnostics.live_viewport_before_on_release = viewport_after;
    snapshot.public_scroll_diagnostics.live_viewport_after_on_release  = viewport_before;
    snapshot.public_scroll_diagnostics.visible_scroll_applied = true;
    snapshot.public_scroll_diagnostics.live_content_publication_blocked = true;
    snapshot.public_scroll_diagnostics.release_reconciliation_result =
        term::Terminal_release_reconciliation_result::EXACT_ANCHOR;
    snapshot.public_scroll_diagnostics.hidden_row_eligibility =
        term::Terminal_hidden_row_eligibility::ELIGIBLE;
    snapshot.public_scroll_diagnostics.hidden_row_clamp_reason =
        term::Terminal_hidden_row_clamp_reason::LIVE_VIEWPORT_BOUNDARY;
    snapshot.public_scroll_diagnostics.public_projection_disable_reason =
        term::Terminal_public_projection_disable_reason::MEMORY_PRESSURE;
    ok &= check(recorder->record_snapshot(8U, QStringLiteral("test snapshot"), snapshot),
        "compact snapshot writes when enabled");

    constexpr int k_public_projection_disable_reason_case_count = 5;
    const term::Terminal_public_projection_disable_reason disable_reason_cases[
        k_public_projection_disable_reason_case_count] = {
        term::Terminal_public_projection_disable_reason::NONE,
        term::Terminal_public_projection_disable_reason::GEOMETRY_INVALIDATED,
        term::Terminal_public_projection_disable_reason::MEMORY_PRESSURE,
        term::Terminal_public_projection_disable_reason::PROJECTION_INVALIDATED,
        term::Terminal_public_projection_disable_reason::UNSUPPORTED_BUFFER,
    };
    for (int i = 0; i < k_public_projection_disable_reason_case_count; ++i) {
        term::Terminal_render_snapshot disable_reason_snapshot =
            term::make_empty_render_snapshot(
                {4, 12},
                viewport_after,
                20U + static_cast<std::uint64_t>(i));
        disable_reason_snapshot.public_scroll_diagnostics.public_projection_disable_reason =
            disable_reason_cases[i];
        ok &= check(
            recorder->record_snapshot(
                20U + static_cast<std::uint64_t>(i),
                QStringLiteral("disable reason snapshot"),
                disable_reason_snapshot),
            "compact snapshot public projection disable reason writes");
    }

    term::Terminal_render_snapshot default_snapshot =
        term::make_empty_render_snapshot({4, 12}, viewport_after, 9U);
    ok &= check(
        recorder->record_snapshot(
            9U,
            QStringLiteral("default diagnostics snapshot"),
            default_snapshot),
        "default compact snapshot writes when enabled");

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
        ok &= check(
            wheel_trace_event->object.value(
                QStringLiteral("effective_synchronized_output_scroll_policy")).toString() ==
                QStringLiteral("IMMEDIATE_PUBLIC_PROJECTION"),
            "surface.wheel_trace records effective synchronized-output scroll policy");
        ok &= check(
            wheel_trace_event->object.value(
                QStringLiteral("synchronized_output_scroll_policy_change_event")).toString() ==
                QStringLiteral("changed_mid_hold"),
            "surface.wheel_trace records mid-hold policy-change event");
        ok &= check(
            wheel_trace_event->object.value(QStringLiteral("diagnostic_reason")).toString() ==
                QStringLiteral("synchronized_output_scroll_policy_changed_mid_hold"),
            "surface.wheel_trace records mid-hold policy-change diagnostic reason");
    }
    const std::optional<term::Terminal_transcript_event> explicit_wheel_trace_event =
        last_event(*events, QStringLiteral("surface.wheel_trace"));
    ok &= check(explicit_wheel_trace_event.has_value(),
        "explicit visible surface.wheel_trace event is present");
    if (explicit_wheel_trace_event.has_value()) {
        ok &= check(
            explicit_wheel_trace_event->object.value(QStringLiteral("render_publication_blocked")).toBool() &&
            explicit_wheel_trace_event->object.value(QStringLiteral("visible_scroll_applied")).toBool(),
            "explicit surface.wheel_trace round-trips visible scroll while publication is blocked");
    }
    ok &= check(first_event(*events, QStringLiteral("snapshot")).has_value(),
        "snapshot compact event is present");
    ok &= check(
        event_count(*events, QStringLiteral("snapshot")) ==
            2 + k_public_projection_disable_reason_case_count,
        "snapshot compact events include explicit, disable-reason, and default diagnostics");
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
        ok &= check(
            snapshot_event->object.value(QStringLiteral("snapshot_basis")).toString() ==
                QStringLiteral("PUBLIC_PROJECTION") &&
            snapshot_event->object.value(QStringLiteral("snapshot_purpose")).toString() ==
                QStringLiteral("SCROLL"),
            "snapshot diagnostics include public projection basis and scroll purpose");
        ok &= check(
            snapshot_event->object.value(
                QStringLiteral("effective_synchronized_output_scroll_policy")).toString() ==
                QStringLiteral("IMMEDIATE_PUBLIC_PROJECTION"),
            "snapshot diagnostics round-trip effective synchronized-output scroll policy");
        ok &= check(
            snapshot_event->object.value(
                QStringLiteral("synchronized_output_scroll_policy_change_event")).toString() ==
                QStringLiteral("changed_mid_hold"),
            "snapshot diagnostics round-trip policy-change event");
        ok &= check(
            snapshot_event->object.value(QStringLiteral("diagnostic_reason")).toString() ==
                QStringLiteral("public_projection_memory_pressure_invalidated"),
            "snapshot diagnostics round-trip memory-pressure diagnostic reason");
        ok &= check(
            snapshot_event->object.value(QStringLiteral("live_content_publication_blocked")).toBool(),
            "snapshot diagnostics round-trip live content publication blocking field");
        ok &= check(
            snapshot_event->object.value(QStringLiteral("public_projection_generation")).toInt() == 42,
            "snapshot diagnostics round-trip public projection generation");
        ok &= check(
            snapshot_event->object.value(QStringLiteral("public_viewport_before")).toObject()
                .value(QStringLiteral("offset_from_tail")).toInt() == 0,
            "snapshot diagnostics round-trip public viewport before");
        ok &= check(
            snapshot_event->object.value(QStringLiteral("public_viewport_after")).toObject()
                .value(QStringLiteral("offset_from_tail")).toInt() == 3,
            "snapshot diagnostics round-trip public viewport after");
        ok &= check(
            snapshot_event->object.value(QStringLiteral("visible_scroll_applied")).toBool(),
            "snapshot diagnostics round-trip visible scroll application field");
        ok &= check(
            snapshot_event->object.value(QStringLiteral("release_reconciliation_result")).toString() ==
                QStringLiteral("exact_anchor"),
            "snapshot diagnostics round-trip release reconciliation result");
        ok &= check(
            snapshot_event->object.value(QStringLiteral("hidden_row_eligibility")).toString() ==
                QStringLiteral("eligible"),
            "snapshot diagnostics round-trip hidden-row eligibility");
        ok &= check(
            snapshot_event->object.value(QStringLiteral("hidden_row_clamp_reason")).toString() ==
                QStringLiteral("live_viewport_boundary"),
            "snapshot diagnostics round-trip hidden-row clamp reason");
        ok &= check(
            snapshot_event->object.value(QStringLiteral("public_projection_disable_reason")).toString() ==
                QStringLiteral("memory_pressure"),
            "snapshot diagnostics round-trip memory-pressure public projection disable reason");
    }
    for (term::Terminal_public_projection_disable_reason disable_reason :
         disable_reason_cases)
    {
        const QString reason_name =
            term::public_projection_disable_reason_name(disable_reason);
        bool found = false;
        for (const term::Terminal_transcript_event& event : *events) {
            if (event.kind == QStringLiteral("snapshot") &&
                event.object.value(QStringLiteral("public_projection_disable_reason")).toString() ==
                    reason_name)
            {
                found = true;
                break;
            }
        }
        ok &= check(found,
            "snapshot diagnostics round-trip every public projection disable reason");
    }
    const std::optional<term::Terminal_transcript_event> default_snapshot_event =
        last_event(*events, QStringLiteral("snapshot"));
    if (default_snapshot_event.has_value()) {
        ok &= check(
            default_snapshot_event->object.value(QStringLiteral("snapshot_basis")).toString() ==
                QStringLiteral("LIVE_CONTENT") &&
            default_snapshot_event->object.value(QStringLiteral("snapshot_purpose")).toString() ==
                QStringLiteral("CONTENT"),
            "default snapshot diagnostics round-trip live content basis and content purpose");
        ok &= check(
            default_snapshot_event->object.value(
                QStringLiteral("effective_synchronized_output_scroll_policy")).toString() ==
                QStringLiteral("DEFER_UNTIL_CONTENT_PUBLICATION") &&
            default_snapshot_event->object.value(
                QStringLiteral("synchronized_output_scroll_policy_change_event")).toString() ==
                QStringLiteral("none") &&
            default_snapshot_event->object.value(QStringLiteral("diagnostic_reason")).toString() ==
                QStringLiteral("none"),
            "default snapshot diagnostics round-trip policy defaults");
        ok &= check(
            default_snapshot_event->object.value(QStringLiteral("public_projection_generation")).toInt() == 0 &&
            !default_snapshot_event->object.value(QStringLiteral("visible_scroll_applied")).toBool() &&
            !default_snapshot_event->object.value(QStringLiteral("live_content_publication_blocked")).toBool(),
            "default snapshot diagnostics round-trip public scroll flags");
        ok &= check(
            default_snapshot_event->object.value(QStringLiteral("release_reconciliation_result")).toString() ==
                QStringLiteral("none") &&
            default_snapshot_event->object.value(QStringLiteral("hidden_row_eligibility")).toString() ==
                QStringLiteral("not_evaluated") &&
            default_snapshot_event->object.value(QStringLiteral("hidden_row_clamp_reason")).toString() ==
                QStringLiteral("none") &&
            default_snapshot_event->object.value(QStringLiteral("public_projection_disable_reason")).toString() ==
                QStringLiteral("none"),
            "default snapshot diagnostics round-trip reconciliation defaults");
    }
    return ok;
}

bool test_writer_records_recovery_flag_values()
{
    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary recovery flag directory is valid")) {
        return false;
    }

    bool ok = true;
    auto check_recorded_recovery_flag = [&](bool recovery_enabled, const char* label) {
        const QString path = temp_dir.filePath(
            recovery_enabled
                ? QStringLiteral("recovery-enabled.ndjson")
                : QStringLiteral("recovery-disabled.ndjson"));

        QString error;
        std::shared_ptr<term::Terminal_transcript_recorder> recorder =
            term::Terminal_transcript_recorder::create(path, true, &error);
        ok &= check(recorder != nullptr, label);
        if (recorder == nullptr) {
            std::cerr << error.toStdString() << '\n';
            return;
        }

        term::Terminal_session_config session_config;
        session_config.recover_scrollback_from_primary_repaints = recovery_enabled;
        ok &= check(
            recorder->record_session_start(1U, valid_launch_config(), session_config),
            label);
        recorder.reset();

        const std::optional<std::vector<term::Terminal_transcript_event>> events =
            term::read_terminal_transcript(path, &error);
        ok &= check(events.has_value(), label);
        if (!events.has_value()) {
            std::cerr << error.toStdString() << '\n';
            return;
        }

        const std::optional<term::Terminal_transcript_event> start =
            first_event(*events, QStringLiteral("session.start"));
        ok &= check(start.has_value(), label);
        if (!start.has_value()) {
            return;
        }

        const QJsonObject session_config_object =
            start->object.value(QStringLiteral("session_config")).toObject();
        ok &= check(
            session_config_object.contains(
                QStringLiteral("recover_scrollback_from_primary_repaints")) &&
                session_config_object.value(
                    QStringLiteral("recover_scrollback_from_primary_repaints")).toBool() ==
                    recovery_enabled,
            label);
    };

    check_recorded_recovery_flag(true,  "writer records enabled recovery flag");
    check_recorded_recovery_flag(false, "writer records disabled recovery flag");
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

bool test_reader_rejects_invalid_leading_event_fields()
{
    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary leading-field malformed directory is valid")) {
        return false;
    }

    bool ok = true;

    QJsonObject header_bool_mistyped = valid_header_object();
    header_bool_mistyped.insert(
        QStringLiteral("snapshot_diagnostics"), QStringLiteral("false"));
    ok &= expect_reader_failure(
        temp_dir,
        QStringLiteral("header-bool-mistyped.ndjson"),
        {json_line(header_bool_mistyped)},
        QStringLiteral("missing or invalid bool field snapshot_diagnostics"));

    QJsonObject start_missing_sequence = valid_session_start_object(1U);
    start_missing_sequence.remove(QStringLiteral("session_sequence"));
    ok &= expect_reader_failure(
        temp_dir,
        QStringLiteral("start-missing-sequence.ndjson"),
        {json_line(valid_header_object()), json_line(start_missing_sequence)},
        QStringLiteral("missing or invalid integer field session_sequence"));

    QJsonObject start_negative_sequence = valid_session_start_object(1U);
    start_negative_sequence.insert(QStringLiteral("session_sequence"), -1);
    ok &= expect_reader_failure(
        temp_dir,
        QStringLiteral("start-negative-sequence.ndjson"),
        {json_line(valid_header_object()), json_line(start_negative_sequence)},
        QStringLiteral("negative integer field session_sequence"));

    QJsonObject wheel_trace_missing_route;
    wheel_trace_missing_route.insert(
        QStringLiteral("kind"), QStringLiteral("surface.wheel_trace"));
    insert_u64(wheel_trace_missing_route, QStringLiteral("event_index"), 1U);
    wheel_trace_missing_route.insert(QStringLiteral("source"), QStringLiteral("wheel"));
    ok &= expect_reader_failure(
        temp_dir,
        QStringLiteral("wheel-trace-missing-route.ndjson"),
        {json_line(valid_header_object()), json_line(wheel_trace_missing_route)},
        QStringLiteral("missing or invalid string field route"));

    QJsonObject wheel_trace_accepted_mistyped = wheel_trace_missing_route;
    wheel_trace_accepted_mistyped.insert(QStringLiteral("route"), QStringLiteral("local"));
    wheel_trace_accepted_mistyped.insert(QStringLiteral("outcome"), QStringLiteral("scrolled"));
    wheel_trace_accepted_mistyped.insert(QStringLiteral("accepted"), 1);
    ok &= expect_reader_failure(
        temp_dir,
        QStringLiteral("wheel-trace-accepted-mistyped.ndjson"),
        {json_line(valid_header_object()), json_line(wheel_trace_accepted_mistyped)},
        QStringLiteral("missing or invalid bool field accepted"));

    return ok;
}

bool test_reader_accepts_session_config_without_recovery_flag()
{
    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary legacy session-config directory is valid")) {
        return false;
    }

    QJsonObject session_config;
    session_config.insert(QStringLiteral("scrollback_limit"), 8);
    session_config.insert(QStringLiteral("effective_scrollback_limit"), 8);
    session_config.insert(
        QStringLiteral("synchronized_output_scroll_policy"),
        QStringLiteral("DEFER_UNTIL_CONTENT_PUBLICATION"));

    QJsonObject start = valid_session_start_object(1U);
    start.insert(QStringLiteral("session_config"), session_config);

    const QString path = temp_dir.filePath(QStringLiteral("legacy-session-config.ndjson"));
    bool ok = true;
    ok &= check(
        write_transcript_lines(path, {json_line(valid_header_object()), json_line(start)}),
        "legacy session-config transcript writes");

    QString error;
    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(path, &error);
    ok &= check(events.has_value(),
        "reader accepts session_config without restored recovery flag");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
    }
    return ok;
}

bool test_reader_defaults_missing_row_provenance_source()
{
    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary row-provenance source directory is valid")) {
        return false;
    }

    QJsonObject snapshot = valid_compact_snapshot_object(1U);
    QJsonArray row_provenance = snapshot.value(QStringLiteral("row_provenance")).toArray();
    QJsonObject provenance = row_provenance.at(0).toObject();
    provenance.remove(QStringLiteral("source"));
    row_provenance.replace(0, provenance);
    snapshot.insert(QStringLiteral("row_provenance"), row_provenance);

    const QString path =
        temp_dir.filePath(QStringLiteral("missing-row-provenance-source.ndjson"));
    bool ok = true;
    ok &= check(
        write_transcript_lines(path, {json_line(valid_header_object()), json_line(snapshot)}),
        "missing row-provenance source transcript writes");

    QString error;
    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(path, &error);
    ok &= check(events.has_value(), "reader accepts missing row_provenance source");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    const std::optional<term::Terminal_transcript_event> snapshot_event =
        first_event(*events, QStringLiteral("snapshot"));
    const QJsonArray defaulted_provenance = snapshot_event.has_value()
        ? snapshot_event->object.value(QStringLiteral("row_provenance")).toArray()
        : QJsonArray{};
    ok &= check(snapshot_event.has_value() &&
            !defaulted_provenance.isEmpty() &&
            defaulted_provenance.at(0).toObject().value(QStringLiteral("source")).toString() ==
                QStringLiteral("terminal_storage"),
        "reader defaults missing row_provenance source to terminal_storage");
    return ok;
}

bool test_reader_rejects_invalid_row_provenance_source()
{
    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary invalid row-provenance source directory is valid")) {
        return false;
    }

    QJsonObject snapshot = valid_compact_snapshot_object(1U);
    QJsonArray row_provenance = snapshot.value(QStringLiteral("row_provenance")).toArray();
    QJsonObject provenance = row_provenance.at(0).toObject();
    provenance.insert(QStringLiteral("source"), QStringLiteral("invalid_source"));
    row_provenance.replace(0, provenance);
    snapshot.insert(QStringLiteral("row_provenance"), row_provenance);

    return expect_reader_failure(
        temp_dir,
        QStringLiteral("invalid-row-provenance-source.ndjson"),
        {json_line(valid_header_object()), json_line(snapshot)},
        QStringLiteral("row_provenance source is invalid"));
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

bool test_reader_rejects_invalid_snapshot_public_scroll_enum_values()
{
    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary invalid snapshot enum directory is valid")) {
        return false;
    }

    struct Invalid_enum_case
    {
        QString field_name;
        QString invalid_value;
    };

    const Invalid_enum_case cases[] = {
        {QStringLiteral("snapshot_basis"), QStringLiteral("NOT_A_BASIS")},
        {QStringLiteral("snapshot_purpose"), QStringLiteral("NOT_A_PURPOSE")},
        {
            QStringLiteral("effective_synchronized_output_scroll_policy"),
            QStringLiteral("NOT_A_POLICY"),
        },
        {
            QStringLiteral("synchronized_output_scroll_policy_change_event"),
            QStringLiteral("not_a_change_event"),
        },
        {QStringLiteral("diagnostic_reason"), QStringLiteral("not_a_reason")},
        {
            QStringLiteral("release_reconciliation_result"),
            QStringLiteral("not_a_release_result"),
        },
        {
            QStringLiteral("hidden_row_eligibility"),
            QStringLiteral("not_a_hidden_row_eligibility"),
        },
        {
            QStringLiteral("hidden_row_clamp_reason"),
            QStringLiteral("not_a_hidden_row_clamp_reason"),
        },
        {
            QStringLiteral("public_projection_disable_reason"),
            QStringLiteral("not_a_projection_disable_reason"),
        },
    };

    bool ok = true;
    for (const Invalid_enum_case& enum_case : cases) {
        QJsonObject snapshot = valid_compact_snapshot_object(1U);
        snapshot.insert(enum_case.field_name, enum_case.invalid_value);
        ok &= expect_reader_failure(
            temp_dir,
            QStringLiteral("compact-snapshot-invalid-%1.ndjson").arg(enum_case.field_name),
            {json_line(valid_header_object()), json_line(snapshot)},
            enum_case.field_name);
    }
    return ok;
}

bool test_reader_rejects_snapshot_basis_purpose_mismatches()
{
    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary snapshot basis-purpose mismatch directory is valid")) {
        return false;
    }

    struct Mismatch_case
    {
        QString basis;
        QString purpose;
        QString filename;
    };

    const Mismatch_case cases[] = {
        {
            QStringLiteral("PUBLIC_PROJECTION"),
            QStringLiteral("CONTENT"),
            QStringLiteral("public-content.ndjson"),
        },
        {
            QStringLiteral("LIVE_CONTENT"),
            QStringLiteral("SCROLL"),
            QStringLiteral("live-scroll.ndjson"),
        },
    };

    bool ok = true;
    for (const Mismatch_case& mismatch : cases) {
        QJsonObject snapshot = valid_compact_snapshot_object(1U);
        snapshot.insert(QStringLiteral("snapshot_basis"), mismatch.basis);
        snapshot.insert(QStringLiteral("snapshot_purpose"), mismatch.purpose);
        ok &= expect_reader_failure(
            temp_dir,
            mismatch.filename,
            {json_line(valid_header_object()), json_line(snapshot)},
            QStringLiteral("incompatible snapshot_basis"));
    }
    return ok;
}

bool test_reader_accepts_selection_projection_diagnostic_reason()
{
    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary selection projection diagnostic directory is valid")) {
        return false;
    }

    QJsonObject snapshot = valid_compact_snapshot_object(1U);
    snapshot.insert(
        QStringLiteral("diagnostic_reason"),
        QStringLiteral("selection_public_projection_unsupported"));

    const QString path = temp_dir.filePath(QStringLiteral("selection-projection-diagnostic.ndjson"));
    bool ok = true;
    ok &= check(
        write_transcript_lines(path, {json_line(valid_header_object()), json_line(snapshot)}),
        "selection projection diagnostic fixture writes");

    QString error;
    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(path, &error);
    ok &= check(events.has_value(), "selection projection diagnostic transcript parses");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    const std::optional<term::Terminal_transcript_event> event =
        first_event(*events, QStringLiteral("snapshot"));
    ok &= check(
        event.has_value() &&
            event->object.value(QStringLiteral("diagnostic_reason")).toString() ==
                QStringLiteral("selection_public_projection_unsupported"),
        "selection projection diagnostic reason is accepted");
    return ok;
}

bool test_reader_defaults_detached_selection_legacy_reason_to_selection_purpose()
{
    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary detached selection legacy directory is valid")) {
        return false;
    }

    QJsonObject snapshot = valid_compact_snapshot_object(1U);
    snapshot.insert(QStringLiteral("reason"), QStringLiteral("selection visual detached"));

    const QString path = temp_dir.filePath(QStringLiteral("detached-selection-legacy.ndjson"));
    bool ok = true;
    ok &= check(
        write_transcript_lines(path, {json_line(valid_header_object()), json_line(snapshot)}),
        "detached selection legacy fixture writes");

    QString error;
    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(path, &error);
    ok &= check(events.has_value(), "detached selection legacy transcript parses");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    const std::optional<term::Terminal_transcript_event> event =
        first_event(*events, QStringLiteral("snapshot"));
    ok &= check(
        event.has_value() &&
            event->object.value(QStringLiteral("snapshot_basis")).toString() ==
                QStringLiteral("LIVE_CONTENT") &&
            event->object.value(QStringLiteral("snapshot_purpose")).toString() ==
                QStringLiteral("SELECTION_DERIVED"),
        "detached selection legacy reason defaults missing purpose to selection-derived");
    return ok;
}

bool test_reader_defaults_old_wheel_trace_policy_fields()
{
    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary old wheel-trace directory is valid")) {
        return false;
    }

    QJsonObject wheel_trace = valid_wheel_trace_object(1U);
    wheel_trace.remove(QStringLiteral("effective_synchronized_output_scroll_policy"));
    wheel_trace.remove(QStringLiteral("synchronized_output_scroll_policy_change_event"));
    wheel_trace.remove(QStringLiteral("diagnostic_reason"));

    const QString path = temp_dir.filePath(QStringLiteral("old-wheel-trace.ndjson"));
    bool ok = true;
    ok &= check(
        write_transcript_lines(path, {json_line(valid_header_object()), json_line(wheel_trace)}),
        "old wheel-trace fixture writes");

    QString error;
    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(path, &error);
    ok &= check(events.has_value(), "old wheel-trace transcript parses with policy defaults");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    const std::optional<term::Terminal_transcript_event> event =
        first_event(*events, QStringLiteral("surface.wheel_trace"));
    ok &= check(
        event.has_value() &&
            event->object.value(
                QStringLiteral("effective_synchronized_output_scroll_policy")).toString() ==
                QStringLiteral("DEFER_UNTIL_CONTENT_PUBLICATION") &&
            event->object.value(
                QStringLiteral("synchronized_output_scroll_policy_change_event")).toString() ==
                QStringLiteral("none") &&
            event->object.value(QStringLiteral("diagnostic_reason")).toString() ==
                QStringLiteral("none"),
        "old wheel-trace policy fields default to deferred/no-change diagnostics");
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

bool test_replay_config_keeps_explicit_deferred_public_scroll_policy()
{
    QJsonObject session_config;
    session_config.insert(QStringLiteral("scrollback_limit"), 8);
    session_config.insert(QStringLiteral("effective_scrollback_limit"), 8);
    session_config.insert(
        QStringLiteral("synchronized_output_scroll_policy"),
        QStringLiteral("DEFER_UNTIL_CONTENT_PUBLICATION"));

    QJsonObject start = valid_session_start_object(1U);
    start.insert(QStringLiteral("session_config"), session_config);

    QJsonObject public_scroll = valid_compact_snapshot_object(2U);
    public_scroll.insert(QStringLiteral("snapshot_basis"), QStringLiteral("PUBLIC_PROJECTION"));
    public_scroll.insert(QStringLiteral("snapshot_purpose"), QStringLiteral("SCROLL"));
    public_scroll.insert(
        QStringLiteral("effective_synchronized_output_scroll_policy"),
        QStringLiteral("IMMEDIATE_PUBLIC_PROJECTION"));

    const term::Terminal_session_config config = replay_session_config({
        {0U, QStringLiteral("header"), valid_header_object()},
        {1U, QStringLiteral("session.start"), start},
        {2U, QStringLiteral("snapshot"), public_scroll},
    });

    return check(
        config.synchronized_output_scroll_policy ==
            term::Terminal_synchronized_output_scroll_policy::DEFER_UNTIL_CONTENT_PUBLICATION,
        "replay config keeps explicit deferred public scroll policy despite public projection events");
}

bool test_replay_config_defaults_missing_selection_viewport_projection_to_disabled()
{
    QJsonObject session_config;
    session_config.insert(QStringLiteral("scrollback_limit"), 8);
    session_config.insert(QStringLiteral("effective_scrollback_limit"), 8);
    session_config.insert(
        QStringLiteral("synchronized_output_scroll_policy"),
        QStringLiteral("DEFER_UNTIL_CONTENT_PUBLICATION"));

    QJsonObject start = valid_session_start_object(1U);
    start.insert(QStringLiteral("session_config"), session_config);

    const term::Terminal_session_config config = replay_session_config({
        {0U, QStringLiteral("header"), valid_header_object()},
        {1U, QStringLiteral("session.start"), start},
    });

    bool ok = true;
    ok &= check(
        !config.selection_viewport_projection_enabled,
        "replay config defaults missing selection viewport projection to disabled");
    ok &= check(
        !config.recover_scrollback_from_primary_repaints,
        "replay config defaults missing recorded recovery flag to disabled");
    return ok;
}

bool test_replay_config_preserves_recorded_recovery_flag()
{
    auto replay_recovery_flag = [](bool recorded_recovery_flag) {
        QJsonObject session_config;
        session_config.insert(QStringLiteral("scrollback_limit"), 8);
        session_config.insert(QStringLiteral("effective_scrollback_limit"), 8);
        session_config.insert(
            QStringLiteral("recover_scrollback_from_primary_repaints"),
            recorded_recovery_flag);

        QJsonObject start = valid_session_start_object(1U);
        start.insert(QStringLiteral("session_config"), session_config);

        return replay_session_config({
            {0U, QStringLiteral("header"), valid_header_object()},
            {1U, QStringLiteral("session.start"), start},
        }).recover_scrollback_from_primary_repaints;
    };

    bool ok = true;
    ok &= check(
        replay_recovery_flag(true),
        "replay config preserves recorded enabled recovery flag");
    ok &= check(
        !replay_recovery_flag(false),
        "replay config preserves recorded disabled recovery flag");
    return ok;
}

bool test_replay_config_defaults_inferred_recovery_to_disabled()
{
    const term::Terminal_session_config config = replay_session_config({
        {0U, QStringLiteral("header"), valid_header_object()},
        {1U, QStringLiteral("snapshot"), valid_compact_snapshot_object(1U)},
    });

    return check(
        !config.recover_scrollback_from_primary_repaints,
        "replay config defaults inferred recovery to disabled");
}

bool test_replay_tool_preserves_recorded_disabled_recovery_flag(
    const QString& replay_tool_path)
{
    bool ok = true;
    ok &= check(!replay_tool_path.isEmpty(), "recovery flag replay tool path is passed");
    if (replay_tool_path.isEmpty()) {
        return false;
    }

    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "recovery flag replay directory is valid")) {
        return false;
    }

    const QString path = temp_dir.filePath(QStringLiteral("recovery-disabled-replay.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, true, &error);
    ok &= check(recorder != nullptr, "recovery flag replay recorder opens");
    if (recorder == nullptr) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    auto backend = std::make_unique<Scripted_backend>();
    Scripted_backend* backend_ptr = backend.get();

    term::Terminal_session_config config;
    config.transcript_recorder = recorder;
    config.scrollback_limit = 8;
    config.recover_scrollback_from_primary_repaints = false;

    term::Terminal_session session(std::move(backend), config);

    term::Terminal_launch_config launch_config = valid_launch_config();
    launch_config.initial_grid_size = term::terminal_grid_size_t{3, 8};
    ok &= check(session.start(launch_config).code == term::Terminal_session_result_code::ACCEPTED,
        "recovery flag replay captured session starts");
    backend_ptr->emit_output(QByteArrayLiteral("aaa\r\nbbb\r\nccc"));
    backend_ptr->emit_output(QByteArrayLiteral(
        "\x1b[?2026h"
        "\x1b[?25l"
        "\x1b[Hbbb\x1b[K\r\n"
        "ccc\x1b[K\r\n"
        "ddd\x1b[K"
        "\x1b[?25h"
        "\x1b[?2026l"));

    const std::optional<term::Terminal_render_snapshot> snapshot =
        session.latest_render_snapshot();
    ok &= check(snapshot.has_value() && snapshot->viewport.scrollback_rows == 0,
        "recorded disabled recovery does not synthesize scrollback");
    recorder.reset();

    const Replay_tool_process_result replay = run_replay_tool(replay_tool_path, path);
    ok &= check(replay.finished, "recovery flag replay tool finishes");
    ok &= check(replay.exit_status == QProcess::NormalExit && replay.exit_code == 0,
        "recovery flag replay tool exits successfully");
    ok &= check(replay.stdout_text.contains("divergent_snapshot_events=0"),
        "recovery flag replay has no divergence");
    if (!ok) {
        print_replay_tool_output(replay);
    }
    return ok;
}

bool test_replay_tool_compares_recovered_row_provenance_source(
    const QString& replay_tool_path)
{
    bool ok = true;
    ok &= check(!replay_tool_path.isEmpty(), "recovered-source replay tool path is passed");
    if (replay_tool_path.isEmpty()) {
        return false;
    }

    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "recovered-source replay directory is valid")) {
        return false;
    }

    const QString path = temp_dir.filePath(QStringLiteral("recovered-source-replay.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, true, &error);
    ok &= check(recorder != nullptr, "recovered-source replay recorder opens");
    if (recorder == nullptr) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    auto backend = std::make_unique<Scripted_backend>();
    Scripted_backend* backend_ptr = backend.get();

    term::Terminal_session_config config;
    config.transcript_recorder = recorder;
    config.scrollback_limit = 8;
    config.recover_scrollback_from_primary_repaints = true;

    term::Terminal_session session(std::move(backend), config);

    term::Terminal_launch_config launch_config = valid_launch_config();
    launch_config.initial_grid_size = term::terminal_grid_size_t{4, 8};
    ok &= check(session.start(launch_config).code == term::Terminal_session_result_code::ACCEPTED,
        "recovered-source replay captured session starts");
    backend_ptr->emit_output(visible_row_write_stream({
        QByteArrayLiteral("aa"),
        QByteArrayLiteral("bb"),
        QByteArrayLiteral("cc"),
        QByteArrayLiteral("dd"),
    }, false));
    backend_ptr->emit_output(visible_row_write_stream({
        QByteArrayLiteral("bb"),
        QByteArrayLiteral("cc"),
        QByteArrayLiteral("dd"),
        QByteArrayLiteral("ee"),
    }, true));

    const auto visible_viewport = [&]() {
        const std::optional<term::Terminal_render_snapshot> snapshot =
            session.latest_render_snapshot();
        return snapshot.has_value() ? snapshot->viewport : session.viewport_state();
    };
    const term::Terminal_viewport_state viewport_before = visible_viewport();
    ok &= check(
        recorder->record_surface_scroll_intent({
            QStringLiteral("api.offset"),
            1,
            1,
            viewport_before,
        }),
        "recovered-source replay records offset scroll intent");
    const term::Terminal_viewport_scroll_result scroll =
        session.scroll_published_viewport_to_offset_from_tail(1);
    ok &= check(scroll.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "recovered-source replay scrolls recovered row into published viewport");
    ok &= check(
        recorder->record_surface_scroll({
            QStringLiteral("api.offset"),
            1,
            1,
            scroll,
            viewport_before,
            visible_viewport(),
        }),
        "recovered-source replay records offset scroll result");

    const std::optional<term::Terminal_render_snapshot> snapshot =
        session.latest_render_snapshot();
    ok &= check(snapshot.has_value() &&
            snapshot->viewport.offset_from_tail == 1 &&
            !snapshot->visible_line_provenance.empty() &&
            snapshot->visible_line_provenance[0].source ==
                term::Terminal_retained_line_provenance_source::RECOVERED_PRIMARY_REPAINT,
        "recovered-source replay production snapshot carries recovered source");
    recorder.reset();

    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(path, &error);
    ok &= check(events.has_value(), "recovered-source transcript parses");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    bool recorded_recovered_source = false;
    for (const term::Terminal_transcript_event& event : *events) {
        if (event.kind != QStringLiteral("snapshot")) {
            continue;
        }

        const QJsonArray row_provenance =
            event.object.value(QStringLiteral("row_provenance")).toArray();
        for (const QJsonValue& value : row_provenance) {
            recorded_recovered_source =
                recorded_recovered_source ||
                value.toObject().value(QStringLiteral("source")).toString() ==
                    QStringLiteral("recovered_primary_repaint");
        }
    }
    ok &= check(recorded_recovered_source,
        "recovered-source transcript snapshot emits recovered row provenance source");

    Replay_tool_process_result replay = run_replay_tool(replay_tool_path, path);
    ok &= check(replay.finished, "recovered-source replay tool finishes");
    ok &= check(replay.exit_status == QProcess::NormalExit && replay.exit_code == 0,
        "recovered-source replay tool exits successfully");
    ok &= check(replay.stdout_text.contains("divergent_snapshot_events=0"),
        "recovered-source replay compares recovered source without divergence");
    ok &= check(replay.stdout_text.contains("source=recovered_primary_repaint"),
        "recovered-source replay tool prints recovered provenance source");

    ok &= check(
        rewrite_first_recovered_row_provenance_source(
            path,
            QStringLiteral("terminal_storage")),
        "recovered-source replay fixture rewrites recovered source");
    replay = run_replay_tool(replay_tool_path, path);
    ok &= check(replay.finished, "recovered-source mismatched replay tool finishes");
    ok &= check(replay.exit_status == QProcess::NormalExit && replay.exit_code != 0,
        "recovered-source mismatched replay tool reports divergence");
    ok &= check(replay.stdout_text.contains("divergent_snapshot_events=1"),
        "recovered-source mismatched replay compares provenance source");
    ok &= check(replay.stdout_text.contains("fields=row_provenance"),
        "recovered-source mismatched replay reports row_provenance field");
    if (!ok) {
        print_replay_tool_output(replay);
    }
    return ok;
}

bool test_replay_tool_defaults_missing_row_provenance_source(
    const QString& replay_tool_path)
{
    bool ok = true;
    ok &= check(!replay_tool_path.isEmpty(), "missing-source replay tool path is passed");
    if (replay_tool_path.isEmpty()) {
        return false;
    }

    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "missing-source replay directory is valid")) {
        return false;
    }

    const QString path = temp_dir.filePath(QStringLiteral("missing-source-replay.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, true, &error);
    ok &= check(recorder != nullptr, "missing-source replay recorder opens");
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
        "missing-source replay captured session starts");
    backend_ptr->emit_output(QByteArrayLiteral("alpha\r\nbravo\r\ncharlie"));
    recorder.reset();

    ok &= check(remove_snapshot_row_provenance_sources(path),
        "missing-source replay fixture removes row_provenance source fields");

    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(path, &error);
    ok &= check(events.has_value(), "missing-source replay transcript parses");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    const std::optional<term::Terminal_transcript_event> snapshot_event =
        first_event(*events, QStringLiteral("snapshot"));
    const QJsonArray row_provenance = snapshot_event.has_value()
        ? snapshot_event->object.value(QStringLiteral("row_provenance")).toArray()
        : QJsonArray{};
    ok &= check(snapshot_event.has_value() &&
            !row_provenance.isEmpty() &&
            row_provenance.at(0).toObject().value(QStringLiteral("source")).toString() ==
                QStringLiteral("terminal_storage"),
        "missing-source replay reader defaults row provenance before replay");

    const Replay_tool_process_result replay = run_replay_tool(replay_tool_path, path);
    ok &= check(replay.finished, "missing-source replay tool finishes");
    ok &= check(replay.exit_status == QProcess::NormalExit && replay.exit_code == 0,
        "missing-source replay tool exits successfully");
    ok &= check(replay.stdout_text.contains("divergent_snapshot_events=0"),
        "missing-source replay has no model divergence");
    ok &= check(replay.stdout_text.contains("source=terminal_storage"),
        "missing-source replay prints defaulted terminal-storage source");
    if (!ok) {
        print_replay_tool_output(replay);
    }
    return ok;
}

bool test_replay_tool_accepts_natural_public_projection_scroll_snapshot(
    const QString& replay_tool_path)
{
    bool ok = true;
    ok &= check(!replay_tool_path.isEmpty(), "natural public projection replay tool path is passed");
    if (replay_tool_path.isEmpty()) {
        return false;
    }

    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "natural public projection replay directory is valid")) {
        return false;
    }

    const QString path = temp_dir.filePath(QStringLiteral("natural-public-projection.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, true, &error);
    ok &= check(recorder != nullptr, "natural public projection recorder opens");
    if (recorder == nullptr) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    auto backend = std::make_unique<Scripted_backend>();
    Scripted_backend* backend_ptr = backend.get();

    term::Terminal_session_config config;
    config.transcript_recorder = recorder;
    config.scrollback_limit = 8;
    config.synchronized_output_scroll_policy =
        term::Terminal_synchronized_output_scroll_policy::IMMEDIATE_PUBLIC_PROJECTION;
    term::Terminal_session session(std::move(backend), config);

    term::Terminal_launch_config launch_config = valid_launch_config();
    launch_config.initial_grid_size = term::terminal_grid_size_t{3, 20};
    ok &= check(session.start(launch_config).code == term::Terminal_session_result_code::ACCEPTED,
        "natural public projection replay captured session starts");
    backend_ptr->emit_output(
        QByteArrayLiteral("row-0\r\nrow-1\r\nrow-2\r\nrow-3\r\nrow-4\r\nrow-5\r\nrow-6\r\nrow-7"));
    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hheld-output"));

    const auto visible_viewport = [&]() {
        const std::optional<term::Terminal_render_snapshot> snapshot =
            session.latest_render_snapshot();
        return snapshot.has_value() ? snapshot->viewport : session.viewport_state();
    };
    const auto record_scroll_result = [&](
        const QString&                               source,
        int                                          requested_line_delta,
        std::optional<int>                           requested_offset_from_tail,
        const term::Terminal_viewport_scroll_result& result,
        const term::Terminal_viewport_state&         viewport_before)
    {
        return recorder->record_surface_scroll({
            source,
            requested_line_delta,
            requested_offset_from_tail,
            result,
            viewport_before,
            visible_viewport(),
        });
    };
    const auto record_line_scroll = [&](const QString& source, int line_delta) {
        const term::Terminal_viewport_state viewport_before = visible_viewport();
        ok &= check(
            recorder->record_surface_scroll_intent({
                source,
                line_delta,
                std::nullopt,
                viewport_before,
            }),
            "natural public projection records line scroll intent");
        const term::Terminal_viewport_scroll_result result =
            source == QStringLiteral("surface.text_area.wheel")
                ? session.scroll_viewport_lines_from_published_state(
                    line_delta,
                    viewport_before)
                : session.scroll_published_viewport_lines(line_delta);
        ok &= check(
            record_scroll_result(source, line_delta, std::nullopt, result, viewport_before),
            "natural public projection records line scroll result");
        return result;
    };
    const auto record_offset_scroll = [&](const QString& source, int offset_from_tail) {
        const term::Terminal_viewport_state viewport_before = visible_viewport();
        const int requested_line_delta = offset_from_tail - viewport_before.offset_from_tail;
        ok &= check(
            recorder->record_surface_scroll_intent({
                source,
                requested_line_delta,
                offset_from_tail,
                viewport_before,
            }),
            "natural public projection records offset scroll intent");
        const term::Terminal_viewport_scroll_result result =
            session.scroll_published_viewport_to_offset_from_tail(offset_from_tail);
        ok &= check(
            record_scroll_result(
                source,
                requested_line_delta,
                offset_from_tail,
                result,
                viewport_before),
            "natural public projection records offset scroll result");
        return result;
    };

    ok &= check(
        record_line_scroll(QStringLiteral("api.lines"), 1).action ==
            term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "natural public projection records public scroll_viewport_lines move");
    ok &= check(
        record_offset_scroll(QStringLiteral("api.offset"), 3).action ==
            term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "natural public projection records public scroll_to_offset move");
    ok &= check(
        record_line_scroll(QStringLiteral("surface.text_area.wheel"), -1).action ==
            term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "natural public projection records text-area wheel move");
    ok &= check(
        record_line_scroll(QStringLiteral("key.page"), 1).action ==
            term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "natural public projection records page-key move");
    ok &= check(
        record_line_scroll(QStringLiteral("app.scrollbar.wheel"), 1).action ==
            term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "natural public projection records app scrollbar wheel move");
    ok &= check(
        record_offset_scroll(QStringLiteral("app.scrollbar.track"), 2).action ==
            term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "natural public projection records app scrollbar track move");
    ok &= check(
        record_line_scroll(QStringLiteral("app.scrollbar.page"), 1).action ==
            term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "natural public projection records app scrollbar page move");
    ok &= check(
        record_offset_scroll(QStringLiteral("app.scrollbar.thumb"), 0).action ==
            term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "natural public projection records app scrollbar thumb move");

    ok &= check(
        session.resize(QSizeF(640.0, 160.0), term::terminal_grid_size_t{4, 20}).code ==
            term::Terminal_session_result_code::ACCEPTED,
        "natural public projection resize invalidates projection");
    ok &= check(
        record_offset_scroll(QStringLiteral("app.scrollbar.thumb"), 1).action ==
            term::Terminal_viewport_scroll_action::DEFERRED_INTENT_RECORDED,
        "natural public projection records invalidated deferred app scrollbar intent");
    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026l"));
    recorder.reset();

    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(path, &error);
    ok &= check(events.has_value(), "natural public projection transcript parses");
    if (events.has_value()) {
        bool has_deferred_scroll = false;
        bool has_invalidated_release = false;
        for (const term::Terminal_transcript_event& event : *events) {
            if (event.kind == QStringLiteral("surface.scroll") &&
                event.object.value(QStringLiteral("action")).toString() ==
                    QStringLiteral("deferred_intent_recorded"))
            {
                has_deferred_scroll = true;
            }
            if (event.kind == QStringLiteral("snapshot") &&
                event.object.value(QStringLiteral("diagnostic_reason")).toString() ==
                    QStringLiteral("public_projection_invalidated_deferred_intent") &&
                event.object.value(QStringLiteral("release_reconciliation_result")).toString() ==
                    QStringLiteral("deferred_offset"))
            {
                has_invalidated_release = true;
            }
        }
        ok &= check(has_deferred_scroll,
            "natural public projection transcript includes deferred scroll result");
        ok &= check(has_invalidated_release,
            "natural public projection transcript includes invalidated deferred release");

        QStringList expected_sources = {
            QStringLiteral("api.lines"),
            QStringLiteral("api.offset"),
            QStringLiteral("key.page"),
            QStringLiteral("surface.text_area.wheel"),
            QStringLiteral("app.scrollbar.wheel"),
            QStringLiteral("app.scrollbar.track"),
            QStringLiteral("app.scrollbar.page"),
            QStringLiteral("app.scrollbar.thumb"),
        };
        expected_sources.sort();
        ok &= check(
            transcript_event_sources(*events, QStringLiteral("surface.scroll_intent")) ==
                expected_sources,
            "natural public projection records expected scroll intent source set");
        ok &= check(
            transcript_event_sources(*events, QStringLiteral("surface.scroll")) ==
                expected_sources,
            "natural public projection records expected scroll source set");
    }

    const Replay_tool_process_result replay = run_replay_tool(replay_tool_path, path);
    ok &= check(replay.finished, "natural public projection replay tool finishes");
    ok &= check(replay.exit_status == QProcess::NormalExit && replay.exit_code == 0,
        "natural public projection replay tool exits successfully");
    const std::optional<int> public_projection_scroll_snapshot_events =
        replay_stdout_metric(
            replay.stdout_text,
            QByteArrayLiteral("public_projection_scroll_snapshot_events"));
    ok &= check(public_projection_scroll_snapshot_events.has_value() &&
        *public_projection_scroll_snapshot_events == 8,
        "natural public projection replay accepts eight public scroll snapshots");
    ok &= check(replay.stdout_text.contains("divergent_snapshot_events=0"),
        "natural public projection replay has no divergence");
    if (!ok) {
        print_replay_tool_output(replay);
    }
    return ok;
}

bool test_replay_tool_rejects_unmatched_public_projection_scroll_snapshot_before_release(
    const QString& replay_tool_path)
{
    bool ok = true;
    ok &= check(!replay_tool_path.isEmpty(), "public projection scroll replay tool path is passed");
    if (replay_tool_path.isEmpty()) {
        return false;
    }

    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary public projection scroll replay directory is valid")) {
        return false;
    }

    const QString path = temp_dir.filePath(QStringLiteral("public-projection-scroll.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, true, &error);
    ok &= check(recorder != nullptr, "public projection scroll replay transcript recorder opens");
    if (recorder == nullptr) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    auto backend = std::make_unique<Scripted_backend>();
    Scripted_backend* backend_ptr = backend.get();

    term::Terminal_session_config config;
    config.transcript_recorder = recorder;
    config.scrollback_limit = 2;
    config.synchronized_output_scroll_policy =
        term::Terminal_synchronized_output_scroll_policy::IMMEDIATE_PUBLIC_PROJECTION;
    term::Terminal_session session(std::move(backend), config);

    term::Terminal_launch_config launch_config = valid_launch_config();
    launch_config.initial_grid_size = term::terminal_grid_size_t{3, 20};
    ok &= check(session.start(launch_config).code == term::Terminal_session_result_code::ACCEPTED,
        "public projection scroll replay captured session starts");
    backend_ptr->emit_output(QByteArrayLiteral("row-0\r\nrow-1\r\nrow-2\r\nrow-3"));

    const term::Terminal_viewport_state detach_before = session.viewport_state();
    ok &= check(
        recorder->record_surface_scroll_intent({
            QStringLiteral("scrollbar"),
            0,
            1,
            detach_before,
        }),
        "public projection scroll replay records pre-hold detach intent");
    const term::Terminal_viewport_scroll_result detach_result =
        session.scroll_published_viewport_to_offset_from_tail(1);
    ok &= check(detach_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "public projection scroll replay detaches viewport before hold");

    const std::optional<term::Terminal_render_snapshot> safe_content =
        session.latest_content_render_snapshot_for_testing();
    ok &= check(safe_content.has_value(),
        "public projection scroll replay fixture has safe content basis");
    if (!safe_content.has_value()) {
        return ok;
    }

    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hhidden-live"));
    const std::optional<term::Terminal_public_projection> captured_projection =
        session.public_projection_for_testing();
    ok &= check(captured_projection.has_value(),
        "public projection scroll replay fixture captures projection");
    if (!captured_projection.has_value()) {
        return ok;
    }

    // This installed projection seam drives replay rejection of a public scroll
    // snapshot that natural production capture cannot reproduce.
    std::vector<term::Terminal_public_projection_row> copied_rows =
        captured_projection->rows();
    bool copied_tail_installed = false;
    for (term::Terminal_public_projection_row& row : copied_rows) {
        if (row.public_row == 3) {
            row = public_projection_row_from_text(3, QStringLiteral("copied-tail"));
            copied_tail_installed = true;
            break;
        }
    }
    if (!copied_tail_installed) {
        copied_rows.push_back(
            public_projection_row_from_text(3, QStringLiteral("copied-tail")));
    }
    session.install_public_projection_for_testing(
        term::Terminal_public_projection::with_copied_rows_for_testing(
            *captured_projection,
            captured_projection->first_copied_public_row(),
            std::move(copied_rows)));

    ok &= check(
        recorder->record_surface_scroll_intent({
            QStringLiteral("surface.text_area.wheel"),
            -1,
            std::nullopt,
            safe_content->viewport,
        }),
        "public projection scroll replay records public scroll intent");
    const std::optional<term::Terminal_render_snapshot> content_basis_before_public_scroll =
        session.latest_content_render_snapshot_for_testing();
    const term::Terminal_viewport_scroll_result public_scroll_result =
        session.scroll_viewport_lines_from_published_state(-1, safe_content->viewport);
    const std::optional<term::Terminal_render_snapshot> public_scroll =
        session.latest_render_snapshot();
    const std::optional<term::Terminal_render_snapshot> content_basis_after_public_scroll =
        session.latest_content_render_snapshot_for_testing();
    ok &= check(public_scroll_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "public projection scroll replay fixture applies real public scroll");
    ok &= check(public_scroll.has_value() &&
        public_scroll->basis == term::Terminal_render_snapshot_basis::PUBLIC_PROJECTION &&
        public_scroll->purpose == term::Terminal_render_snapshot_purpose::SCROLL,
        "public projection scroll replay fixture records a session-produced public scroll snapshot");
    ok &= check(content_basis_before_public_scroll.has_value() &&
        content_basis_after_public_scroll.has_value() &&
        content_basis_after_public_scroll->metadata.sequence ==
            content_basis_before_public_scroll->metadata.sequence &&
        content_basis_after_public_scroll->basis ==
            term::Terminal_render_snapshot_basis::LIVE_CONTENT &&
        content_basis_after_public_scroll->purpose ==
            term::Terminal_render_snapshot_purpose::CONTENT,
        "recorded public projection scroll does not advance the safe live-content basis");

    recorder.reset();

    const Replay_tool_process_result replay = run_replay_tool(replay_tool_path, path);
    ok &= check(replay.finished, "public projection scroll replay tool finishes");
    ok &= check(replay.exit_status == QProcess::NormalExit && replay.exit_code != 0,
        "public projection scroll replay tool rejects unmatched installed public snapshot");
    ok &= check(replay.stdout_text.contains("recorded_snapshot_events=3"),
        "public projection scroll replay sees pre-release public and live snapshots");
    const std::optional<int> replay_public_scroll_snapshot_events =
        replay_stdout_metric(
            replay.stdout_text,
            QByteArrayLiteral("public_projection_scroll_snapshot_events"));
    ok &= check(replay_public_scroll_snapshot_events.has_value() &&
        *replay_public_scroll_snapshot_events == 0,
        "public projection scroll replay does not self-accept unmatched public scroll snapshots");
    ok &= check(replay.stdout_text.contains("divergent_snapshot_events=1"),
        "public projection scroll replay reports unmatched public snapshot divergence");
    ok &= check(replay.stdout_text.contains(
            "snapshot.basis=PUBLIC_PROJECTION snapshot.purpose=SCROLL"),
        "public projection scroll replay reports the unmatched public scroll snapshot");

    ok &= check(corrupt_first_public_projection_scroll_snapshot_to_self_consistent_wrong_content(path),
        "public projection scroll replay corruption fixture rewrites recorded public snapshot");
    const Replay_tool_process_result corrupted_replay = run_replay_tool(replay_tool_path, path);
    ok &= check(corrupted_replay.finished,
        "corrupted public projection scroll replay tool finishes");
    ok &= check(
        corrupted_replay.exit_status == QProcess::NormalExit && corrupted_replay.exit_code != 0,
        "corrupted public projection scroll replay tool reports divergence");
    const std::optional<int> corrupted_public_scroll_snapshot_events =
        replay_stdout_metric(
            corrupted_replay.stdout_text,
            QByteArrayLiteral("public_projection_scroll_snapshot_events"));
    ok &= check(corrupted_public_scroll_snapshot_events.has_value() &&
        *corrupted_public_scroll_snapshot_events == 0,
        "corrupted public projection scroll replay does not self-accept wrong visible-row content");
    ok &= check(corrupted_replay.stdout_text.contains("divergent_snapshot_events=1"),
        "corrupted public projection scroll replay counts self-consistent wrong-content divergence");
    if (!ok) {
        print_replay_tool_output(replay);
        print_replay_tool_output(corrupted_replay);
    }
    return ok;
}

bool test_replay_tool_compares_every_snapshot_in_contiguous_run(
    const QString& replay_tool_path)
{
    bool ok = true;
    ok &= check(!replay_tool_path.isEmpty(), "strict snapshot-run replay tool path is passed");
    if (replay_tool_path.isEmpty()) {
        return false;
    }

    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary strict snapshot-run replay directory is valid")) {
        return false;
    }

    const QString path = temp_dir.filePath(QStringLiteral("strict-snapshot-run.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, true, &error);
    ok &= check(recorder != nullptr, "strict snapshot-run replay transcript recorder opens");
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
        "strict snapshot-run replay captured session starts");
    backend_ptr->emit_output(QByteArrayLiteral("before"));
    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hheld"));
    backend_ptr->emit_output(QByteArrayLiteral("final\x1b[?2026l"));
    recorder.reset();

    const int inserted_snapshot_events =
        insert_final_snapshot_copy_before_last_snapshot(path);
    ok &= check(inserted_snapshot_events > 0,
        "strict snapshot-run replay fixture inserts a duplicate final snapshot");

    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(path, &error);
    ok &= check(events.has_value(), "strict snapshot-run rewritten transcript parses");
    const int recorded_snapshot_count =
        events.has_value() ? event_count(*events, QStringLiteral("snapshot")) : 0;
    ok &= check(recorded_snapshot_count > inserted_snapshot_events,
        "strict snapshot-run fixture leaves final snapshot unchanged");
    const int expected_matching_snapshot_events =
        recorded_snapshot_count - inserted_snapshot_events;

    const Replay_tool_process_result replay = run_replay_tool(replay_tool_path, path);
    ok &= check(replay.finished, "strict snapshot-run replay tool finishes");
    ok &= check(replay.exit_status == QProcess::NormalExit && replay.exit_code != 0,
        "strict snapshot-run replay tool rejects final-only snapshot masking");
    const std::optional<int> recorded_snapshot_events =
        replay_stdout_metric(replay.stdout_text, QByteArrayLiteral("recorded_snapshot_events"));
    ok &= check(recorded_snapshot_events.has_value() &&
            *recorded_snapshot_events == recorded_snapshot_count,
        "strict snapshot-run replay sees every recorded snapshot");
    const std::optional<int> replayed_snapshot_events =
        replay_stdout_metric(replay.stdout_text, QByteArrayLiteral("replayed_snapshot_events"));
    ok &= check(replayed_snapshot_events.has_value() &&
            *replayed_snapshot_events == expected_matching_snapshot_events,
        "strict snapshot-run replay reports replayed snapshot count");
    const std::optional<int> matching_snapshot_events =
        replay_stdout_metric(replay.stdout_text, QByteArrayLiteral("matching_snapshot_events"));
    ok &= check(matching_snapshot_events.has_value() &&
            *matching_snapshot_events == expected_matching_snapshot_events,
        "strict snapshot-run replay still compares unchanged snapshots");
    const std::optional<int> divergent_snapshot_events =
        replay_stdout_metric(replay.stdout_text, QByteArrayLiteral("divergent_snapshot_events"));
    ok &= check(divergent_snapshot_events.has_value() &&
            *divergent_snapshot_events == inserted_snapshot_events,
        "strict snapshot-run replay rejects final-only snapshot masking");

    const Replay_tool_process_result default_replay =
        run_replay_tool_with_arguments(replay_tool_path, {path});
    ok &= check(default_replay.finished, "default snapshot-run replay tool finishes");
    ok &= check(
        default_replay.exit_status == QProcess::NormalExit && default_replay.exit_code != 0,
        "default snapshot-run replay is strict all-snapshot replay");
    const std::optional<int> default_divergent_snapshot_events =
        replay_stdout_metric(
            default_replay.stdout_text,
            QByteArrayLiteral("divergent_snapshot_events"));
    ok &= check(default_divergent_snapshot_events.has_value() &&
            *default_divergent_snapshot_events == inserted_snapshot_events,
        "default snapshot-run replay compares every snapshot");
    if (!ok) {
        print_replay_tool_output(replay);
        print_replay_tool_output(default_replay);
    }
    return ok;
}

bool test_replay_tool_rejects_surplus_replayed_snapshots(const QString& replay_tool_path)
{
    bool ok = true;
    ok &= check(!replay_tool_path.isEmpty(), "surplus replayed snapshot tool path is passed");
    if (replay_tool_path.isEmpty()) {
        return false;
    }

    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary surplus replayed snapshot directory is valid")) {
        return false;
    }

    const QString path = temp_dir.filePath(QStringLiteral("surplus-replayed-snapshot.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, true, &error);
    ok &= check(recorder != nullptr, "surplus replayed snapshot recorder opens");
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
        "surplus replayed snapshot captured session starts");
    backend_ptr->emit_output(QByteArrayLiteral("before"));
    backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026hheld"));
    backend_ptr->emit_output(QByteArrayLiteral("final\x1b[?2026l"));
    recorder.reset();

    const std::optional<std::vector<term::Terminal_transcript_event>> original_events =
        term::read_terminal_transcript(path, &error);
    ok &= check(original_events.has_value(), "surplus replayed original transcript parses");
    const int original_snapshot_count =
        original_events.has_value() ? event_count(*original_events, QStringLiteral("snapshot")) : 0;
    ok &= check(original_snapshot_count > 1,
        "surplus replayed fixture records multiple snapshots");

    const int removed_snapshot_events = remove_last_snapshot_event(path);
    ok &= check(removed_snapshot_events == 1,
        "surplus replayed fixture removes the final recorded snapshot");

    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(path, &error);
    ok &= check(events.has_value(), "surplus replayed rewritten transcript parses");
    const int recorded_snapshot_count =
        events.has_value() ? event_count(*events, QStringLiteral("snapshot")) : 0;
    ok &= check(recorded_snapshot_count == original_snapshot_count - removed_snapshot_events,
        "surplus replayed fixture leaves a recorded snapshot prefix");

    const Replay_tool_process_result replay = run_replay_tool(replay_tool_path, path);
    ok &= check(replay.finished, "surplus replayed snapshot tool finishes");
    ok &= check(replay.exit_status == QProcess::NormalExit && replay.exit_code != 0,
        "surplus replayed snapshot tool rejects snapshot count mismatch");
    const std::optional<int> recorded_snapshot_events =
        replay_stdout_metric(replay.stdout_text, QByteArrayLiteral("recorded_snapshot_events"));
    ok &= check(recorded_snapshot_events.has_value() &&
            *recorded_snapshot_events == recorded_snapshot_count,
        "surplus replayed snapshot tool reports recorded count");
    const std::optional<int> replayed_snapshot_events =
        replay_stdout_metric(replay.stdout_text, QByteArrayLiteral("replayed_snapshot_events"));
    ok &= check(replayed_snapshot_events.has_value() &&
            *replayed_snapshot_events == original_snapshot_count,
        "surplus replayed snapshot tool reports replayed count");
    const std::optional<int> matching_snapshot_events =
        replay_stdout_metric(replay.stdout_text, QByteArrayLiteral("matching_snapshot_events"));
    ok &= check(matching_snapshot_events.has_value() &&
            *matching_snapshot_events == recorded_snapshot_count,
        "surplus replayed snapshot tool matches the recorded prefix");
    const std::optional<int> divergent_snapshot_events =
        replay_stdout_metric(replay.stdout_text, QByteArrayLiteral("divergent_snapshot_events"));
    ok &= check(divergent_snapshot_events.has_value() &&
            *divergent_snapshot_events == removed_snapshot_events,
        "surplus replayed snapshot tool reports the surplus snapshot divergence");
    ok &= check(replay.stdout_text.contains("fields=surplus_replayed_snapshot"),
        "surplus replayed snapshot tool identifies the first surplus replayed snapshot");
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
    ok &= check(replay.stdout_text.contains("divergent_snapshot_events=0"),
        "replay tool replays new snapshot schema fields without divergence");
    ok &= check(replay.stdout_text.contains("snapshot.basis=LIVE_CONTENT snapshot.purpose=SELECTION_DERIVED"),
        "replay tool prints live-content selection snapshot basis and purpose");
    ok &= check(
        replay.stdout_text.contains("selection_replay=semantic_range_from_surface_selection_drag"),
        "replay tool labels semantic selection replay");
    if (!ok) {
        print_replay_tool_output(replay);
    }
    return ok;
}

bool test_replay_tool_compares_invalid_range_selected_text_result(
    const QString& replay_tool_path)
{
    bool ok = true;
    ok &= check(!replay_tool_path.isEmpty(), "replay tool path is passed to invalid-range test");
    if (replay_tool_path.isEmpty()) {
        return false;
    }

    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary invalid-range selected-text directory is valid")) {
        return false;
    }

    const QString path =
        temp_dir.filePath(QStringLiteral("invalid-range-selected-text.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, true, &error);
    ok &= check(recorder != nullptr, "invalid-range selected-text recorder opens");
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
    launch_config.initial_grid_size = term::terminal_grid_size_t{2, 8};
    ok &= check(session.start(launch_config).code == term::Terminal_session_result_code::ACCEPTED,
        "invalid-range replay captured session starts");
    backend_ptr->emit_output(QByteArrayLiteral("seed"));

    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 2;
    viewport.follow_tail  = true;
    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({2, 8}, viewport, 31U);
    snapshot.cursor.visible = false;
    snapshot.dirty_row_ranges.push_back({0, 2});
    snapshot.visible_line_provenance = {
        {0, 1U, 1U},
        {1, 2U, 1U},
    };

    term::Terminal_selection_range invalid_range;
    invalid_range.start = {3, 0};
    invalid_range.end   = {3, 2};
    invalid_range.mode  = term::Terminal_selection_mode::NORMAL;
    snapshot.selection_spans.push_back({
        invalid_range,
        0,
        0,
        2,
    });

    const term::Terminal_selection_result selected_text =
        term::selected_text_from_render_snapshot(snapshot, invalid_range);
    ok &= check(!snapshot.selection_spans.empty() &&
            selected_text.code == term::Terminal_selection_result_code::INVALID_RANGE,
        "invalid-range selected-text fixture has visible spans with an invalid source range");
    ok &= check(term::validate_render_snapshot(snapshot).status ==
            term::Terminal_render_snapshot_status::OK,
        "invalid-range selected-text fixture remains structurally valid");

    ok &= check(recorder->record_snapshot(
            1U,
            QStringLiteral("invalid range selected text"),
            snapshot),
        "invalid-range selected-text snapshot records");
    recorder.reset();

    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(path, &error);
    ok &= check(events.has_value(), "invalid-range selected-text transcript parses");
    if (!events.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    const std::optional<term::Terminal_transcript_event> snapshot_event =
        last_event(*events, QStringLiteral("snapshot"));
    ok &= check(snapshot_event.has_value(), "invalid-range selected-text snapshot event exists");
    if (!snapshot_event.has_value()) {
        return ok;
    }

    const QJsonObject recorded_selected_text =
        snapshot_event->object.value(QStringLiteral("selected_text")).toObject();
    ok &= check(
        snapshot_event->object.value(QStringLiteral("selection_span_count")).toInt() > 0 &&
            !snapshot_event->object.value(QStringLiteral("selection_spans")).toArray().isEmpty(),
        "invalid-range selected-text recording keeps the non-empty selection spans");
    ok &= check(
        recorded_selected_text.value(QStringLiteral("result")).toString() ==
            QStringLiteral("invalid_range"),
        "recorder selected_text uses the named invalid_range result");
    ok &= check(
        !recorded_selected_text.value(QStringLiteral("available")).toBool(),
        "invalid-range selected_text is marked unavailable");
    ok &= check(
        recorded_selected_text.value(QStringLiteral("source_range")).isObject(),
        "invalid-range selected_text keeps the source range");

    const Replay_tool_process_result replay = run_replay_tool(replay_tool_path, path);
    ok &= check(replay.finished, "invalid-range replay tool finishes");
    ok &= check(replay.exit_status == QProcess::NormalExit && replay.exit_code != 0,
        "invalid-range replay tool reports the deliberately unmatched snapshot");
    ok &= check(replay.stdout_text.contains("divergent_snapshot_events=1"),
        "invalid-range replay tool compares the production snapshot diagnostics");
    ok &= check(replay.stdout_text.contains(
            "recorded_selected_text.result=invalid_range"),
        "invalid-range replay tool reports the named recorded selected_text result");
    ok &= check(!replay.stdout_text.contains("recorded_selected_text.result=2"),
        "invalid-range replay tool does not report a numeric selected_text result");
    if (!ok) {
        print_replay_tool_output(replay);
    }

    return ok;
}

bool test_replay_tool_prints_named_non_ok_selected_text_result(
    const QString& replay_tool_path)
{
    bool ok = true;
    ok &= check(!replay_tool_path.isEmpty(), "replay tool path is passed to no-selection test");
    if (replay_tool_path.isEmpty()) {
        return false;
    }

    QTemporaryDir temp_dir;
    if (!check(temp_dir.isValid(), "temporary no-selection replay directory is valid")) {
        return false;
    }

    const QString path = temp_dir.filePath(QStringLiteral("tool-no-selection.ndjson"));
    QString error;
    std::shared_ptr<term::Terminal_transcript_recorder> recorder =
        term::Terminal_transcript_recorder::create(path, true, &error);
    ok &= check(recorder != nullptr, "no-selection replay transcript recorder opens");
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
        "no-selection replay captured session starts");
    backend_ptr->emit_output(QByteArrayLiteral("alpha"));
    recorder.reset();

    const Replay_tool_process_result replay = run_replay_tool(replay_tool_path, path);
    ok &= check(replay.finished, "no-selection replay tool finishes");
    ok &= check(replay.exit_status == QProcess::NormalExit && replay.exit_code == 0,
        "no-selection replay tool exits successfully");
    ok &= check(replay.stdout_text.contains("selected_text.result=no_selection"),
        "replay tool prints named no-selection result");
    ok &= check(!replay.stdout_text.contains("selected_text.result=1"),
        "replay tool does not print numeric no-selection result");
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
    ok &= test_writer_records_recovery_flag_values();
    ok &= test_snapshot_diagnostics_flag_controls_snapshot_event();
    ok &= test_snapshot_timing_diagnostics_include_snapshot_metadata();
    ok &= test_reader_rejects_malformed_transcripts();
    ok &= test_reader_rejects_invalid_leading_event_fields();
    ok &= test_reader_accepts_session_config_without_recovery_flag();
    ok &= test_reader_defaults_missing_row_provenance_source();
    ok &= test_reader_rejects_invalid_row_provenance_source();
    ok &= test_reader_rejects_compact_snapshots_missing_required_diagnostics();
    ok &= test_reader_rejects_invalid_snapshot_public_scroll_enum_values();
    ok &= test_reader_rejects_snapshot_basis_purpose_mismatches();
    ok &= test_reader_accepts_selection_projection_diagnostic_reason();
    ok &= test_reader_defaults_detached_selection_legacy_reason_to_selection_purpose();
    ok &= test_reader_defaults_old_wheel_trace_policy_fields();
    ok &= test_transcript_orders_host_and_resize_requests_before_synchronous_output();
    ok &= test_text_area_resize_requests_record_from_session();
    ok &= test_terminal_reply_replay_does_not_duplicate_host_write();
    ok &= test_scroll_intent_orders_before_snapshot_diagnostics();
    ok &= test_replay_noop_scroll_intent_does_not_consume_next_scroll();
    ok &= test_replay_drives_terminal_state();
    ok &= test_replay_config_keeps_explicit_deferred_public_scroll_policy();
    ok &= test_replay_config_defaults_missing_selection_viewport_projection_to_disabled();
    ok &= test_replay_config_preserves_recorded_recovery_flag();
    ok &= test_replay_config_defaults_inferred_recovery_to_disabled();
    const QString replay_tool_path =
        argc >= 2 ? QString::fromLocal8Bit(argv[1]) : QString();
    ok &= test_replay_tool_preserves_recorded_disabled_recovery_flag(replay_tool_path);
    ok &= test_replay_tool_compares_recovered_row_provenance_source(replay_tool_path);
    ok &= test_replay_tool_defaults_missing_row_provenance_source(replay_tool_path);
    ok &= test_replay_tool_accepts_natural_public_projection_scroll_snapshot(replay_tool_path);
    ok &= test_replay_tool_rejects_unmatched_public_projection_scroll_snapshot_before_release(
        replay_tool_path);
    ok &= test_replay_tool_compares_every_snapshot_in_contiguous_run(replay_tool_path);
    ok &= test_replay_tool_rejects_surplus_replayed_snapshots(replay_tool_path);
    ok &= test_replay_tool_prints_diagnostics(replay_tool_path);
    ok &= test_replay_tool_compares_invalid_range_selected_text_result(replay_tool_path);
    ok &= test_replay_tool_prints_named_non_ok_selected_text_result(replay_tool_path);
    return ok ? 0 : 1;
}
