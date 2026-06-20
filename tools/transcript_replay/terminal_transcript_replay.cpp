#include "vnm_terminal/internal/terminal_session.h"
#include "vnm_terminal/internal/terminal_transcript.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <vector>
#include <cstdint>

namespace term = vnm_terminal::internal;

namespace {

constexpr int k_surface_default_scrollback_limit = 10000;

QString usage_text()
{
    return QStringLiteral("usage: vnm_terminal_transcript_replay <transcript.ndjson>\n");
}

QString buffer_name(term::Terminal_buffer_id buffer)
{
    switch (buffer) {
        case term::Terminal_buffer_id::PRIMARY:
            return QStringLiteral("primary");
        case term::Terminal_buffer_id::ALTERNATE:
            return QStringLiteral("alternate");
    }

    return QStringLiteral("unknown");
}

term::Terminal_buffer_id buffer_from_name(const QString& buffer)
{
    return buffer == QStringLiteral("alternate")
        ? term::Terminal_buffer_id::ALTERNATE
        : term::Terminal_buffer_id::PRIMARY;
}

QString selection_mode_name(term::Terminal_selection_mode mode)
{
    switch (mode) {
        case term::Terminal_selection_mode::NONE:
            return QStringLiteral("none");
        case term::Terminal_selection_mode::NORMAL:
            return QStringLiteral("normal");
        case term::Terminal_selection_mode::WORD:
            return QStringLiteral("word");
        case term::Terminal_selection_mode::LINE:
            return QStringLiteral("line");
    }

    return QStringLiteral("unknown");
}

QString alternate_policy_name(term::Terminal_alternate_screen_scroll_policy policy)
{
    switch (policy) {
        case term::Terminal_alternate_screen_scroll_policy::KEEP_AT_TAIL:
            return QStringLiteral("keep_at_tail");
        case term::Terminal_alternate_screen_scroll_policy::WHEEL_TO_TERMINAL_INPUT:
            return QStringLiteral("wheel_to_terminal_input");
    }

    return QStringLiteral("unknown");
}

term::Terminal_alternate_screen_scroll_policy alternate_policy_from_name(
    const QString& policy)
{
    return policy == QStringLiteral("wheel_to_terminal_input")
        ? term::Terminal_alternate_screen_scroll_policy::WHEEL_TO_TERMINAL_INPUT
        : term::Terminal_alternate_screen_scroll_policy::KEEP_AT_TAIL;
}

QString escaped_text(QString text)
{
    text.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    text.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    text.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    text.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
    text.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return text;
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
    if (mode == QStringLiteral("word")) {
        return term::Terminal_selection_mode::WORD;
    }
    if (mode == QStringLiteral("line")) {
        return term::Terminal_selection_mode::LINE;
    }
    if (mode == QStringLiteral("none")) {
        return term::Terminal_selection_mode::NONE;
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

QString snapshot_row_text(const term::Terminal_render_snapshot& snapshot, int row)
{
    const term::Terminal_render_snapshot_row_content_view rows(snapshot);
    return term::selected_text_from_render_snapshot_row(
        rows.row_at(row),
        0,
        snapshot.grid_size.columns,
        true);
}

QJsonObject grid_size_object(term::terminal_grid_size_t grid_size)
{
    return {
        {QStringLiteral("rows"),    grid_size.rows},
        {QStringLiteral("columns"), grid_size.columns},
    };
}

QJsonObject position_object(term::terminal_grid_position_t position)
{
    return {
        {QStringLiteral("row"),    position.row},
        {QStringLiteral("column"), position.column},
    };
}

QJsonObject selection_range_object(const term::Terminal_selection_range& range)
{
    return {
        {QStringLiteral("start"), position_object(range.start)},
        {QStringLiteral("end"),   position_object(range.end)},
        {QStringLiteral("mode"),  selection_mode_name(range.mode)},
    };
}

QJsonObject viewport_object(const term::Terminal_viewport_state& viewport)
{
    return {
        {QStringLiteral("active_buffer"),    buffer_name(viewport.active_buffer)},
        {QStringLiteral("scrollback_rows"),  viewport.scrollback_rows},
        {QStringLiteral("visible_rows"),     viewport.visible_rows},
        {QStringLiteral("offset_from_tail"), viewport.offset_from_tail},
        {QStringLiteral("follow_tail"),      viewport.follow_tail},
        {QStringLiteral("alternate_screen_scroll_policy"),
            alternate_policy_name(viewport.alternate_screen_scroll_policy)},
    };
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

term::Terminal_viewport_state valid_transcript_viewport_or_fallback(
    term::Terminal_viewport_state viewport,
    term::Terminal_viewport_state fallback)
{
    const term::Terminal_viewport_state default_viewport;
    return
        viewport.active_buffer == default_viewport.active_buffer &&
        viewport.scrollback_rows == default_viewport.scrollback_rows &&
        viewport.visible_rows == default_viewport.visible_rows &&
        viewport.offset_from_tail == default_viewport.offset_from_tail &&
        viewport.follow_tail == default_viewport.follow_tail &&
        viewport.alternate_screen_scroll_policy ==
            default_viewport.alternate_screen_scroll_policy
            ? fallback
            : viewport;
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

QJsonArray visible_rows_array(const term::Terminal_render_snapshot& snapshot)
{
    QJsonArray rows;
    for (int row = 0; row < snapshot.grid_size.rows; ++row) {
        const QString text = snapshot_row_text(snapshot, row);
        QJsonObject object;
        object.insert(QStringLiteral("row"), row);
        object.insert(QStringLiteral("text"), text);
        object.insert(QStringLiteral("hash64"), text_hash64(text));
        rows.append(object);
    }
    return rows;
}

QJsonArray row_provenance_array(const term::Terminal_render_snapshot& snapshot)
{
    QJsonArray rows;
    const int row_count = std::min<int>(
        snapshot.grid_size.rows,
        static_cast<int>(snapshot.visible_line_provenance.size()));
    for (int row = 0; row < row_count; ++row) {
        const term::Terminal_render_line_provenance& provenance =
            snapshot.visible_line_provenance[static_cast<std::size_t>(row)];
        QJsonObject object;
        object.insert(QStringLiteral("row"), row);
        object.insert(QStringLiteral("logical_row"), static_cast<qint64>(provenance.logical_row));
        object.insert(
            QStringLiteral("retained_line_id"),
            QString::number(static_cast<qulonglong>(provenance.retained_line_id)));
        object.insert(
            QStringLiteral("content_generation"),
            QString::number(static_cast<qulonglong>(provenance.content_generation)));
        object.insert(
            QStringLiteral("source"),
            term::retained_line_provenance_source_name(provenance.source));
        rows.append(object);
    }
    return rows;
}

QJsonArray dirty_row_ranges_array(const term::Terminal_render_snapshot& snapshot)
{
    QJsonArray ranges;
    for (const term::Terminal_render_dirty_row_range& range : snapshot.dirty_row_ranges) {
        ranges.append(QJsonObject{
            {QStringLiteral("first_row"), range.first_row},
            {QStringLiteral("row_count"), range.row_count},
        });
    }
    return ranges;
}

QJsonArray selection_spans_array(const term::Terminal_render_snapshot& snapshot)
{
    QJsonArray spans;
    for (const term::Terminal_render_selection_span& span : snapshot.selection_spans) {
        spans.append(QJsonObject{
            {QStringLiteral("row"),          span.row},
            {QStringLiteral("first_column"), span.first_column},
            {QStringLiteral("column_count"), span.column_count},
            {QStringLiteral("source_range"), selection_range_object(span.source_range)},
        });
    }
    return spans;
}

QString selection_result_code_name(term::Terminal_selection_result_code code)
{
    switch (code) {
        case term::Terminal_selection_result_code::OK:
            return QStringLiteral("ok");
        case term::Terminal_selection_result_code::NO_SELECTION:
            return QStringLiteral("no_selection");
        case term::Terminal_selection_result_code::INVALID_RANGE:
            return QStringLiteral("invalid_range");
    }

    return QStringLiteral("unknown");
}

QJsonObject selected_text_object(const term::Terminal_render_snapshot& snapshot)
{
    QJsonObject object;
    if (snapshot.selection_spans.empty()) {
        object.insert(QStringLiteral("available"), false);
        object.insert(QStringLiteral("result"), QStringLiteral("no_selection"));
        return object;
    }

    const term::Terminal_selection_range& range = snapshot.selection_spans.front().source_range;
    const term::Terminal_selection_result result =
        term::selected_text_from_render_snapshot(snapshot, range);
    object.insert(QStringLiteral("available"), result.code == term::Terminal_selection_result_code::OK);
    object.insert(QStringLiteral("result"), selection_result_code_name(result.code));
    object.insert(QStringLiteral("source_range"), selection_range_object(range));
    if (result.code == term::Terminal_selection_result_code::OK) {
        object.insert(QStringLiteral("text"), result.text);
        object.insert(QStringLiteral("hash64"), text_hash64(result.text));
    }
    return object;
}

QJsonObject snapshot_diagnostics_object(const term::Terminal_render_snapshot& snapshot)
{
    QJsonObject cursor;
    cursor.insert(QStringLiteral("visible"), snapshot.cursor.visible);
    cursor.insert(QStringLiteral("position"), position_object(snapshot.cursor.position));
    const term::Terminal_render_snapshot_row_content_view rows(snapshot);

    QJsonObject object;
    object.insert(QStringLiteral("mode"), QStringLiteral("compact"));
    object.insert(QStringLiteral("grid_size"), grid_size_object(snapshot.grid_size));
    object.insert(QStringLiteral("viewport"), viewport_object(snapshot.viewport));
    object.insert(QStringLiteral("cursor"), cursor);
    object.insert(QStringLiteral("snapshot_basis"), term::render_snapshot_basis_name(snapshot.basis));
    object.insert(QStringLiteral("snapshot_purpose"), term::render_snapshot_purpose_name(snapshot.purpose));
    object.insert(
        QStringLiteral("effective_synchronized_output_scroll_policy"),
        term::synchronized_output_scroll_policy_name(
            snapshot.public_scroll_diagnostics.effective_policy));
    object.insert(
        QStringLiteral("synchronized_output_scroll_policy_change_event"),
        term::synchronized_output_policy_change_event_name(
            snapshot.public_scroll_diagnostics.policy_change_event));
    object.insert(
        QStringLiteral("diagnostic_reason"),
        term::public_scroll_diagnostic_reason_name(
            snapshot.public_scroll_diagnostics.diagnostic_reason));
    object.insert(
        QStringLiteral("public_projection_generation"),
        static_cast<qint64>(snapshot.public_scroll_diagnostics.public_projection_generation));
    object.insert(
        QStringLiteral("public_viewport_before"),
        viewport_object(
            valid_transcript_viewport_or_fallback(
                snapshot.public_scroll_diagnostics.public_viewport_before,
                snapshot.viewport)));
    object.insert(
        QStringLiteral("public_viewport_after"),
        viewport_object(
            valid_transcript_viewport_or_fallback(
                snapshot.public_scroll_diagnostics.public_viewport_after,
                snapshot.viewport)));
    object.insert(
        QStringLiteral("live_viewport_before_on_release"),
        viewport_object(
            valid_transcript_viewport_or_fallback(
                snapshot.public_scroll_diagnostics.live_viewport_before_on_release,
                snapshot.viewport)));
    object.insert(
        QStringLiteral("live_viewport_after_on_release"),
        viewport_object(
            valid_transcript_viewport_or_fallback(
                snapshot.public_scroll_diagnostics.live_viewport_after_on_release,
                snapshot.viewport)));
    object.insert(
        QStringLiteral("visible_scroll_applied"),
        snapshot.public_scroll_diagnostics.visible_scroll_applied);
    object.insert(
        QStringLiteral("live_content_publication_blocked"),
        snapshot.public_scroll_diagnostics.live_content_publication_blocked);
    object.insert(
        QStringLiteral("release_reconciliation_result"),
        term::release_reconciliation_result_name(
            snapshot.public_scroll_diagnostics.release_reconciliation_result));
    object.insert(
        QStringLiteral("hidden_row_eligibility"),
        term::hidden_row_eligibility_name(
            snapshot.public_scroll_diagnostics.hidden_row_eligibility));
    object.insert(
        QStringLiteral("hidden_row_clamp_reason"),
        term::hidden_row_clamp_reason_name(
            snapshot.public_scroll_diagnostics.hidden_row_clamp_reason));
    object.insert(
        QStringLiteral("public_projection_disable_reason"),
        term::public_projection_disable_reason_name(
            snapshot.public_scroll_diagnostics.public_projection_disable_reason));
    object.insert(QStringLiteral("snapshot_sequence"), static_cast<qint64>(snapshot.metadata.sequence));
    object.insert(
        QStringLiteral("row_origin_generation"),
        static_cast<qint64>(snapshot.metadata.row_origin_generation));
    object.insert(QStringLiteral("cell_count"), static_cast<int>(rows.cell_count()));
    object.insert(
        QStringLiteral("dirty_row_range_count"),
        static_cast<int>(snapshot.dirty_row_ranges.size()));
    object.insert(
        QStringLiteral("selection_span_count"),
        static_cast<int>(snapshot.selection_spans.size()));
    object.insert(QStringLiteral("backend_geometry_in_sync"), snapshot.metadata.backend_geometry_in_sync);
    object.insert(QStringLiteral("visible_rows"), visible_rows_array(snapshot));
    object.insert(QStringLiteral("row_provenance"), row_provenance_array(snapshot));
    object.insert(QStringLiteral("dirty_row_ranges"), dirty_row_ranges_array(snapshot));
    object.insert(QStringLiteral("selection_spans"), selection_spans_array(snapshot));
    object.insert(QStringLiteral("selected_text"), selected_text_object(snapshot));
    return object;
}

void remove_snapshot_envelope_fields(QJsonObject& object)
{
    object.remove(QStringLiteral("kind"));
    object.remove(QStringLiteral("event_index"));
    object.remove(QStringLiteral("session_sequence"));
    object.remove(QStringLiteral("reason"));
}

QJsonObject comparable_model_snapshot_object(QJsonObject object)
{
    remove_snapshot_envelope_fields(object);
    object.remove(QStringLiteral("dirty_row_range_count"));
    object.remove(QStringLiteral("dirty_row_ranges"));
    return object;
}

QByteArray canonical_model_json(QJsonObject object)
{
    return QJsonDocument(comparable_model_snapshot_object(std::move(object))).toJson(
        QJsonDocument::Compact);
}

QJsonObject comparable_dirty_snapshot_object(const QJsonObject& object)
{
    QJsonObject dirty;
    dirty.insert(
        QStringLiteral("dirty_row_range_count"),
        object.value(QStringLiteral("dirty_row_range_count")));
    dirty.insert(
        QStringLiteral("dirty_row_ranges"),
        object.value(QStringLiteral("dirty_row_ranges")));
    return dirty;
}

bool dirty_snapshot_fields_differ(
    const QJsonObject& recorded,
    const QJsonObject& replayed)
{
    return comparable_dirty_snapshot_object(recorded) != comparable_dirty_snapshot_object(replayed);
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

bool is_replay_transparent_diagnostic_event(const QString& kind)
{
    return
        kind == QStringLiteral("transcript.timing")      ||
        kind == QStringLiteral("surface.wheel_ingress")  ||
        kind == QStringLiteral("surface.wheel_trace");
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

std::vector<QString> differing_top_level_fields(
    const QJsonObject& recorded,
    const QJsonObject& replayed)
{
    std::set<QString> field_names;
    for (auto it = recorded.begin(); it != recorded.end(); ++it) {
        field_names.insert(it.key());
    }
    for (auto it = replayed.begin(); it != replayed.end(); ++it) {
        field_names.insert(it.key());
    }

    std::vector<QString> fields;
    for (const QString& field_name : field_names) {
        if (recorded.value(field_name) != replayed.value(field_name)) {
            fields.push_back(field_name);
        }
    }
    return fields;
}

QString join_fields(const std::vector<QString>& fields)
{
    QStringList names;
    for (const QString& field : fields) {
        names.push_back(field);
    }
    return names.join(QStringLiteral(","));
}

class Replay_backend final : public term::Terminal_backend
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

    term::Terminal_backend_result write(QByteArray bytes) override
    {
        if (!m_running) {
            return term::backend_reject(
                term::Terminal_backend_error_code::WRITE_FAILED,
                QStringLiteral("replay backend is not running"));
        }

        writes.push_back(std::move(bytes));
        return term::backend_accept();
    }

    term::Terminal_backend_result resize(term::Terminal_backend_resize_request request) override
    {
        resizes.push_back(request);
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
        m_running = false;
        m_callbacks.process_exited(exit);
    }

    std::vector<QByteArray> writes;
    std::vector<term::Terminal_backend_resize_request> resizes;

private:
    bool                             m_running = false;
    term::Terminal_backend_callbacks m_callbacks;
};

struct Replay_result
{
    QString error;
    int terminal_reply_host_writes_skipped = 0;
    int backend_error_events               = 0;
    int process_exit_events                = 0;
    int text_area_resize_request_events    = 0;
    int recorded_snapshot_events           = 0;
    int intermediate_snapshot_events       = 0;
    int matching_snapshot_events           = 0;
    int divergent_snapshot_events          = 0;
    int dirty_mismatch_snapshot_events     = 0;
    int public_projection_scroll_snapshot_events = 0;
    int semantic_selection_events          = 0;
    int surface_scroll_intents             = 0;
    std::optional<std::uint64_t> first_divergent_event_index;
    std::optional<std::uint64_t> first_divergent_recorded_snapshot_sequence;
    std::optional<std::uint64_t> first_divergent_replayed_snapshot_sequence;
    std::vector<QString> first_divergent_fields;
    QString first_divergent_recorded_selected_text_result;
    QString first_divergent_replayed_selected_text_result;
    std::optional<std::uint64_t> first_dirty_mismatch_event_index;
    std::vector<QString> first_dirty_mismatch_fields;
    std::vector<QByteArray> host_writes;
    std::optional<term::Terminal_render_snapshot> snapshot;
    term::Terminal_viewport_state viewport;
    term::Terminal_selection_result selected_text;
};

QString selected_text_result_from_snapshot_object(const QJsonObject& object)
{
    return object.value(QStringLiteral("selected_text"))
        .toObject()
        .value(QStringLiteral("result"))
        .toString(QStringLiteral("missing"));
}

std::optional<std::uint64_t> snapshot_sequence_from_object(const QJsonObject& object)
{
    const QJsonValue value = object.value(QStringLiteral("snapshot_sequence"));
    if (!value.isDouble()) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(value.toDouble());
}

void record_first_snapshot_divergence(
    Replay_result&                         replay,
    const term::Terminal_transcript_event& event,
    const std::optional<QJsonObject>&      replayed)
{
    if (replay.first_divergent_event_index.has_value()) {
        return;
    }

    replay.first_divergent_event_index = event.event_index;
    replay.first_divergent_recorded_selected_text_result =
        selected_text_result_from_snapshot_object(event.object);
    replay.first_divergent_recorded_snapshot_sequence =
        snapshot_sequence_from_object(event.object);
    if (replayed.has_value()) {
        replay.first_divergent_replayed_snapshot_sequence =
            snapshot_sequence_from_object(*replayed);
        replay.first_divergent_replayed_selected_text_result =
            selected_text_result_from_snapshot_object(*replayed);
        replay.first_divergent_fields = differing_top_level_fields(
            comparable_model_snapshot_object(event.object),
            comparable_model_snapshot_object(*replayed));
    }
    else {
        replay.first_divergent_replayed_selected_text_result = QStringLiteral("missing");
        replay.first_divergent_fields = {QStringLiteral("snapshot")};
    }
}

void record_first_dirty_mismatch(
    Replay_result&                         replay,
    const term::Terminal_transcript_event& event,
    const QJsonObject&                     replayed)
{
    if (replay.first_dirty_mismatch_event_index.has_value()) {
        return;
    }

    replay.first_dirty_mismatch_event_index = event.event_index;
    replay.first_dirty_mismatch_fields = differing_top_level_fields(
        comparable_dirty_snapshot_object(event.object),
        comparable_dirty_snapshot_object(replayed));
}

void compare_recorded_snapshot(
    Replay_result&                         replay,
    const term::Terminal_session&          session,
    const term::Terminal_transcript_event& event)
{
    const std::optional<term::Terminal_render_snapshot> snapshot =
        session.latest_render_snapshot();
    const bool recorded_public_projection_scroll =
        event.object.value(QStringLiteral("snapshot_basis")).toString() ==
            QStringLiteral("PUBLIC_PROJECTION") &&
        event.object.value(QStringLiteral("snapshot_purpose")).toString() ==
            QStringLiteral("SCROLL");
    if (!snapshot.has_value()) {
        ++replay.divergent_snapshot_events;
        record_first_snapshot_divergence(replay, event, std::nullopt);
        return;
    }

    const QJsonObject replayed_object = snapshot_diagnostics_object(*snapshot);
    if (recorded_public_projection_scroll &&
        (snapshot->basis   != term::Terminal_render_snapshot_basis::PUBLIC_PROJECTION ||
            snapshot->purpose != term::Terminal_render_snapshot_purpose::SCROLL))
    {
        ++replay.divergent_snapshot_events;
        record_first_snapshot_divergence(replay, event, replayed_object);
        return;
    }

    if (dirty_snapshot_fields_differ(event.object, replayed_object)) {
        ++replay.dirty_mismatch_snapshot_events;
        record_first_dirty_mismatch(replay, event, replayed_object);
    }

    const QByteArray recorded = canonical_model_json(event.object);
    const QByteArray replayed = canonical_model_json(replayed_object);
    if (recorded == replayed) {
        if (recorded_public_projection_scroll) {
            ++replay.public_projection_scroll_snapshot_events;
        }
        ++replay.matching_snapshot_events;
        return;
    }

    ++replay.divergent_snapshot_events;
    record_first_snapshot_divergence(replay, event, replayed_object);
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

bool scroll_event_uses_published_state_source(
    const term::Terminal_transcript_event& event)
{
    return
        event.object.value(QStringLiteral("source")).toString() ==
            QStringLiteral("surface.text_area.wheel") &&
        event.object.contains(QStringLiteral("viewport_before"));
}

bool apply_scroll_event(
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
        *out_error = QStringLiteral("replay cannot synthesize terminal-input scroll for event %1")
            .arg(static_cast<qulonglong>(event.event_index));
        return false;
    }
    return true;
}

void apply_selection_event(
    term::Terminal_session&                session,
    const term::Terminal_transcript_event& event)
{
    const QString phase = event.object.value(QStringLiteral("phase")).toString();
    const bool moved    = event.object.value(QStringLiteral("moved")).toBool();
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

Replay_result replay_events(const std::vector<term::Terminal_transcript_event>& events)
{
    Replay_result replay;
    auto backend = std::make_unique<Replay_backend>();
    Replay_backend* backend_ptr = backend.get();
    term::Terminal_session session(std::move(backend), replay_session_config(events));

    const bool has_resize_request = std::any_of(
        events.begin(),
        events.end(),
        [](const term::Terminal_transcript_event& event) {
            return event.kind == QStringLiteral("session.resize_request");
        });

    bool started = false;
    std::optional<Pending_scroll_intent> pending_scroll_intent;
    std::optional<term::Terminal_transcript_event> pending_snapshot_event;
    const auto flush_pending_snapshot = [&]() {
        if (!pending_snapshot_event.has_value()) {
            return;
        }

        compare_recorded_snapshot(replay, session, *pending_snapshot_event);
        pending_snapshot_event.reset();
    };

    for (const term::Terminal_transcript_event& event : events) {
        if (event.kind == QStringLiteral("header") ||
            is_replay_transparent_diagnostic_event(event.kind))
        {
            continue;
        }

        if (event.kind != QStringLiteral("snapshot")) {
            flush_pending_snapshot();
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
            const QString source = event.object.value(QStringLiteral("source")).toString();
            if (source == QStringLiteral("terminal_reply")) {
                ++replay.terminal_reply_host_writes_skipped;
                continue;
            }

            const QByteArray bytes = event_bytes(event);
            const std::optional<bool> focused = focus_state_from_host_write_bytes(bytes);
            const term::Terminal_session_result result =
                focused.has_value()
                    ? session.write_focus_event(*focused).result
                    : session.write_user_bytes(bytes);
            if (result.code != term::Terminal_session_result_code::ACCEPTED) {
                replay.error = QStringLiteral("replay host.write was rejected at event %1")
                    .arg(static_cast<qulonglong>(event.event_index));
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
                replay.error = QStringLiteral("replay session.resize was rejected at event %1")
                    .arg(static_cast<qulonglong>(event.event_index));
                return replay;
            }
        }
        else
        if (event.kind == QStringLiteral("session.backend_error")) {
            backend_ptr->emit_error({
                term::Terminal_backend_error_code::READ_FAILED,
                event.object.value(QStringLiteral("message")).toString(),
            });
            session.process_backend_callback_events();
            ++replay.backend_error_events;
        }
        else
        if (event.kind == QStringLiteral("session.process_exit")) {
            backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 0});
            session.process_backend_callback_events();
            ++replay.process_exit_events;
        }
        else
        if (event.kind == QStringLiteral("session.text_area_resize_request")) {
            ++replay.text_area_resize_request_events;
        }
        else
        if (event.kind == QStringLiteral("surface.scroll_intent")) {
            term::Terminal_viewport_scroll_result result;
            if (!apply_scroll_event(session, event, &result, &replay.error)) {
                return replay;
            }
            pending_scroll_intent.reset();
            if (result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED ||
                result.action ==
                    term::Terminal_viewport_scroll_action::DEFERRED_INTENT_RECORDED)
            {
                pending_scroll_intent = pending_scroll_intent_from_event(event, result);
            }
            ++replay.surface_scroll_intents;
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
                if (!apply_scroll_event(session, event, &result, &replay.error)) {
                    return replay;
                }
            }
        }
        else
        if (event.kind == QStringLiteral("surface.selection_drag")) {
            apply_selection_event(session, event);
            ++replay.semantic_selection_events;
        }
        else
        if (event.kind == QStringLiteral("snapshot")) {
            ++replay.recorded_snapshot_events;
            if (pending_snapshot_event.has_value()) {
                ++replay.intermediate_snapshot_events;
            }
            pending_snapshot_event = event;
        }
    }
    flush_pending_snapshot();

    replay.host_writes   = backend_ptr->writes;
    replay.snapshot      = session.latest_render_snapshot();
    replay.viewport      = session.viewport_state();
    replay.selected_text = session.selected_text();
    if (replay.divergent_snapshot_events != 0) {
        replay.error = QStringLiteral("recorded snapshot diagnostics diverged from replayed model");
    }
    return replay;
}

void print_snapshot_diagnostics(const term::Terminal_render_snapshot& snapshot)
{
    const term::Terminal_render_snapshot_row_content_view rows(snapshot);
    std::cout
        << "grid=" << snapshot.grid_size.rows << "x" << snapshot.grid_size.columns << '\n'
        << "viewport.active_buffer=" << buffer_name(snapshot.viewport.active_buffer).toStdString()
        << " viewport.scrollback_rows=" << snapshot.viewport.scrollback_rows
        << " viewport.visible_rows=" << snapshot.viewport.visible_rows
        << " viewport.offset_from_tail=" << snapshot.viewport.offset_from_tail
        << " viewport.follow_tail=" << (snapshot.viewport.follow_tail ? "true" : "false") << '\n'
        << "snapshot.sequence=" << static_cast<unsigned long long>(snapshot.metadata.sequence)
        << " row_origin_generation="
        << static_cast<unsigned long long>(snapshot.metadata.row_origin_generation) << '\n'
        << "snapshot.basis=" << term::render_snapshot_basis_name(snapshot.basis).toStdString()
        << " snapshot.purpose=" << term::render_snapshot_purpose_name(snapshot.purpose).toStdString()
        << '\n'
        << "cell_count=" << rows.cell_count()
        << " dirty_row_range_count=" << snapshot.dirty_row_ranges.size()
        << " selection_span_count=" << snapshot.selection_spans.size() << '\n';

    for (const term::Terminal_render_dirty_row_range& range : snapshot.dirty_row_ranges) {
        std::cout
            << "dirty_row_range first_row=" << range.first_row
            << " row_count=" << range.row_count << '\n';
    }

    for (int row = 0; row < snapshot.grid_size.rows; ++row) {
        const QString text = snapshot_row_text(snapshot, row);
        std::cout
            << "visible_row[" << row << "] hash64=" << text_hash64(text).toStdString()
            << " text=\"" << escaped_text(text).toStdString() << "\"";
        if (row < static_cast<int>(snapshot.visible_line_provenance.size())) {
            const term::Terminal_render_line_provenance& provenance =
                snapshot.visible_line_provenance[static_cast<std::size_t>(row)];
            std::cout
                << " logical_row=" << static_cast<long long>(provenance.logical_row)
                << " retained_line_id="
                << static_cast<unsigned long long>(provenance.retained_line_id)
                << " content_generation="
                << static_cast<unsigned long long>(provenance.content_generation)
                << " source="
                << term::retained_line_provenance_source_name(provenance.source).toStdString();
        }
        std::cout << '\n';
    }

    for (const term::Terminal_render_selection_span& span : snapshot.selection_spans) {
        std::cout
            << "selection_span row=" << span.row
            << " first_column=" << span.first_column
            << " column_count=" << span.column_count
            << " source_start=" << span.source_range.start.row << ':' << span.source_range.start.column
            << " source_end=" << span.source_range.end.row << ':' << span.source_range.end.column
            << " mode=" << selection_mode_name(span.source_range.mode).toStdString()
            << '\n';
    }
}

}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    if (arguments.size() != 2) {
        std::cerr << usage_text().toStdString();
        return 2;
    }

    QString error;
    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(arguments.at(1), &error);
    if (!events.has_value()) {
        std::cerr << "transcript invalid: " << error.toStdString() << '\n';
        return 2;
    }

    Replay_result replay = replay_events(*events);
    if (!replay.snapshot.has_value()) {
        if (replay.error.isEmpty()) {
            replay.error = QStringLiteral("replay produced no render snapshot");
        }
    }

    std::cout
        << "transcript_events=" << events->size() << '\n'
        << "recorded_snapshot_events=" << replay.recorded_snapshot_events
        << " intermediate_snapshot_events=" << replay.intermediate_snapshot_events
        << " matching_snapshot_events=" << replay.matching_snapshot_events
        << " divergent_snapshot_events=" << replay.divergent_snapshot_events << '\n'
        << "dirty_mismatch_snapshot_events=" << replay.dirty_mismatch_snapshot_events << '\n'
        << "public_projection_scroll_snapshot_events="
        << replay.public_projection_scroll_snapshot_events << '\n'
        << "terminal_reply_host_writes_skipped="
        << replay.terminal_reply_host_writes_skipped << '\n'
        << "backend_error_events=" << replay.backend_error_events
        << " process_exit_events=" << replay.process_exit_events
        << " text_area_resize_request_events="
        << replay.text_area_resize_request_events << '\n'
        << "surface_scroll_intents=" << replay.surface_scroll_intents
        << " semantic_selection_events=" << replay.semantic_selection_events << '\n'
        << "host_write_count=" << replay.host_writes.size() << '\n'
        << "selection_replay="
        << (replay.semantic_selection_events == 0
            ? "none"
            : "semantic_range_from_surface_selection_drag")
        << '\n';

    if (replay.first_divergent_event_index.has_value()) {
        std::cout
            << "first_divergent_snapshot.event_index="
            << static_cast<unsigned long long>(*replay.first_divergent_event_index)
            << " recorded_snapshot_sequence=";
        if (replay.first_divergent_recorded_snapshot_sequence.has_value()) {
            std::cout << static_cast<unsigned long long>(
                *replay.first_divergent_recorded_snapshot_sequence);
        }
        else {
            std::cout << "unknown";
        }
        std::cout << " replayed_snapshot_sequence=";
        if (replay.first_divergent_replayed_snapshot_sequence.has_value()) {
            std::cout << static_cast<unsigned long long>(
                *replay.first_divergent_replayed_snapshot_sequence);
        }
        else {
            std::cout << "missing";
        }
        std::cout
            << " fields="
            << join_fields(replay.first_divergent_fields).toStdString()
            << " recorded_selected_text.result="
            << replay.first_divergent_recorded_selected_text_result.toStdString()
            << " replayed_selected_text.result="
            << replay.first_divergent_replayed_selected_text_result.toStdString()
            << '\n';
    }

    if (replay.first_dirty_mismatch_event_index.has_value()) {
        std::cout
            << "first_dirty_mismatch_snapshot.event_index="
            << static_cast<unsigned long long>(*replay.first_dirty_mismatch_event_index)
            << " fields="
            << join_fields(replay.first_dirty_mismatch_fields).toStdString()
            << '\n';
    }

    if (replay.snapshot.has_value()) {
        print_snapshot_diagnostics(*replay.snapshot);
    }

    std::cout
        << "selected_text.result=" << selection_result_code_name(replay.selected_text.code).toStdString()
        << " selected_text=\""
        << escaped_text(replay.selected_text.text).toStdString() << "\""
        << " selected_text.hash64=" << text_hash64(replay.selected_text.text).toStdString()
        << '\n';

    if (!replay.error.isEmpty()) {
        std::cerr << "replay failed: " << replay.error.toStdString() << '\n';
        return 3;
    }

    return 0;
}
