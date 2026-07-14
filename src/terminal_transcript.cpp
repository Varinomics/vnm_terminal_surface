#include "vnm_terminal/internal/terminal_transcript.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
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
    object.insert(QStringLiteral("deferred_intent_recorded"), deferred_intent_recorded);
    // Public-projection scroll publication passes an explicit visible result.
    object.insert(
        QStringLiteral("visible_scroll_applied"),
        visible_scroll_applied.value_or(
            local_scroll_applied && !render_publication_blocked));
}

namespace {

constexpr int k_transcript_schema_version = 1;
constexpr qint64 k_transcript_timing_record_threshold_ns = 250000;

void insert_u64(QJsonObject& object, const QString& name, std::uint64_t value)
{
    object.insert(name, static_cast<qint64>(value));
}

void insert_u64_if_missing(QJsonObject& object, const QString& name, std::uint64_t value)
{
    if (!object.contains(name)) {
        insert_u64(object, name, value);
    }
}

void insert_string_if_missing(QJsonObject& object, const QString& name, const QString& value)
{
    if (!object.contains(name)) {
        object.insert(name, value);
    }
}

void insert_bool_if_missing(QJsonObject& object, const QString& name, bool value)
{
    if (!object.contains(name)) {
        object.insert(name, value);
    }
}

QString u64_string(std::uint64_t value)
{
    return QString::number(static_cast<qulonglong>(value));
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

QString bytes_base64(QByteArrayView bytes)
{
    return QString::fromLatin1(QByteArray(bytes.data(), bytes.size()).toBase64());
}

QJsonObject grid_size_object(terminal_grid_size_t grid_size)
{
    return {
        {QStringLiteral("rows"),    grid_size.rows},
        {QStringLiteral("columns"), grid_size.columns},
    };
}

QJsonObject position_object(terminal_grid_position_t position)
{
    return {
        {QStringLiteral("row"),    position.row},
        {QStringLiteral("column"), position.column},
    };
}

QString buffer_name(Terminal_buffer_id buffer)
{
    switch (buffer) {
        case Terminal_buffer_id::PRIMARY:
            return QStringLiteral("primary");
        case Terminal_buffer_id::ALTERNATE:
            return QStringLiteral("alternate");
    }

    return QStringLiteral("unknown");
}

QString selection_mode_name(Terminal_selection_mode mode)
{
    switch (mode) {
        case Terminal_selection_mode::NONE:
            return QStringLiteral("none");
        case Terminal_selection_mode::NORMAL:
            return QStringLiteral("normal");
        case Terminal_selection_mode::WORD:
            return QStringLiteral("word");
        case Terminal_selection_mode::LINE:
            return QStringLiteral("line");
    }

    return QStringLiteral("unknown");
}

QString alternate_screen_scroll_policy_name(Terminal_alternate_screen_scroll_policy policy)
{
    switch (policy) {
        case Terminal_alternate_screen_scroll_policy::KEEP_AT_TAIL:
            return QStringLiteral("keep_at_tail");
        case Terminal_alternate_screen_scroll_policy::WHEEL_TO_TERMINAL_INPUT:
            return QStringLiteral("wheel_to_terminal_input");
    }

    return QStringLiteral("unknown");
}

QJsonObject selection_range_object(const Terminal_selection_range& range)
{
    return {
        {QStringLiteral("start"), position_object(range.start)},
        {QStringLiteral("end"),   position_object(range.end)},
        {QStringLiteral("mode"),  selection_mode_name(range.mode)},
    };
}

QString selection_result_code_name(Terminal_selection_result_code code)
{
    switch (code) {
        case Terminal_selection_result_code::OK:
            return QStringLiteral("ok");
        case Terminal_selection_result_code::NO_SELECTION:
            return QStringLiteral("no_selection");
        case Terminal_selection_result_code::INVALID_RANGE:
            return QStringLiteral("invalid_range");
    }

    return QStringLiteral("unknown");
}

QJsonArray dirty_row_ranges_array(const Terminal_render_snapshot& snapshot)
{
    QJsonArray rows;
    for (const Terminal_render_dirty_row_range& range : snapshot.dirty_row_ranges) {
        rows.append(QJsonObject{
            {QStringLiteral("first_row"), range.first_row},
            {QStringLiteral("row_count"), range.row_count},
        });
    }
    return rows;
}

QJsonArray row_provenance_array(const Terminal_render_snapshot& snapshot)
{
    QJsonArray rows;
    const int row_count = std::min<int>(
        snapshot.grid_size.rows,
        static_cast<int>(snapshot.visible_line_provenance.size()));
    for (int row = 0; row < row_count; ++row) {
        const Terminal_render_line_provenance& provenance =
            snapshot.visible_line_provenance[static_cast<std::size_t>(row)];
        QJsonObject object;
        object.insert(QStringLiteral("row"), row);
        object.insert(QStringLiteral("logical_row"), static_cast<qint64>(provenance.logical_row));
        object.insert(QStringLiteral("retained_line_id"), u64_string(provenance.retained_line_id));
        object.insert(QStringLiteral("content_generation"), u64_string(provenance.content_generation));
        object.insert(
            QStringLiteral("source"),
            retained_line_provenance_source_name(provenance.source));
        rows.append(object);
    }
    return rows;
}

QJsonArray visible_rows_array(const Terminal_render_snapshot_row_content_view& rows)
{
    QJsonArray array;
    for (int row = 0; row < rows.row_count(); ++row) {
        const QString text =
            selected_text_from_render_snapshot_row(
                rows.row_at(row),
                0,
                rows.column_count(),
                true);
        QJsonObject object;
        object.insert(QStringLiteral("row"), row);
        object.insert(QStringLiteral("text"), text);
        object.insert(QStringLiteral("hash64"), text_hash64(text));
        array.append(object);
    }
    return array;
}

QJsonArray selection_spans_array(const Terminal_render_snapshot& snapshot)
{
    QJsonArray spans;
    for (const Terminal_render_selection_span& span : snapshot.selection_spans) {
        spans.append(QJsonObject{
            {QStringLiteral("row"),          span.row},
            {QStringLiteral("first_column"), span.first_column},
            {QStringLiteral("column_count"), span.column_count},
            {QStringLiteral("source_range"), selection_range_object(span.source_range)},
        });
    }
    return spans;
}

QJsonObject selected_text_object(const Terminal_render_snapshot& snapshot)
{
    QJsonObject object;
    if (snapshot.selection_spans.empty()) {
        object.insert(QStringLiteral("available"), false);
        object.insert(QStringLiteral("result"), QStringLiteral("no_selection"));
        return object;
    }

    const Terminal_selection_range& range = snapshot.selection_spans.front().source_range;
    const Terminal_selection_result result =
        selected_text_from_render_snapshot(snapshot, range);
    object.insert(QStringLiteral("available"), result.code == Terminal_selection_result_code::OK);
    object.insert(QStringLiteral("result"), selection_result_code_name(result.code));
    object.insert(QStringLiteral("source_range"), selection_range_object(range));
    if (result.code == Terminal_selection_result_code::OK) {
        object.insert(QStringLiteral("text"), result.text);
        object.insert(QStringLiteral("hash64"), text_hash64(result.text));
    }
    return object;
}

QJsonObject viewport_object(const Terminal_viewport_state& viewport)
{
    return {
        {QStringLiteral("active_buffer"),    buffer_name(viewport.active_buffer)},
        {QStringLiteral("scrollback_rows"),  viewport.scrollback_rows},
        {QStringLiteral("visible_rows"),     viewport.visible_rows},
        {QStringLiteral("offset_from_tail"), viewport.offset_from_tail},
        {QStringLiteral("follow_tail"),      viewport.follow_tail},
        {QStringLiteral("alternate_screen_scroll_policy"),
            alternate_screen_scroll_policy_name(viewport.alternate_screen_scroll_policy)},
    };
}

Terminal_viewport_state valid_transcript_viewport_or_fallback(
    Terminal_viewport_state viewport,
    Terminal_viewport_state fallback)
{
    const Terminal_viewport_state default_viewport;
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

void insert_public_scroll_policy_defaults(QJsonObject& object)
{
    insert_string_if_missing(
        object,
        QStringLiteral("effective_synchronized_output_scroll_policy"),
        synchronized_output_scroll_policy_name(
            Terminal_synchronized_output_scroll_policy::DEFER_UNTIL_CONTENT_PUBLICATION));
    insert_string_if_missing(
        object,
        QStringLiteral("synchronized_output_scroll_policy_change_event"),
        synchronized_output_policy_change_event_name(
            Terminal_synchronized_output_policy_change_event::NONE));
    insert_string_if_missing(
        object,
        QStringLiteral("diagnostic_reason"),
        public_scroll_diagnostic_reason_name(
            Terminal_public_scroll_diagnostic_reason::NONE));
}

QString legacy_snapshot_purpose_name(const QJsonObject& object)
{
    const QString reason = object.value(QStringLiteral("reason")).toString();

    // Compatibility for snapshot records written before snapshot_purpose
    // existed is limited to producer-owned reason strings. Free-form reason
    // text remains CONTENT even when it happens to mention selection or
    // geometry.
    if (reason == QStringLiteral("selection changed")          ||
        reason == QStringLiteral("selection cleared")          ||
        reason == QStringLiteral("selection visual detached"))
    {
        return render_snapshot_purpose_name(
            Terminal_render_snapshot_purpose::SELECTION_DERIVED);
    }

    if (reason == QStringLiteral("resize geometry snapshot ready")) {
        return render_snapshot_purpose_name(
            Terminal_render_snapshot_purpose::GEOMETRY_DERIVED);
    }

    return render_snapshot_purpose_name(Terminal_render_snapshot_purpose::CONTENT);
}

void insert_snapshot_public_scroll_fields(
    QJsonObject&                    object,
    const Terminal_render_snapshot& snapshot)
{
    const Terminal_public_scroll_diagnostics& diagnostics =
        snapshot.public_scroll_diagnostics;
    const Terminal_viewport_state public_viewport_before =
        valid_transcript_viewport_or_fallback(
            diagnostics.public_viewport_before,
            snapshot.viewport);
    const Terminal_viewport_state public_viewport_after =
        valid_transcript_viewport_or_fallback(
            diagnostics.public_viewport_after,
            snapshot.viewport);
    const Terminal_viewport_state live_viewport_before_on_release =
        valid_transcript_viewport_or_fallback(
            diagnostics.live_viewport_before_on_release,
            snapshot.viewport);
    const Terminal_viewport_state live_viewport_after_on_release =
        valid_transcript_viewport_or_fallback(
            diagnostics.live_viewport_after_on_release,
            snapshot.viewport);

    object.insert(QStringLiteral("snapshot_basis"), render_snapshot_basis_name(snapshot.basis));
    object.insert(QStringLiteral("snapshot_purpose"), render_snapshot_purpose_name(snapshot.purpose));
    object.insert(
        QStringLiteral("effective_synchronized_output_scroll_policy"),
        synchronized_output_scroll_policy_name(diagnostics.effective_policy));
    object.insert(
        QStringLiteral("synchronized_output_scroll_policy_change_event"),
        synchronized_output_policy_change_event_name(diagnostics.policy_change_event));
    object.insert(
        QStringLiteral("diagnostic_reason"),
        public_scroll_diagnostic_reason_name(diagnostics.diagnostic_reason));
    insert_u64(
        object,
        QStringLiteral("public_projection_generation"),
        diagnostics.public_projection_generation);
    object.insert(
        QStringLiteral("public_viewport_before"),
        viewport_object(public_viewport_before));
    object.insert(
        QStringLiteral("public_viewport_after"),
        viewport_object(public_viewport_after));
    object.insert(
        QStringLiteral("live_viewport_before_on_release"),
        viewport_object(live_viewport_before_on_release));
    object.insert(
        QStringLiteral("live_viewport_after_on_release"),
        viewport_object(live_viewport_after_on_release));
    object.insert(QStringLiteral("visible_scroll_applied"), diagnostics.visible_scroll_applied);
    object.insert(
        QStringLiteral("live_content_publication_blocked"),
        diagnostics.live_content_publication_blocked);
    object.insert(
        QStringLiteral("release_reconciliation_result"),
        release_reconciliation_result_name(diagnostics.release_reconciliation_result));
    object.insert(
        QStringLiteral("hidden_row_eligibility"),
        hidden_row_eligibility_name(diagnostics.hidden_row_eligibility));
    object.insert(
        QStringLiteral("hidden_row_clamp_reason"),
        hidden_row_clamp_reason_name(diagnostics.hidden_row_clamp_reason));
    object.insert(
        QStringLiteral("public_projection_disable_reason"),
        public_projection_disable_reason_name(diagnostics.public_projection_disable_reason));
}

void apply_snapshot_schema_compatibility_defaults(QJsonObject& object)
{
    insert_string_if_missing(
        object,
        QStringLiteral("snapshot_basis"),
        render_snapshot_basis_name(Terminal_render_snapshot_basis::LIVE_CONTENT));
    insert_string_if_missing(
        object,
        QStringLiteral("snapshot_purpose"),
        legacy_snapshot_purpose_name(object));
    insert_public_scroll_policy_defaults(object);
    insert_u64_if_missing(object, QStringLiteral("public_projection_generation"), 0U);
    insert_bool_if_missing(object, QStringLiteral("visible_scroll_applied"), false);
    insert_bool_if_missing(object, QStringLiteral("live_content_publication_blocked"), false);
    insert_string_if_missing(
        object,
        QStringLiteral("release_reconciliation_result"),
        release_reconciliation_result_name(Terminal_release_reconciliation_result::NONE));
    insert_string_if_missing(
        object,
        QStringLiteral("hidden_row_eligibility"),
        hidden_row_eligibility_name(Terminal_hidden_row_eligibility::NOT_EVALUATED));
    insert_string_if_missing(
        object,
        QStringLiteral("hidden_row_clamp_reason"),
        hidden_row_clamp_reason_name(Terminal_hidden_row_clamp_reason::NONE));
    insert_string_if_missing(
        object,
        QStringLiteral("public_projection_disable_reason"),
        public_projection_disable_reason_name(Terminal_public_projection_disable_reason::NONE));

    const QJsonObject viewport = object.value(QStringLiteral("viewport")).toObject();
    if (!viewport.isEmpty()) {
        if (!object.contains(QStringLiteral("public_viewport_before"))) {
            object.insert(QStringLiteral("public_viewport_before"), viewport);
        }
        if (!object.contains(QStringLiteral("public_viewport_after"))) {
            object.insert(QStringLiteral("public_viewport_after"), viewport);
        }
        if (!object.contains(QStringLiteral("live_viewport_before_on_release"))) {
            object.insert(QStringLiteral("live_viewport_before_on_release"), viewport);
        }
        if (!object.contains(QStringLiteral("live_viewport_after_on_release"))) {
            object.insert(QStringLiteral("live_viewport_after_on_release"), viewport);
        }
    }

    QJsonArray row_provenance = object.value(QStringLiteral("row_provenance")).toArray();
    bool updated_row_provenance = false;
    for (int row = 0; row < row_provenance.size(); ++row) {
        QJsonObject provenance = row_provenance.at(row).toObject();
        if (provenance.contains(QStringLiteral("source"))) {
            continue;
        }

        provenance.insert(QStringLiteral("source"), QStringLiteral("terminal_storage"));
        row_provenance.replace(row, provenance);
        updated_row_provenance = true;
    }
    if (updated_row_provenance) {
        object.insert(QStringLiteral("row_provenance"), row_provenance);
    }
}

void apply_transcript_compatibility_defaults(QJsonObject& object, const QString& kind)
{
    if (kind == QStringLiteral("snapshot")) {
        apply_snapshot_schema_compatibility_defaults(object);
        return;
    }

    if (kind == QStringLiteral("surface.wheel_trace")) {
        insert_public_scroll_policy_defaults(object);
    }
}

QString model_resize_result_name(Terminal_model_resize_result result)
{
    switch (result) {
        case Terminal_model_resize_result::APPLIED:
            return QStringLiteral("applied");
        case Terminal_model_resize_result::INVALID_GRID_SIZE:
            return QStringLiteral("invalid_grid_size");
        case Terminal_model_resize_result::NOT_APPLIED:
            return QStringLiteral("not_applied");
    }

    return QStringLiteral("unknown");
}

QString backend_resize_result_name(Terminal_backend_resize_result result)
{
    switch (result) {
        case Terminal_backend_resize_result::APPLIED:
            return QStringLiteral("applied");
        case Terminal_backend_resize_result::FAILED:
            return QStringLiteral("failed");
    }

    return QStringLiteral("unknown");
}

QString exit_reason_name(Terminal_exit_reason reason)
{
    switch (reason) {
        case Terminal_exit_reason::EXITED:
            return QStringLiteral("exited");
        case Terminal_exit_reason::INTERRUPTED:
            return QStringLiteral("interrupted");
        case Terminal_exit_reason::TERMINATED:
            return QStringLiteral("terminated");
        case Terminal_exit_reason::FAILED_TO_START:
            return QStringLiteral("failed_to_start");
    }

    return QStringLiteral("unknown");
}

QString backend_error_code_name(Terminal_backend_error_code code)
{
    switch (code) {
        case Terminal_backend_error_code::INVALID_LAUNCH_CONFIG:
            return QStringLiteral("invalid_launch_config");
        case Terminal_backend_error_code::INVALID_INITIAL_GRID_SIZE:
            return QStringLiteral("invalid_initial_grid_size");
        case Terminal_backend_error_code::WORKING_DIRECTORY_UNAVAILABLE:
            return QStringLiteral("working_directory_unavailable");
        case Terminal_backend_error_code::START_FAILED:
            return QStringLiteral("start_failed");
        case Terminal_backend_error_code::WRITE_FAILED:
            return QStringLiteral("write_failed");
        case Terminal_backend_error_code::RESIZE_FAILED:
            return QStringLiteral("resize_failed");
        case Terminal_backend_error_code::INTERRUPT_FAILED:
            return QStringLiteral("interrupt_failed");
        case Terminal_backend_error_code::TERMINATE_FAILED:
            return QStringLiteral("terminate_failed");
        case Terminal_backend_error_code::OUTPUT_OVERFLOW:
            return QStringLiteral("output_overflow");
        case Terminal_backend_error_code::CALLBACK_MISSING:
            return QStringLiteral("callback_missing");
        case Terminal_backend_error_code::READ_FAILED:
            return QStringLiteral("read_failed");
    }

    return QStringLiteral("unknown");
}

QString viewport_scroll_action_name(Terminal_viewport_scroll_action action)
{
    switch (action) {
        case Terminal_viewport_scroll_action::VIEWPORT_MOVED:
            return QStringLiteral("viewport_moved");
        case Terminal_viewport_scroll_action::AT_BOUNDARY:
            return QStringLiteral("at_boundary");
        case Terminal_viewport_scroll_action::DEFERRED_INTENT_RECORDED:
            return QStringLiteral("deferred_intent_recorded");
        case Terminal_viewport_scroll_action::TERMINAL_INPUT:
            return QStringLiteral("terminal_input");
    }

    return QStringLiteral("unknown");
}

QJsonObject resize_object(const Terminal_resize_transaction& resize)
{
    QJsonObject object;
    insert_u64(object, QStringLiteral("transaction_id"), resize.id);
    object.insert(QStringLiteral("target_grid_size"), grid_size_object(resize.target_grid_size));
    object.insert(QStringLiteral("snapshot_grid_size"), grid_size_object(resize.snapshot_grid_size));
    object.insert(QStringLiteral("source_width"), resize.source_geometry.width());
    object.insert(QStringLiteral("source_height"), resize.source_geometry.height());
    object.insert(QStringLiteral("active_buffer"), buffer_name(resize.active_buffer));
    object.insert(QStringLiteral("model_result"), model_resize_result_name(resize.model_result));
    object.insert(QStringLiteral("backend_result"), backend_resize_result_name(resize.backend_result));
    object.insert(QStringLiteral("backend_geometry_in_sync"), resize.backend_geometry_in_sync);
    return object;
}

QJsonObject resize_request_object(const Terminal_resize_transaction& resize)
{
    QJsonObject object;
    insert_u64(object, QStringLiteral("transaction_id"), resize.id);
    object.insert(QStringLiteral("target_grid_size"), grid_size_object(resize.target_grid_size));
    object.insert(QStringLiteral("source_width"), resize.source_geometry.width());
    object.insert(QStringLiteral("source_height"), resize.source_geometry.height());
    object.insert(QStringLiteral("active_buffer"), buffer_name(resize.active_buffer));
    return object;
}

QJsonObject session_config_object(const Terminal_session_config& config)
{
    const int effective_scrollback_limit = std::max(0, config.scrollback_limit);
    return {
        {QStringLiteral("recover_scrollback_from_primary_repaints"),
            config.recover_scrollback_from_primary_repaints},
        {QStringLiteral("selection_viewport_projection_enabled"),
            config.selection_viewport_projection_enabled},
        {QStringLiteral("scrollback_limit"),           effective_scrollback_limit},
        {QStringLiteral("effective_scrollback_limit"), effective_scrollback_limit},
        {QStringLiteral("retained_history_capacity_bytes"),
            static_cast<qint64>(config.retained_history_capacity_bytes)},
        {QStringLiteral("synchronized_output_scroll_policy"),
            synchronized_output_scroll_policy_name(config.synchronized_output_scroll_policy)},
    };
}

bool read_event_index(
    const QJsonObject& object,
    std::uint64_t*     out_event_index)
{
    const QJsonValue value = object.value(QStringLiteral("event_index"));
    if (!value.isDouble()) {
        return false;
    }

    const double number = value.toDouble(-1.0);
    if (number < 0.0 || std::floor(number) != number) {
        return false;
    }

    *out_event_index = static_cast<std::uint64_t>(number);
    return true;
}

bool fail_read(QString* out_error, const QString& message)
{
    if (out_error != nullptr) {
        *out_error = message;
    }
    return false;
}

bool read_integral_value(const QJsonValue& value, qint64* out_value)
{
    if (!value.isDouble()) {
        return false;
    }

    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < static_cast<double>(std::numeric_limits<qint64>::min()) ||
        number > static_cast<double>(std::numeric_limits<qint64>::max()))
    {
        return false;
    }

    *out_value = static_cast<qint64>(number);
    return true;
}

bool require_integral_field(
    const QJsonObject& object,
    const QString&     field_name,
    int                line_number,
    QString*           out_error,
    qint64*            out_value = nullptr)
{
    qint64 value = 0;
    if (!read_integral_value(object.value(field_name), &value)) {
        return fail_read(
            out_error,
            QStringLiteral("transcript line %1 missing or invalid integer field %2")
                .arg(line_number)
                .arg(field_name));
    }

    if (out_value != nullptr) {
        *out_value = value;
    }
    return true;
}

bool require_nonnegative_integral_field(
    const QJsonObject& object,
    const QString&     field_name,
    int                line_number,
    QString*           out_error,
    qint64*            out_value = nullptr)
{
    qint64 value = 0;
    if (!require_integral_field(object, field_name, line_number, out_error, &value)) {
        return false;
    }
    if (value < 0) {
        return fail_read(
            out_error,
            QStringLiteral("transcript line %1 has negative integer field %2")
                .arg(line_number)
                .arg(field_name));
    }

    if (out_value != nullptr) {
        *out_value = value;
    }
    return true;
}

bool require_number_field(
    const QJsonObject& object,
    const QString&     field_name,
    int                line_number,
    QString*           out_error)
{
    if (!object.value(field_name).isDouble()) {
        return fail_read(
            out_error,
            QStringLiteral("transcript line %1 missing or invalid number field %2")
                .arg(line_number)
                .arg(field_name));
    }
    return true;
}

bool require_bool_field(
    const QJsonObject& object,
    const QString&     field_name,
    int                line_number,
    QString*           out_error)
{
    if (!object.value(field_name).isBool()) {
        return fail_read(
            out_error,
            QStringLiteral("transcript line %1 missing or invalid bool field %2")
                .arg(line_number)
                .arg(field_name));
    }
    return true;
}

bool require_string_field(
    const QJsonObject& object,
    const QString&     field_name,
    int                line_number,
    QString*           out_error,
    QString*           out_value = nullptr)
{
    const QJsonValue value = object.value(field_name);
    if (!value.isString()) {
        return fail_read(
            out_error,
            QStringLiteral("transcript line %1 missing or invalid string field %2")
                .arg(line_number)
                .arg(field_name));
    }

    if (out_value != nullptr) {
        *out_value = value.toString();
    }
    return true;
}

bool require_string_enum_field(
    const QJsonObject&             object,
    const QString&                 field_name,
    std::initializer_list<QString> allowed_values,
    int                            line_number,
    QString*                       out_error,
    QString*                       out_value = nullptr)
{
    QString value;
    if (!require_string_field(object, field_name, line_number, out_error, &value)) {
        return false;
    }

    for (const QString& allowed_value : allowed_values) {
        if (value == allowed_value) {
            if (out_value != nullptr) {
                *out_value = value;
            }
            return true;
        }
    }

    return fail_read(
        out_error,
        QStringLiteral("transcript line %1 has unsupported value %2 for field %3")
            .arg(line_number)
            .arg(value)
            .arg(field_name));
}

bool validate_optional_string_enum_field(
    const QJsonObject&             object,
    const QString&                 field_name,
    std::initializer_list<QString> allowed_values,
    int                            line_number,
    QString*                       out_error)
{
    if (!object.contains(field_name)) {
        return true;
    }

    return require_string_enum_field(
        object,
        field_name,
        allowed_values,
        line_number,
        out_error);
}

bool require_object_field(
    const QJsonObject& object,
    const QString&     field_name,
    int                line_number,
    QString*           out_error,
    QJsonObject*       out_value)
{
    const QJsonValue value = object.value(field_name);
    if (!value.isObject()) {
        return fail_read(
            out_error,
            QStringLiteral("transcript line %1 missing or invalid object field %2")
                .arg(line_number)
                .arg(field_name));
    }

    *out_value = value.toObject();
    return true;
}

bool require_array_field(
    const QJsonObject& object,
    const QString&     field_name,
    int                line_number,
    QString*           out_error,
    QJsonArray*        out_value)
{
    const QJsonValue value = object.value(field_name);
    if (!value.isArray()) {
        return fail_read(
            out_error,
            QStringLiteral("transcript line %1 missing or invalid array field %2")
                .arg(line_number)
                .arg(field_name));
    }

    *out_value = value.toArray();
    return true;
}

bool require_nonnegative_int_field(
    const QJsonObject& object,
    const QString&     field_name,
    int                line_number,
    QString*           out_error,
    int*               out_value = nullptr)
{
    qint64 value = 0;
    if (!require_nonnegative_integral_field(object, field_name, line_number, out_error, &value)) {
        return false;
    }
    if (value > std::numeric_limits<int>::max()) {
        return fail_read(
            out_error,
            QStringLiteral("transcript line %1 integer field %2 exceeds int range")
                .arg(line_number)
                .arg(field_name));
    }

    if (out_value != nullptr) {
        *out_value = static_cast<int>(value);
    }
    return true;
}

bool validate_grid_size_object(
    const QJsonObject& object,
    const QString&     field_name,
    int                line_number,
    QString*           out_error)
{
    QJsonObject grid_size;
    return
        require_object_field(object, field_name, line_number, out_error, &grid_size) &&
        require_nonnegative_int_field(grid_size, QStringLiteral("rows"), line_number, out_error) &&
        require_nonnegative_int_field(grid_size, QStringLiteral("columns"), line_number, out_error);
}

bool validate_position_object(
    const QJsonObject& object,
    const QString&     field_name,
    int                line_number,
    QString*           out_error)
{
    QJsonObject position;
    return
        require_object_field(object, field_name, line_number, out_error, &position) &&
        require_nonnegative_int_field(position, QStringLiteral("row"), line_number, out_error) &&
        require_nonnegative_int_field(position, QStringLiteral("column"), line_number, out_error);
}

bool validate_optional_position_object(
    const QJsonObject& object,
    const QString&     field_name,
    int                line_number,
    QString*           out_error)
{
    if (!object.contains(field_name)) {
        return true;
    }

    return validate_position_object(object, field_name, line_number, out_error);
}

bool validate_selection_range_object(
    const QJsonObject& object,
    const QString&     field_name,
    int                line_number,
    QString*           out_error)
{
    QJsonObject range;
    QString mode;
    if (!require_object_field(object, field_name, line_number, out_error, &range) ||
        !validate_position_object(range, QStringLiteral("start"), line_number, out_error) ||
        !validate_position_object(range, QStringLiteral("end"), line_number, out_error) ||
        !require_string_enum_field(
            range,
            QStringLiteral("mode"),
            {
                QStringLiteral("none"),
                QStringLiteral("normal"),
                QStringLiteral("word"),
                QStringLiteral("line"),
            },
            line_number,
            out_error,
            &mode))
    {
        return false;
    }

    if (mode == QStringLiteral("none")) {
        return fail_read(
            out_error,
            QStringLiteral("transcript line %1 selection range cannot use mode none")
                .arg(line_number));
    }

    return true;
}

bool validate_optional_selection_range_object(
    const QJsonObject& object,
    const QString&     field_name,
    int                line_number,
    QString*           out_error)
{
    if (!object.contains(field_name)) {
        return true;
    }

    return validate_selection_range_object(object, field_name, line_number, out_error);
}

bool validate_viewport_object(
    const QJsonObject& object,
    const QString&     field_name,
    int                line_number,
    QString*           out_error)
{
    QJsonObject viewport;
    int scrollback_rows  = 0;
    int visible_rows     = 0;
    int offset_from_tail = 0;
    if (!require_object_field(object, field_name, line_number, out_error, &viewport) ||
        !require_string_enum_field(
            viewport,
            QStringLiteral("active_buffer"),
            {QStringLiteral("primary"), QStringLiteral("alternate")},
            line_number,
            out_error) ||
        !require_nonnegative_int_field(
            viewport,
            QStringLiteral("scrollback_rows"),
            line_number,
            out_error,
            &scrollback_rows) ||
        !require_nonnegative_int_field(
            viewport,
            QStringLiteral("visible_rows"),
            line_number,
            out_error,
            &visible_rows) ||
        !require_nonnegative_int_field(
            viewport,
            QStringLiteral("offset_from_tail"),
            line_number,
            out_error,
            &offset_from_tail) ||
        !require_bool_field(viewport, QStringLiteral("follow_tail"), line_number, out_error) ||
        !require_string_enum_field(
            viewport,
            QStringLiteral("alternate_screen_scroll_policy"),
            {QStringLiteral("keep_at_tail"), QStringLiteral("wheel_to_terminal_input")},
            line_number,
            out_error))
    {
        return false;
    }

    if (visible_rows <= 0) {
        return fail_read(
            out_error,
            QStringLiteral("transcript line %1 viewport visible_rows must be positive")
                .arg(line_number));
    }
    if (offset_from_tail > scrollback_rows) {
        return fail_read(
            out_error,
            QStringLiteral("transcript line %1 viewport offset_from_tail exceeds scrollback_rows")
                .arg(line_number));
    }

    return true;
}

bool validate_optional_viewport_object(
    const QJsonObject& object,
    const QString&     field_name,
    int                line_number,
    QString*           out_error)
{
    if (!object.contains(field_name)) {
        return true;
    }

    return validate_viewport_object(object, field_name, line_number, out_error);
}

bool validate_optional_session_config_object(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    if (!object.contains(QStringLiteral("session_config"))) {
        return true;
    }

    QJsonObject config;
    if (!require_object_field(
            object,
            QStringLiteral("session_config"),
            line_number,
            out_error,
            &config))
    {
        return false;
    }

    if (config.contains(QStringLiteral("retained_history_capacity_bytes"))) {
        qint64 retained_history_capacity_bytes = 0;
        if (!require_nonnegative_integral_field(
                config,
                QStringLiteral("retained_history_capacity_bytes"),
                line_number,
                out_error,
                &retained_history_capacity_bytes))
        {
            return false;
        }
        const std::uint64_t retained_capacity =
            static_cast<std::uint64_t>(retained_history_capacity_bytes);
        if (retained_capacity < k_terminal_min_retained_history_capacity_bytes ||
            retained_capacity > k_terminal_max_retained_history_capacity_bytes)
        {
            return fail_read(
                out_error,
                QStringLiteral(
                    "transcript line %1 retained_history_capacity_bytes is outside [%2, %3]")
                    .arg(line_number)
                    .arg(k_terminal_min_retained_history_capacity_bytes)
                    .arg(k_terminal_max_retained_history_capacity_bytes));
        }
    }

    return
        (!config.contains(QStringLiteral("recover_scrollback_from_primary_repaints")) ||
            require_bool_field(
                config,
                QStringLiteral("recover_scrollback_from_primary_repaints"),
                line_number,
                out_error)) &&
        (!config.contains(QStringLiteral("selection_viewport_projection_enabled")) ||
            require_bool_field(
                config,
                QStringLiteral("selection_viewport_projection_enabled"),
                line_number,
                out_error)) &&
        require_nonnegative_int_field(
            config,
            QStringLiteral("scrollback_limit"),
            line_number,
            out_error) &&
        (!config.contains(QStringLiteral("effective_scrollback_limit")) ||
            require_nonnegative_int_field(
                config,
                QStringLiteral("effective_scrollback_limit"),
                line_number,
                out_error)) &&
        validate_optional_string_enum_field(
            config,
            QStringLiteral("synchronized_output_scroll_policy"),
            {
                QStringLiteral("DEFER_UNTIL_CONTENT_PUBLICATION"),
                QStringLiteral("IMMEDIATE_PUBLIC_PROJECTION"),
            },
            line_number,
            out_error);
}

bool validate_base64_payload(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    QString encoding;
    QString bytes_base64;
    qint64 byte_count = 0;
    if (!require_string_field(
            object,
            QStringLiteral("encoding"),
            line_number,
            out_error,
            &encoding) ||
        !require_string_field(
            object,
            QStringLiteral("bytes_base64"),
            line_number,
            out_error,
            &bytes_base64) ||
        !require_nonnegative_integral_field(
            object,
            QStringLiteral("byte_count"),
            line_number,
            out_error,
            &byte_count))
    {
        return false;
    }

    if (encoding != QStringLiteral("base64")) {
        return fail_read(
            out_error,
            QStringLiteral("transcript line %1 uses unsupported byte encoding %2")
                .arg(line_number)
                .arg(encoding));
    }

    const QByteArray::FromBase64Result decoded = QByteArray::fromBase64Encoding(
        bytes_base64.toLatin1(),
        QByteArray::AbortOnBase64DecodingErrors);
    if (decoded.decodingStatus != QByteArray::Base64DecodingStatus::Ok) {
        return fail_read(
            out_error,
            QStringLiteral("transcript line %1 contains invalid base64 payload")
                .arg(line_number));
    }

    if (decoded.decoded.size() != byte_count) {
        return fail_read(
            out_error,
            QStringLiteral("transcript line %1 byte_count does not match decoded payload")
                .arg(line_number));
    }

    return true;
}

bool validate_launch_argv(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    const QJsonValue value = object.value(QStringLiteral("argv"));
    if (!value.isArray()) {
        return fail_read(
            out_error,
            QStringLiteral("transcript line %1 missing or invalid argv array")
                .arg(line_number));
    }

    const QJsonArray argv = value.toArray();
    if (argv.isEmpty()) {
        return fail_read(
            out_error,
            QStringLiteral("transcript line %1 argv must not be empty")
                .arg(line_number));
    }

    bool first_argument = true;
    for (const QJsonValue& argument : argv) {
        if (!argument.isString()) {
            return fail_read(
                out_error,
                QStringLiteral("transcript line %1 argv contains a non-string value")
                    .arg(line_number));
        }
        if (first_argument && argument.toString().trimmed().isEmpty()) {
            return fail_read(
                out_error,
                QStringLiteral("transcript line %1 argv first value must not be empty")
                    .arg(line_number));
        }
        first_argument = false;
    }

    return true;
}

bool validate_resize_request_fields(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    return
        require_nonnegative_integral_field(
            object,
            QStringLiteral("transaction_id"),
            line_number,
            out_error) &&
        validate_grid_size_object(
            object,
            QStringLiteral("target_grid_size"),
            line_number,
            out_error) &&
        require_number_field(object, QStringLiteral("source_width"), line_number, out_error) &&
        require_number_field(object, QStringLiteral("source_height"), line_number, out_error) &&
        require_string_enum_field(
            object,
            QStringLiteral("active_buffer"),
            {QStringLiteral("primary"), QStringLiteral("alternate")},
            line_number,
            out_error);
}

bool validate_dirty_row_ranges_array(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    QJsonArray ranges;
    if (!require_array_field(object, QStringLiteral("dirty_row_ranges"), line_number, out_error, &ranges)) {
        return false;
    }

    for (const QJsonValue& value : ranges) {
        if (!value.isObject()) {
            return fail_read(
                out_error,
                QStringLiteral("transcript line %1 dirty_row_ranges contains a non-object value")
                    .arg(line_number));
        }

        const QJsonObject range = value.toObject();
        if (!require_nonnegative_int_field(range, QStringLiteral("first_row"), line_number, out_error) ||
            !require_nonnegative_int_field(range, QStringLiteral("row_count"), line_number, out_error))
        {
            return false;
        }
    }

    return true;
}

bool validate_visible_rows_array(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    QJsonArray rows;
    if (!require_array_field(object, QStringLiteral("visible_rows"), line_number, out_error, &rows)) {
        return false;
    }

    for (const QJsonValue& value : rows) {
        if (!value.isObject()) {
            return fail_read(
                out_error,
                QStringLiteral("transcript line %1 visible_rows contains a non-object value")
                    .arg(line_number));
        }

        const QJsonObject row = value.toObject();
        if (!require_nonnegative_int_field(row, QStringLiteral("row"), line_number, out_error) ||
            !require_string_field(row, QStringLiteral("text"), line_number, out_error) ||
            !require_string_field(row, QStringLiteral("hash64"), line_number, out_error))
        {
            return false;
        }
    }

    return true;
}

bool validate_retained_line_provenance_source_name(
    const QJsonObject& row,
    int                line_number,
    QString*           out_error)
{
    if (!row.contains(QStringLiteral("source"))) {
        return true;
    }

    QString source;
    if (!require_string_field(row, QStringLiteral("source"), line_number, out_error, &source)) {
        return false;
    }

    if (source == QStringLiteral("terminal_storage") ||
        source == QStringLiteral("recovered_primary_repaint"))
    {
        return true;
    }

    return fail_read(
        out_error,
        QStringLiteral("transcript line %1 row_provenance source is invalid")
            .arg(line_number));
}

bool validate_row_provenance_array(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    QJsonArray rows;
    if (!require_array_field(object, QStringLiteral("row_provenance"), line_number, out_error, &rows)) {
        return false;
    }

    for (const QJsonValue& value : rows) {
        if (!value.isObject()) {
            return fail_read(
                out_error,
                QStringLiteral("transcript line %1 row_provenance contains a non-object value")
                    .arg(line_number));
        }

        const QJsonObject row = value.toObject();
        if (!require_nonnegative_int_field(row, QStringLiteral("row"), line_number, out_error) ||
            !require_integral_field(row, QStringLiteral("logical_row"), line_number, out_error) ||
            !require_string_field(row, QStringLiteral("retained_line_id"), line_number, out_error) ||
            !require_string_field(row, QStringLiteral("content_generation"), line_number, out_error) ||
            !validate_retained_line_provenance_source_name(row, line_number, out_error))
        {
            return false;
        }
    }

    return true;
}

bool validate_selection_spans_array(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    QJsonArray spans;
    if (!require_array_field(object, QStringLiteral("selection_spans"), line_number, out_error, &spans)) {
        return false;
    }

    for (const QJsonValue& value : spans) {
        if (!value.isObject()) {
            return fail_read(
                out_error,
                QStringLiteral("transcript line %1 selection_spans contains a non-object value")
                    .arg(line_number));
        }

        const QJsonObject span = value.toObject();
        if (!require_nonnegative_int_field(span, QStringLiteral("row"), line_number, out_error) ||
            !require_nonnegative_int_field(span, QStringLiteral("first_column"), line_number, out_error) ||
            !require_nonnegative_int_field(span, QStringLiteral("column_count"), line_number, out_error) ||
            !validate_selection_range_object(
                span,
                QStringLiteral("source_range"),
                line_number,
                out_error))
        {
            return false;
        }
    }

    return true;
}

bool validate_selected_text_object(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    QJsonObject selected_text;
    if (!require_object_field(
            object,
            QStringLiteral("selected_text"),
            line_number,
            out_error,
            &selected_text))
    {
        return false;
    }

    const bool available = selected_text.value(QStringLiteral("available")).toBool(false);
    if (!require_bool_field(selected_text, QStringLiteral("available"), line_number, out_error) ||
        !require_string_field(selected_text, QStringLiteral("result"), line_number, out_error))
    {
        return false;
    }
    if (selected_text.contains(QStringLiteral("source_range")) &&
        !validate_selection_range_object(
            selected_text,
            QStringLiteral("source_range"),
            line_number,
            out_error))
    {
        return false;
    }
    if (available &&
        (!require_string_field(selected_text, QStringLiteral("text"), line_number, out_error) ||
            !require_string_field(selected_text, QStringLiteral("hash64"), line_number, out_error)))
    {
        return false;
    }

    return true;
}

bool validate_optional_public_scroll_policy_fields(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    return
        validate_optional_string_enum_field(
            object,
            QStringLiteral("effective_synchronized_output_scroll_policy"),
            {
                QStringLiteral("DEFER_UNTIL_CONTENT_PUBLICATION"),
                QStringLiteral("IMMEDIATE_PUBLIC_PROJECTION"),
            },
            line_number,
            out_error) &&
        validate_optional_string_enum_field(
            object,
            QStringLiteral("synchronized_output_scroll_policy_change_event"),
            {
                QStringLiteral("none"),
                QStringLiteral("changed_mid_hold"),
            },
            line_number,
            out_error) &&
        validate_optional_string_enum_field(
            object,
            QStringLiteral("diagnostic_reason"),
            {
                QStringLiteral("none"),
                QStringLiteral("synchronized_output_scroll_policy_changed_mid_hold"),
                QStringLiteral("public_projection_geometry_invalidated"),
                QStringLiteral("public_projection_memory_pressure_invalidated"),
                QStringLiteral("public_projection_invalidated_deferred_intent"),
                QStringLiteral("public_projection_scroll_publication_failed"),
                QStringLiteral("detached_anchor_content_generation_changed"),
                QStringLiteral("detached_anchor_geometry_changed"),
                QStringLiteral("detached_anchor_not_retained"),
                QStringLiteral("selection_public_projection_unsupported"),
                QStringLiteral("screen_buffer_epoch_changed"),
                QStringLiteral("buffer_transition_released"),
            },
            line_number,
            out_error);
}

bool validate_optional_snapshot_public_scroll_fields(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    return
        validate_optional_string_enum_field(
            object,
            QStringLiteral("snapshot_basis"),
            {
                QStringLiteral("LIVE_CONTENT"),
                QStringLiteral("PUBLIC_PROJECTION"),
            },
            line_number,
            out_error) &&
        validate_optional_string_enum_field(
            object,
            QStringLiteral("snapshot_purpose"),
            {
                QStringLiteral("CONTENT"),
                QStringLiteral("SELECTION_DERIVED"),
                QStringLiteral("GEOMETRY_DERIVED"),
                QStringLiteral("SCROLL"),
            },
            line_number,
            out_error) &&
        validate_optional_public_scroll_policy_fields(object, line_number, out_error) &&
        (!object.contains(QStringLiteral("public_projection_generation")) ||
            require_nonnegative_integral_field(
                object,
                QStringLiteral("public_projection_generation"),
                line_number,
                out_error)) &&
        validate_optional_viewport_object(
            object,
            QStringLiteral("public_viewport_before"),
            line_number,
            out_error) &&
        validate_optional_viewport_object(
            object,
            QStringLiteral("public_viewport_after"),
            line_number,
            out_error) &&
        validate_optional_viewport_object(
            object,
            QStringLiteral("live_viewport_before_on_release"),
            line_number,
            out_error) &&
        validate_optional_viewport_object(
            object,
            QStringLiteral("live_viewport_after_on_release"),
            line_number,
            out_error) &&
        (!object.contains(QStringLiteral("visible_scroll_applied")) ||
            require_bool_field(
                object,
                QStringLiteral("visible_scroll_applied"),
                line_number,
                out_error)) &&
        (!object.contains(QStringLiteral("live_content_publication_blocked")) ||
            require_bool_field(
                object,
                QStringLiteral("live_content_publication_blocked"),
                line_number,
                out_error)) &&
        validate_optional_string_enum_field(
            object,
            QStringLiteral("release_reconciliation_result"),
            {
                QStringLiteral("none"),
                QStringLiteral("sticky_tail"),
                QStringLiteral("exact_anchor"),
                QStringLiteral("retained_id_best_effort"),
                QStringLiteral("nearest_successor"),
                QStringLiteral("nearest_predecessor"),
                QStringLiteral("oldest_available_live"),
                QStringLiteral("deferred_offset"),
                QStringLiteral("incompatible_buffer"),
            },
            line_number,
            out_error) &&
        validate_optional_string_enum_field(
            object,
            QStringLiteral("hidden_row_eligibility"),
            {
                QStringLiteral("not_evaluated"),
                QStringLiteral("eligible"),
                QStringLiteral("ineligible"),
            },
            line_number,
            out_error) &&
        validate_optional_string_enum_field(
            object,
            QStringLiteral("hidden_row_clamp_reason"),
            {
                QStringLiteral("none"),
                QStringLiteral("public_viewport_boundary"),
                QStringLiteral("live_viewport_boundary"),
            },
            line_number,
            out_error) &&
        validate_optional_string_enum_field(
            object,
            QStringLiteral("public_projection_disable_reason"),
            {
                QStringLiteral("none"),
                QStringLiteral("geometry_invalidated"),
                QStringLiteral("memory_pressure"),
                QStringLiteral("projection_invalidated"),
                QStringLiteral("unsupported_buffer"),
            },
            line_number,
            out_error);
}

bool validate_snapshot_basis_purpose_biconditional(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    const QString basis = object.value(QStringLiteral("snapshot_basis")).toString(
        render_snapshot_basis_name(Terminal_render_snapshot_basis::LIVE_CONTENT));
    const QString purpose = object.value(QStringLiteral("snapshot_purpose")).toString(
        legacy_snapshot_purpose_name(object));

    const bool public_projection_basis = basis == QStringLiteral("PUBLIC_PROJECTION");
    const bool scroll_purpose          = purpose == QStringLiteral("SCROLL");
    if (public_projection_basis != scroll_purpose) {
        return fail_read(
            out_error,
            QStringLiteral(
                "transcript line %1 has incompatible snapshot_basis %2 and snapshot_purpose %3")
                .arg(line_number)
                .arg(basis)
                .arg(purpose));
    }

    return true;
}

// Per-kind transcript event validation.
//
// The validator is structured as a data-driven schema table: each event kind
// maps to a descriptor that lists the leading run of required scalar fields
// (string / non-negative integer / bool) shared with the composable field
// helpers, plus an optional named predicate for the irregular per-kind logic.
//
// The leading-field list captures only the contiguous run of plain required
// scalar fields at the start of a kind's validation, in source order. The
// helpers short-circuit on the first failure, so running the descriptor's
// fields in order before the predicate reproduces the exact accept/reject
// decision and error string of a hand-written `&&` chain. Anything that does
// not fit that plain shape - enums with a value set, signed integers, numbers,
// nested objects, optional fields, base64/argv/resize/viewport composites, and
// cross-field rules such as the basis/purpose biconditional or the shared
// backend.output/host.write payload - stays in the named predicate, which
// continues exactly where the leading-field run left off.

enum class Event_field_kind
{
    STRING,
    NONNEGATIVE_INTEGRAL,
    BOOL,
};

struct event_field_t {
    Event_field_kind kind;
    const char*      name;
};

using Event_post_validator = bool (*)(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error);

struct Event_schema
{
    const char*                    kind;
    std::initializer_list<event_field_t> leading_fields;
    Event_post_validator           post_validate;
};

bool validate_leading_event_fields(
    const QJsonObject&                          object,
    std::initializer_list<event_field_t>        fields,
    int                                         line_number,
    QString*                                    out_error)
{
    for (const event_field_t& field : fields) {
        const QString name = QString::fromLatin1(field.name);
        bool ok = true;
        switch (field.kind) {
            case Event_field_kind::STRING:
                ok = require_string_field(object, name, line_number, out_error);
                break;
            case Event_field_kind::NONNEGATIVE_INTEGRAL:
                ok = require_nonnegative_integral_field(object, name, line_number, out_error);
                break;
            case Event_field_kind::BOOL:
                ok = require_bool_field(object, name, line_number, out_error);
                break;
            default:
                ok = false;
                break;
        }
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool post_validate_header(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    // schema, schema_version, byte_encoding are read again here for their
    // bespoke value checks; the leading-field run already proved their types.
    const QString schema        = object.value(QStringLiteral("schema")).toString();
    const qint64 schema_version = object.value(QStringLiteral("schema_version")).toInteger();
    const QString byte_encoding = object.value(QStringLiteral("byte_encoding")).toString();

    if (object.contains(QStringLiteral("timing_diagnostics")) &&
        !require_bool_field(object, QStringLiteral("timing_diagnostics"), line_number, out_error))
    {
        return false;
    }

    if (schema != QStringLiteral("vnm_terminal.session_surface_transcript")) {
        return fail_read(
            out_error,
            QStringLiteral("transcript line %1 has unsupported schema %2")
                .arg(line_number)
                .arg(schema));
    }
    if (schema_version != k_transcript_schema_version) {
        return fail_read(
            out_error,
            QStringLiteral("transcript line %1 has unsupported schema_version %2")
                .arg(line_number)
                .arg(schema_version));
    }
    if (byte_encoding != QStringLiteral("base64")) {
        return fail_read(
            out_error,
            QStringLiteral("transcript line %1 has unsupported byte_encoding %2")
                .arg(line_number)
                .arg(byte_encoding));
    }
    return true;
}

bool post_validate_session_start(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    return
        validate_launch_argv(object, line_number, out_error) &&
        require_string_field(
            object,
            QStringLiteral("working_directory"),
            line_number,
            out_error) &&
        validate_optional_session_config_object(object, line_number, out_error) &&
        (!object.contains(QStringLiteral("initial_grid_size")) ||
            validate_grid_size_object(
                object,
                QStringLiteral("initial_grid_size"),
                line_number,
                out_error));
}

bool post_validate_backend_output(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    return validate_base64_payload(object, line_number, out_error);
}

bool post_validate_host_write(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    return
        validate_base64_payload(object, line_number, out_error) &&
        require_string_enum_field(
            object,
            QStringLiteral("source"),
            {
                QStringLiteral("user"),
                QStringLiteral("paste"),
                QStringLiteral("terminal_reply"),
            },
            line_number,
            out_error);
}

bool post_validate_session_resize_request(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    return validate_resize_request_fields(object, line_number, out_error);
}

bool post_validate_session_resize(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    return
        validate_resize_request_fields(object, line_number, out_error) &&
        validate_grid_size_object(
            object,
            QStringLiteral("snapshot_grid_size"),
            line_number,
            out_error) &&
        require_string_enum_field(
            object,
            QStringLiteral("model_result"),
            {
                QStringLiteral("applied"),
                QStringLiteral("invalid_grid_size"),
                QStringLiteral("not_applied"),
            },
            line_number,
            out_error) &&
        require_string_enum_field(
            object,
            QStringLiteral("backend_result"),
            {QStringLiteral("applied"), QStringLiteral("failed")},
            line_number,
            out_error) &&
        require_bool_field(
            object,
            QStringLiteral("backend_geometry_in_sync"),
            line_number,
            out_error);
}

bool post_validate_session_process_exit(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    return
        require_string_enum_field(
            object,
            QStringLiteral("reason"),
            {
                QStringLiteral("exited"),
                QStringLiteral("interrupted"),
                QStringLiteral("terminated"),
                QStringLiteral("failed_to_start"),
            },
            line_number,
            out_error) &&
        require_integral_field(object, QStringLiteral("exit_code"), line_number, out_error);
}

bool post_validate_session_backend_error(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    return
        require_string_enum_field(
            object,
            QStringLiteral("code"),
            {
                QStringLiteral("invalid_launch_config"),
                QStringLiteral("invalid_initial_grid_size"),
                QStringLiteral("working_directory_unavailable"),
                QStringLiteral("start_failed"),
                QStringLiteral("write_failed"),
                QStringLiteral("resize_failed"),
                QStringLiteral("interrupt_failed"),
                QStringLiteral("terminate_failed"),
                QStringLiteral("output_overflow"),
                QStringLiteral("callback_missing"),
                QStringLiteral("read_failed"),
            },
            line_number,
            out_error) &&
        require_string_field(object, QStringLiteral("message"), line_number, out_error);
}

bool post_validate_session_text_area_resize_request(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    return validate_grid_size_object(
        object,
        QStringLiteral("grid_size"),
        line_number,
        out_error);
}

bool post_validate_surface_scroll_intent(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    return
        require_integral_field(
            object,
            QStringLiteral("requested_line_delta"),
            line_number,
            out_error) &&
        validate_viewport_object(
            object,
            QStringLiteral("viewport_before"),
            line_number,
            out_error) &&
        (!object.contains(QStringLiteral("requested_offset_from_tail")) ||
            require_integral_field(
                object,
                QStringLiteral("requested_offset_from_tail"),
                line_number,
                out_error)) &&
        validate_optional_public_scroll_policy_fields(object, line_number, out_error);
}

bool post_validate_surface_scroll(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    return
        require_integral_field(
            object,
            QStringLiteral("requested_line_delta"),
            line_number,
            out_error) &&
        require_integral_field(
            object,
            QStringLiteral("applied_line_delta"),
            line_number,
            out_error) &&
        require_string_enum_field(
            object,
            QStringLiteral("action"),
            {
                QStringLiteral("viewport_moved"),
                QStringLiteral("at_boundary"),
                QStringLiteral("deferred_intent_recorded"),
                QStringLiteral("terminal_input"),
            },
            line_number,
            out_error) &&
        validate_viewport_object(
            object,
            QStringLiteral("viewport_before"),
            line_number,
            out_error) &&
        validate_viewport_object(
            object,
            QStringLiteral("viewport_after"),
            line_number,
            out_error) &&
        (!object.contains(QStringLiteral("requested_offset_from_tail")) ||
            require_integral_field(
                object,
                QStringLiteral("requested_offset_from_tail"),
                line_number,
                out_error)) &&
        (!object.contains(QStringLiteral("viewport")) ||
            validate_viewport_object(
                object,
                QStringLiteral("viewport"),
                line_number,
                out_error)) &&
        validate_optional_public_scroll_policy_fields(object, line_number, out_error);
}

bool post_validate_surface_wheel_trace(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    return
        require_integral_field(object, QStringLiteral("angle_delta_x"), line_number, out_error) &&
        require_integral_field(object, QStringLiteral("angle_delta_y"), line_number, out_error) &&
        require_integral_field(object, QStringLiteral("pixel_delta_x"), line_number, out_error) &&
        require_integral_field(object, QStringLiteral("pixel_delta_y"), line_number, out_error) &&
        require_integral_field(object, QStringLiteral("modifiers"), line_number, out_error) &&
        require_integral_field(object, QStringLiteral("wheel_steps"), line_number, out_error) &&
        require_integral_field(
            object,
            QStringLiteral("effective_line_delta"),
            line_number,
            out_error) &&
        require_number_field(object, QStringLiteral("angle_remainder"), line_number, out_error) &&
        require_number_field(object, QStringLiteral("pixel_remainder"), line_number, out_error) &&
        require_bool_field(object, QStringLiteral("session_present"), line_number, out_error) &&
        require_bool_field(
            object,
            QStringLiteral("render_publication_blocked"),
            line_number,
            out_error) &&
        require_bool_field(
            object,
            QStringLiteral("published_synchronized_output"),
            line_number,
            out_error) &&
        require_bool_field(object, QStringLiteral("alternate_screen"), line_number, out_error) &&
        require_bool_field(
            object,
            QStringLiteral("local_scroll_attempted"),
            line_number,
            out_error) &&
        require_bool_field(
            object,
            QStringLiteral("local_scroll_intent_recorded"),
            line_number,
            out_error) &&
        require_bool_field(
            object,
            QStringLiteral("local_scroll_applied"),
            line_number,
            out_error) &&
        require_bool_field(
            object,
            QStringLiteral("visible_scroll_applied"),
            line_number,
            out_error) &&
        require_bool_field(
            object,
            QStringLiteral("live_sgr_mouse_reporting"),
            line_number,
            out_error) &&
        require_bool_field(
            object,
            QStringLiteral("published_sgr_mouse_reporting"),
            line_number,
            out_error) &&
        require_bool_field(
            object,
            QStringLiteral("published_mouse_tracking"),
            line_number,
            out_error) &&
        require_nonnegative_integral_field(
            object,
            QStringLiteral("backend_drain_calls"),
            line_number,
            out_error) &&
        require_nonnegative_integral_field(
            object,
            QStringLiteral("backend_drain_elapsed_ns"),
            line_number,
            out_error) &&
        (!object.contains(QStringLiteral("local_scroll_block_reason")) ||
            require_string_field(
                object,
                QStringLiteral("local_scroll_block_reason"),
                line_number,
                out_error)) &&
        (!object.contains(QStringLiteral("scroll_action")) ||
            require_string_enum_field(
                object,
                QStringLiteral("scroll_action"),
                {
                    QStringLiteral("viewport_moved"),
                    QStringLiteral("at_boundary"),
                    QStringLiteral("deferred_intent_recorded"),
                    QStringLiteral("terminal_input"),
                },
                line_number,
                out_error)) &&
        (!object.contains(QStringLiteral("applied_line_delta")) ||
            require_integral_field(
                object,
                QStringLiteral("applied_line_delta"),
                line_number,
                out_error)) &&
        (!object.contains(QStringLiteral("published_viewport")) ||
            validate_viewport_object(
                object,
                QStringLiteral("published_viewport"),
                line_number,
                out_error)) &&
        (!object.contains(QStringLiteral("live_viewport")) ||
            validate_viewport_object(
                object,
                QStringLiteral("live_viewport"),
                line_number,
                out_error)) &&
        (!object.contains(QStringLiteral("viewport_before")) ||
            validate_viewport_object(
                object,
                QStringLiteral("viewport_before"),
                line_number,
                out_error)) &&
        (!object.contains(QStringLiteral("viewport_after")) ||
            validate_viewport_object(
                object,
                QStringLiteral("viewport_after"),
                line_number,
                out_error)) &&
        validate_optional_public_scroll_policy_fields(object, line_number, out_error);
}

bool post_validate_surface_wheel_ingress(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    return
        require_string_enum_field(
            object,
            QStringLiteral("phase"),
            {QStringLiteral("ingress")},
            line_number,
            out_error) &&
        require_bool_field(object, QStringLiteral("accepted_on_entry"), line_number, out_error) &&
        require_integral_field(object, QStringLiteral("angle_delta_x"), line_number, out_error) &&
        require_integral_field(object, QStringLiteral("angle_delta_y"), line_number, out_error) &&
        require_integral_field(object, QStringLiteral("pixel_delta_x"), line_number, out_error) &&
        require_integral_field(object, QStringLiteral("pixel_delta_y"), line_number, out_error) &&
        require_integral_field(object, QStringLiteral("modifiers"), line_number, out_error) &&
        require_number_field(object, QStringLiteral("position_x"), line_number, out_error) &&
        require_number_field(object, QStringLiteral("position_y"), line_number, out_error);
}

bool post_validate_transcript_timing(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    return
        require_string_enum_field(
            object,
            QStringLiteral("operation"),
            {
                QStringLiteral("event_write"),
                QStringLiteral("snapshot_diagnostic_emit"),
            },
            line_number,
            out_error) &&
        require_string_field(object, QStringLiteral("record_kind"), line_number, out_error) &&
        require_nonnegative_integral_field(
            object,
            QStringLiteral("record_event_index"),
            line_number,
            out_error) &&
        require_nonnegative_integral_field(
            object,
            QStringLiteral("object_construction_elapsed_ns"),
            line_number,
            out_error) &&
        require_nonnegative_integral_field(
            object,
            QStringLiteral("json_serialization_elapsed_ns"),
            line_number,
            out_error) &&
        require_nonnegative_integral_field(
            object,
            QStringLiteral("ndjson_append_elapsed_ns"),
            line_number,
            out_error) &&
        require_nonnegative_integral_field(
            object,
            QStringLiteral("file_write_elapsed_ns"),
            line_number,
            out_error) &&
        require_nonnegative_integral_field(
            object,
            QStringLiteral("file_flush_elapsed_ns"),
            line_number,
            out_error) &&
        require_nonnegative_integral_field(
            object,
            QStringLiteral("write_event_elapsed_ns"),
            line_number,
            out_error) &&
        require_nonnegative_integral_field(
            object,
            QStringLiteral("total_elapsed_ns"),
            line_number,
            out_error) &&
        require_nonnegative_integral_field(
            object,
            QStringLiteral("ndjson_byte_count"),
            line_number,
            out_error) &&
        (!object.contains(QStringLiteral("payload_byte_count")) ||
            require_nonnegative_integral_field(
                object,
                QStringLiteral("payload_byte_count"),
                line_number,
                out_error)) &&
        (!object.contains(QStringLiteral("source")) ||
            require_string_field(object, QStringLiteral("source"), line_number, out_error)) &&
        (!object.contains(QStringLiteral("route")) ||
            require_string_field(object, QStringLiteral("route"), line_number, out_error)) &&
        (!object.contains(QStringLiteral("outcome")) ||
            require_string_field(object, QStringLiteral("outcome"), line_number, out_error)) &&
        (!object.contains(QStringLiteral("snapshot_reason")) ||
            require_string_field(
                object,
                QStringLiteral("snapshot_reason"),
                line_number,
                out_error)) &&
        (!object.contains(QStringLiteral("snapshot_sequence")) ||
            require_nonnegative_integral_field(
                object,
                QStringLiteral("snapshot_sequence"),
                line_number,
                out_error)) &&
        (!object.contains(QStringLiteral("viewport_cell_count")) ||
            require_nonnegative_integral_field(
                object,
                QStringLiteral("viewport_cell_count"),
                line_number,
                out_error)) &&
        (!object.contains(QStringLiteral("snapshot_cell_count")) ||
            require_nonnegative_integral_field(
                object,
                QStringLiteral("snapshot_cell_count"),
                line_number,
                out_error));
}

bool post_validate_surface_selection_drag(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    return
        require_string_enum_field(
            object,
            QStringLiteral("phase"),
            {
                QStringLiteral("start"),
                QStringLiteral("update"),
                QStringLiteral("finish"),
                QStringLiteral("clear"),
                QStringLiteral("cancel"),
            },
            line_number,
            out_error) &&
        require_bool_field(object, QStringLiteral("moved"), line_number, out_error) &&
        validate_optional_position_object(
            object,
            QStringLiteral("anchor"),
            line_number,
            out_error) &&
        validate_optional_position_object(
            object,
            QStringLiteral("focus"),
            line_number,
            out_error) &&
        validate_optional_selection_range_object(
            object,
            QStringLiteral("range"),
            line_number,
            out_error);
}

bool post_validate_snapshot(
    const QJsonObject& object,
    int                line_number,
    QString*           out_error)
{
    QJsonObject cursor;
    return
        require_string_enum_field(
            object,
            QStringLiteral("mode"),
            {QStringLiteral("compact")},
            line_number,
            out_error) &&
        require_nonnegative_integral_field(
            object,
            QStringLiteral("session_sequence"),
            line_number,
            out_error) &&
        require_string_field(object, QStringLiteral("reason"), line_number, out_error) &&
        validate_grid_size_object(object, QStringLiteral("grid_size"), line_number, out_error) &&
        validate_viewport_object(object, QStringLiteral("viewport"), line_number, out_error) &&
        require_object_field(object, QStringLiteral("cursor"), line_number, out_error, &cursor) &&
        require_bool_field(cursor, QStringLiteral("visible"), line_number, out_error) &&
        validate_position_object(cursor, QStringLiteral("position"), line_number, out_error) &&
        require_nonnegative_integral_field(
            object,
            QStringLiteral("cell_count"),
            line_number,
            out_error) &&
        require_nonnegative_integral_field(
            object,
            QStringLiteral("dirty_row_range_count"),
            line_number,
            out_error) &&
        require_nonnegative_integral_field(
            object,
            QStringLiteral("selection_span_count"),
            line_number,
            out_error) &&
        require_bool_field(
            object,
            QStringLiteral("backend_geometry_in_sync"),
            line_number,
            out_error) &&
        (!object.contains(QStringLiteral("snapshot_sequence")) ||
            require_nonnegative_integral_field(
                object,
                QStringLiteral("snapshot_sequence"),
                line_number,
                out_error)) &&
        (!object.contains(QStringLiteral("row_origin_generation")) ||
            require_nonnegative_integral_field(
                object,
                QStringLiteral("row_origin_generation"),
                line_number,
                out_error)) &&
        validate_optional_snapshot_public_scroll_fields(object, line_number, out_error) &&
        validate_snapshot_basis_purpose_biconditional(object, line_number, out_error) &&
        validate_visible_rows_array(object, line_number, out_error) &&
        validate_row_provenance_array(object, line_number, out_error) &&
        validate_dirty_row_ranges_array(object, line_number, out_error) &&
        validate_selection_spans_array(object, line_number, out_error) &&
        validate_selected_text_object(object, line_number, out_error);
}

// Schema table: one descriptor per event kind. backend.output and host.write
// share the session_sequence leading field but diverge in their predicate, so
// they appear as two entries pointing at sibling predicates over a common base.
constexpr Event_field_kind k_str    = Event_field_kind::STRING;
constexpr Event_field_kind k_nnint  = Event_field_kind::NONNEGATIVE_INTEGRAL;
constexpr Event_field_kind k_bool   = Event_field_kind::BOOL;

const Event_schema k_event_schemas[] = {
    {
        "header",
        {
            {k_str,   "schema"},
            {k_nnint, "schema_version"},
            {k_str,   "byte_encoding"},
            {k_bool,  "snapshot_diagnostics"},
        },
        post_validate_header,
    },
    {
        "session.start",
        {{k_nnint, "session_sequence"}},
        post_validate_session_start,
    },
    {
        "backend.output",
        {{k_nnint, "session_sequence"}},
        post_validate_backend_output,
    },
    {
        "host.write",
        {{k_nnint, "session_sequence"}},
        post_validate_host_write,
    },
    {
        "session.resize_request",
        {{k_nnint, "session_sequence"}},
        post_validate_session_resize_request,
    },
    {
        "session.resize",
        {{k_nnint, "session_sequence"}},
        post_validate_session_resize,
    },
    {
        "session.process_exit",
        {{k_nnint, "session_sequence"}},
        post_validate_session_process_exit,
    },
    {
        "session.backend_error",
        {{k_nnint, "session_sequence"}},
        post_validate_session_backend_error,
    },
    {
        "session.text_area_resize_request",
        {{k_nnint, "session_sequence"}},
        post_validate_session_text_area_resize_request,
    },
    {
        "surface.scroll_intent",
        {{k_str, "source"}},
        post_validate_surface_scroll_intent,
    },
    {
        "surface.scroll",
        {{k_str, "source"}},
        post_validate_surface_scroll,
    },
    {
        "surface.wheel_trace",
        {
            {k_str,  "source"},
            {k_str,  "route"},
            {k_str,  "outcome"},
            {k_bool, "accepted"},
        },
        post_validate_surface_wheel_trace,
    },
    {
        "surface.wheel_ingress",
        {{k_str, "source"}},
        post_validate_surface_wheel_ingress,
    },
    {
        "transcript.timing",
        {},
        post_validate_transcript_timing,
    },
    {
        "surface.selection_drag",
        {},
        post_validate_surface_selection_drag,
    },
    {
        "snapshot",
        {},
        post_validate_snapshot,
    },
};

bool validate_event_object(
    const QJsonObject& object,
    const QString&     kind,
    int                line_number,
    QString*           out_error)
{
    for (const Event_schema& schema : k_event_schemas) {
        if (kind != QLatin1String(schema.kind)) {
            continue;
        }
        if (!validate_leading_event_fields(object, schema.leading_fields, line_number, out_error)) {
            return false;
        }
        return schema.post_validate(object, line_number, out_error);
    }

    return fail_read(
        out_error,
        QStringLiteral("unsupported transcript event kind %1 at line %2")
            .arg(kind)
            .arg(line_number));
}

}

struct Terminal_transcript_recorder::Timing_context
{
    QString       record_kind;
    QString       operation;
    QString       source;
    QString       route;
    QString       outcome;
    QString       snapshot_reason;
    qint64        payload_byte_count               = -1;
    qint64        object_construction_elapsed_ns   = 0;
    qint64        viewport_cell_count              = -1;
    qint64        snapshot_cell_count              = -1;
    std::uint64_t snapshot_sequence                = 0U;
    bool          has_snapshot_sequence            = false;
};

struct Terminal_transcript_recorder::Write_timing
{
    std::uint64_t record_event_index               = 0U;
    qint64        json_serialization_elapsed_ns    = 0;
    qint64        ndjson_append_elapsed_ns         = 0;
    qint64        file_write_elapsed_ns            = 0;
    qint64        file_flush_elapsed_ns            = 0;
    qint64        write_event_elapsed_ns           = 0;
    qint64        ndjson_byte_count                = 0;
};

bool timing_value_exceeds_threshold(qint64 elapsed_ns)
{
    return elapsed_ns >= k_transcript_timing_record_threshold_ns;
}

bool should_record_transcript_timing(
    const Terminal_transcript_recorder::Timing_context& context,
    const Terminal_transcript_recorder::Write_timing&   timing)
{
    const qint64 total_elapsed_ns =
        context.object_construction_elapsed_ns + timing.write_event_elapsed_ns;
    return
        timing_value_exceeds_threshold(context.object_construction_elapsed_ns) ||
        timing_value_exceeds_threshold(timing.json_serialization_elapsed_ns)  ||
        timing_value_exceeds_threshold(timing.ndjson_append_elapsed_ns)       ||
        timing_value_exceeds_threshold(timing.file_write_elapsed_ns)          ||
        timing_value_exceeds_threshold(timing.file_flush_elapsed_ns)          ||
        timing_value_exceeds_threshold(timing.write_event_elapsed_ns)         ||
        timing_value_exceeds_threshold(total_elapsed_ns);
}

QString timing_context_string(
    const QString&      context_value,
    const QJsonObject&  record_object,
    const QString&      field_name)
{
    if (!context_value.isEmpty()) {
        return context_value;
    }

    return record_object.value(field_name).toString();
}

QJsonObject transcript_timing_object(
    const Terminal_transcript_recorder::Timing_context& context,
    const Terminal_transcript_recorder::Write_timing&   timing,
    const QJsonObject&                                  record_object)
{
    const QString record_kind =
        context.record_kind.isEmpty()
            ? record_object.value(QStringLiteral("kind")).toString()
            : context.record_kind;
    const QString operation =
        !context.operation.isEmpty()
            ? context.operation
            : record_kind == QStringLiteral("snapshot")
                ? QStringLiteral("snapshot_diagnostic_emit")
                : QStringLiteral("event_write");
    const qint64 total_elapsed_ns =
        context.object_construction_elapsed_ns + timing.write_event_elapsed_ns;

    QJsonObject object;
    object.insert(QStringLiteral("kind"), QStringLiteral("transcript.timing"));
    object.insert(QStringLiteral("operation"), operation);
    object.insert(QStringLiteral("record_kind"), record_kind);
    insert_u64(object, QStringLiteral("record_event_index"), timing.record_event_index);
    object.insert(
        QStringLiteral("object_construction_elapsed_ns"),
        context.object_construction_elapsed_ns);
    object.insert(
        QStringLiteral("json_serialization_elapsed_ns"),
        timing.json_serialization_elapsed_ns);
    object.insert(QStringLiteral("ndjson_append_elapsed_ns"), timing.ndjson_append_elapsed_ns);
    object.insert(QStringLiteral("file_write_elapsed_ns"), timing.file_write_elapsed_ns);
    object.insert(QStringLiteral("file_flush_elapsed_ns"), timing.file_flush_elapsed_ns);
    object.insert(QStringLiteral("write_event_elapsed_ns"), timing.write_event_elapsed_ns);
    object.insert(QStringLiteral("total_elapsed_ns"), total_elapsed_ns);
    object.insert(QStringLiteral("ndjson_byte_count"), timing.ndjson_byte_count);

    if (context.payload_byte_count >= 0) {
        object.insert(QStringLiteral("payload_byte_count"), context.payload_byte_count);
    }
    else {
        const QJsonValue byte_count_value = record_object.value(QStringLiteral("byte_count"));
        if (byte_count_value.isDouble()) {
            const qint64 byte_count = static_cast<qint64>(byte_count_value.toDouble());
            if (byte_count >= 0) {
                object.insert(QStringLiteral("payload_byte_count"), byte_count);
            }
        }
    }

    const QString source = timing_context_string(
        context.source,
        record_object,
        QStringLiteral("source"));
    if (!source.isEmpty()) {
        object.insert(QStringLiteral("source"), source);
    }

    const QString route = timing_context_string(
        context.route,
        record_object,
        QStringLiteral("route"));
    if (!route.isEmpty()) {
        object.insert(QStringLiteral("route"), route);
    }

    const QString outcome = timing_context_string(
        context.outcome,
        record_object,
        QStringLiteral("outcome"));
    if (!outcome.isEmpty()) {
        object.insert(QStringLiteral("outcome"), outcome);
    }

    const QString snapshot_reason =
        timing_context_string(context.snapshot_reason, record_object, QStringLiteral("reason"));
    if (!snapshot_reason.isEmpty()) {
        object.insert(QStringLiteral("snapshot_reason"), snapshot_reason);
    }

    if (context.has_snapshot_sequence) {
        insert_u64(object, QStringLiteral("snapshot_sequence"), context.snapshot_sequence);
    }
    if (context.viewport_cell_count >= 0) {
        object.insert(QStringLiteral("viewport_cell_count"), context.viewport_cell_count);
    }
    if (context.snapshot_cell_count >= 0) {
        object.insert(QStringLiteral("snapshot_cell_count"), context.snapshot_cell_count);
    }

    return object;
}

Terminal_transcript_recorder::Terminal_transcript_recorder(
    QString path,
    bool    snapshot_diagnostics,
    bool    timing_diagnostics)
:
    m_path(std::move(path)),
    m_file(m_path),
    m_snapshot_diagnostics(snapshot_diagnostics),
    m_timing_diagnostics(timing_diagnostics)
{}

Terminal_transcript_recorder::~Terminal_transcript_recorder()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file.isOpen()) {
        m_file.flush();
        m_file.close();
    }
}

std::shared_ptr<Terminal_transcript_recorder> Terminal_transcript_recorder::create(
    const QString& path,
    bool           snapshot_diagnostics,
    bool           timing_diagnostics,
    QString*       out_error)
{
    auto recorder = std::shared_ptr<Terminal_transcript_recorder>(
        new Terminal_transcript_recorder(path, snapshot_diagnostics, timing_diagnostics));
    if (!recorder->open(out_error)) {
        return {};
    }

    if (!recorder->record_header()) {
        if (out_error != nullptr) {
            *out_error = recorder->error_message();
        }
        return {};
    }

    return recorder;
}

std::shared_ptr<Terminal_transcript_recorder> Terminal_transcript_recorder::create(
    const QString& path,
    bool           snapshot_diagnostics,
    QString*       out_error)
{
    return create(path, snapshot_diagnostics, false, out_error);
}

QString Terminal_transcript_recorder::path() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_path;
}

bool Terminal_transcript_recorder::snapshot_diagnostics_enabled() const
{
    return m_snapshot_diagnostics;
}

bool Terminal_transcript_recorder::timing_diagnostics_enabled() const
{
    return m_timing_diagnostics;
}

bool Terminal_transcript_recorder::failed() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_failed;
}

QString Terminal_transcript_recorder::error_message() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_error_message;
}

bool Terminal_transcript_recorder::record_session_start(
    std::uint64_t                  session_sequence,
    const Terminal_launch_config&  launch_config,
    const Terminal_session_config& session_config)
{
    QJsonObject object;
    object.insert(QStringLiteral("kind"), QStringLiteral("session.start"));
    insert_u64(object, QStringLiteral("session_sequence"), session_sequence);
    if (launch_config.initial_grid_size.has_value()) {
        object.insert(QStringLiteral("initial_grid_size"), grid_size_object(*launch_config.initial_grid_size));
    }
    object.insert(QStringLiteral("session_config"), session_config_object(session_config));
    object.insert(QStringLiteral("argv"), QJsonArray::fromStringList(launch_config.argv));
    object.insert(QStringLiteral("working_directory"), launch_config.working_directory);
    return write_event(std::move(object));
}

bool Terminal_transcript_recorder::record_backend_output(
    std::uint64_t  session_sequence,
    QByteArrayView bytes)
{
    Timing_context timing_context;
    QElapsedTimer construction_timer;
    if (m_timing_diagnostics) {
        construction_timer.start();
    }

    QJsonObject object;
    object.insert(QStringLiteral("kind"), QStringLiteral("backend.output"));
    insert_u64(object, QStringLiteral("session_sequence"), session_sequence);
    object.insert(QStringLiteral("encoding"), QStringLiteral("base64"));
    object.insert(QStringLiteral("bytes_base64"), bytes_base64(bytes));
    object.insert(QStringLiteral("byte_count"), static_cast<qint64>(bytes.size()));
    if (m_timing_diagnostics) {
        timing_context.record_kind                    = QStringLiteral("backend.output");
        timing_context.operation                      = QStringLiteral("event_write");
        timing_context.payload_byte_count             = static_cast<qint64>(bytes.size());
        timing_context.object_construction_elapsed_ns = construction_timer.nsecsElapsed();
    }
    return write_event(std::move(object), timing_context);
}

bool Terminal_transcript_recorder::record_host_write(
    std::uint64_t  session_sequence,
    const QString& source,
    QByteArrayView bytes)
{
    Timing_context timing_context;
    QElapsedTimer construction_timer;
    if (m_timing_diagnostics) {
        construction_timer.start();
    }

    QJsonObject object;
    object.insert(QStringLiteral("kind"), QStringLiteral("host.write"));
    insert_u64(object, QStringLiteral("session_sequence"), session_sequence);
    object.insert(QStringLiteral("source"), source);
    object.insert(QStringLiteral("encoding"), QStringLiteral("base64"));
    object.insert(QStringLiteral("bytes_base64"), bytes_base64(bytes));
    object.insert(QStringLiteral("byte_count"), static_cast<qint64>(bytes.size()));
    if (m_timing_diagnostics) {
        timing_context.record_kind                    = QStringLiteral("host.write");
        timing_context.operation                      = QStringLiteral("event_write");
        timing_context.source                         = source;
        timing_context.payload_byte_count             = static_cast<qint64>(bytes.size());
        timing_context.object_construction_elapsed_ns = construction_timer.nsecsElapsed();
    }
    return write_event(std::move(object), timing_context);
}

bool Terminal_transcript_recorder::record_session_resize(
    std::uint64_t                      session_sequence,
    const Terminal_resize_transaction& resize)
{
    QJsonObject object = resize_object(resize);
    object.insert(QStringLiteral("kind"), QStringLiteral("session.resize"));
    insert_u64(object, QStringLiteral("session_sequence"), session_sequence);
    return write_event(std::move(object));
}

bool Terminal_transcript_recorder::record_session_resize_request(
    std::uint64_t                      session_sequence,
    const Terminal_resize_transaction& resize)
{
    QJsonObject object = resize_request_object(resize);
    object.insert(QStringLiteral("kind"), QStringLiteral("session.resize_request"));
    insert_u64(object, QStringLiteral("session_sequence"), session_sequence);
    return write_event(std::move(object));
}

bool Terminal_transcript_recorder::record_text_area_resize_request(
    std::uint64_t        session_sequence,
    terminal_grid_size_t grid_size)
{
    QJsonObject object;
    object.insert(QStringLiteral("kind"), QStringLiteral("session.text_area_resize_request"));
    insert_u64(object, QStringLiteral("session_sequence"), session_sequence);
    object.insert(QStringLiteral("grid_size"), grid_size_object(grid_size));
    return write_event(std::move(object));
}

bool Terminal_transcript_recorder::record_session_process_exit(
    std::uint64_t                session_sequence,
    const Terminal_backend_exit& exit)
{
    QJsonObject object;
    object.insert(QStringLiteral("kind"), QStringLiteral("session.process_exit"));
    insert_u64(object, QStringLiteral("session_sequence"), session_sequence);
    object.insert(QStringLiteral("reason"), exit_reason_name(exit.reason));
    object.insert(QStringLiteral("exit_code"), exit.exit_code);
    return write_event(std::move(object));
}

bool Terminal_transcript_recorder::record_session_backend_error(
    std::uint64_t                 session_sequence,
    const Terminal_backend_error& error)
{
    QJsonObject object;
    object.insert(QStringLiteral("kind"), QStringLiteral("session.backend_error"));
    insert_u64(object, QStringLiteral("session_sequence"), session_sequence);
    object.insert(QStringLiteral("code"), backend_error_code_name(error.code));
    object.insert(QStringLiteral("message"), error.message);
    return write_event(std::move(object));
}

bool Terminal_transcript_recorder::record_surface_scroll(
    const Terminal_transcript_surface_scroll_event& event)
{
    QJsonObject object;
    object.insert(QStringLiteral("kind"), QStringLiteral("surface.scroll"));
    object.insert(QStringLiteral("source"), event.source);
    object.insert(QStringLiteral("requested_line_delta"), event.requested_line_delta);
    object.insert(QStringLiteral("applied_line_delta"), event.result.applied_line_delta);
    object.insert(QStringLiteral("action"), viewport_scroll_action_name(event.result.action));
    object.insert(QStringLiteral("viewport_before"), viewport_object(event.viewport_before));
    object.insert(QStringLiteral("viewport_after"), viewport_object(event.viewport_after));
    if (event.requested_offset_from_tail.has_value()) {
        object.insert(QStringLiteral("requested_offset_from_tail"), *event.requested_offset_from_tail);
    }
    return write_event(std::move(object));
}

bool Terminal_transcript_recorder::record_surface_scroll_intent(
    const Terminal_transcript_surface_scroll_intent_event& event)
{
    QJsonObject object;
    object.insert(QStringLiteral("kind"), QStringLiteral("surface.scroll_intent"));
    object.insert(QStringLiteral("source"), event.source);
    object.insert(QStringLiteral("requested_line_delta"), event.requested_line_delta);
    object.insert(QStringLiteral("viewport_before"), viewport_object(event.viewport_before));
    if (event.requested_offset_from_tail.has_value()) {
        object.insert(QStringLiteral("requested_offset_from_tail"), *event.requested_offset_from_tail);
    }
    return write_event(std::move(object));
}

bool Terminal_transcript_recorder::record_surface_wheel_trace(QJsonObject object)
{
    object.insert(QStringLiteral("kind"), QStringLiteral("surface.wheel_trace"));
    insert_public_scroll_policy_defaults(object);
    return write_event(std::move(object));
}

bool Terminal_transcript_recorder::record_surface_wheel_ingress(QJsonObject object)
{
    object.insert(QStringLiteral("kind"), QStringLiteral("surface.wheel_ingress"));
    return write_event(std::move(object));
}

bool Terminal_transcript_recorder::record_surface_selection_drag(
    const Terminal_transcript_surface_selection_drag_event& event)
{
    QJsonObject object;
    object.insert(QStringLiteral("kind"), QStringLiteral("surface.selection_drag"));
    object.insert(QStringLiteral("phase"), event.phase);
    object.insert(QStringLiteral("moved"), event.moved);
    if (event.anchor.has_value()) {
        object.insert(QStringLiteral("anchor"), position_object(*event.anchor));
    }
    if (event.focus.has_value()) {
        object.insert(QStringLiteral("focus"), position_object(*event.focus));
    }
    if (event.range.has_value()) {
        object.insert(QStringLiteral("range"), selection_range_object(*event.range));
    }
    return write_event(std::move(object));
}

bool Terminal_transcript_recorder::record_snapshot(
    std::uint64_t                   session_sequence,
    const QString&                  reason,
    const Terminal_render_snapshot& snapshot)
{
    if (!m_snapshot_diagnostics) {
        return true;
    }

    Timing_context timing_context;
    QElapsedTimer construction_timer;
    if (m_timing_diagnostics) {
        construction_timer.start();
    }

    QJsonObject cursor;
    cursor.insert(QStringLiteral("visible"), snapshot.cursor.visible);
    cursor.insert(QStringLiteral("position"), position_object(snapshot.cursor.position));

    QJsonObject object;
    object.insert(QStringLiteral("kind"), QStringLiteral("snapshot"));
    object.insert(QStringLiteral("mode"), QStringLiteral("compact"));
    insert_u64(object, QStringLiteral("session_sequence"), session_sequence);
    object.insert(QStringLiteral("reason"), reason);
    object.insert(QStringLiteral("grid_size"), grid_size_object(snapshot.grid_size));
    object.insert(QStringLiteral("viewport"), viewport_object(snapshot.viewport));
    object.insert(QStringLiteral("cursor"), cursor);
    insert_snapshot_public_scroll_fields(object, snapshot);
    insert_u64(object, QStringLiteral("snapshot_sequence"), snapshot.metadata.sequence);
    insert_u64(object, QStringLiteral("row_origin_generation"), snapshot.metadata.row_origin_generation);
    const Terminal_render_snapshot_row_content_view rows(snapshot);
    object.insert(QStringLiteral("cell_count"), static_cast<int>(rows.cell_count()));
    object.insert(QStringLiteral("dirty_row_range_count"), static_cast<int>(snapshot.dirty_row_ranges.size()));
    object.insert(QStringLiteral("selection_span_count"), static_cast<int>(snapshot.selection_spans.size()));
    object.insert(QStringLiteral("backend_geometry_in_sync"), snapshot.metadata.backend_geometry_in_sync);
    object.insert(QStringLiteral("visible_rows"), visible_rows_array(rows));
    object.insert(QStringLiteral("row_provenance"), row_provenance_array(snapshot));
    object.insert(QStringLiteral("dirty_row_ranges"), dirty_row_ranges_array(snapshot));
    object.insert(QStringLiteral("selection_spans"), selection_spans_array(snapshot));
    object.insert(QStringLiteral("selected_text"), selected_text_object(snapshot));
    if (m_timing_diagnostics) {
        timing_context.record_kind                    = QStringLiteral("snapshot");
        timing_context.operation                      = QStringLiteral("snapshot_diagnostic_emit");
        timing_context.snapshot_reason                = reason;
        timing_context.object_construction_elapsed_ns = construction_timer.nsecsElapsed();
        timing_context.viewport_cell_count =
            static_cast<qint64>(snapshot.grid_size.rows) *
            static_cast<qint64>(snapshot.grid_size.columns);
        timing_context.snapshot_cell_count   = static_cast<qint64>(rows.cell_count());
        timing_context.snapshot_sequence     = snapshot.metadata.sequence;
        timing_context.has_snapshot_sequence = true;
    }
    return write_event(std::move(object), timing_context);
}

bool Terminal_transcript_recorder::open(QString* out_error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_path.trimmed().isEmpty()) {
        m_failed        = true;
        m_error_message = QStringLiteral("transcript capture requires a non-empty path");
        if (out_error != nullptr) {
            *out_error = m_error_message;
        }
        return false;
    }

    const QFileInfo file_info(m_path);
    const QDir parent_dir = file_info.absoluteDir();
    if (!parent_dir.exists()) {
        m_failed = true;
        m_error_message = QStringLiteral("transcript capture parent directory does not exist: %1")
            .arg(parent_dir.absolutePath());
        if (out_error != nullptr) {
            *out_error = m_error_message;
        }
        return false;
    }

    m_path = file_info.absoluteFilePath();
    m_file.setFileName(m_path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_failed = true;
        m_error_message = QStringLiteral("transcript capture could not open %1: %2")
            .arg(m_path, m_file.errorString());
        if (out_error != nullptr) {
            *out_error = m_error_message;
        }
        return false;
    }

    return true;
}

bool Terminal_transcript_recorder::record_header()
{
    QJsonObject object;
    object.insert(QStringLiteral("kind"), QStringLiteral("header"));
    object.insert(QStringLiteral("schema"), QStringLiteral("vnm_terminal.session_surface_transcript"));
    object.insert(QStringLiteral("schema_version"), k_transcript_schema_version);
    object.insert(QStringLiteral("byte_encoding"), QStringLiteral("base64"));
    object.insert(QStringLiteral("snapshot_diagnostics"), m_snapshot_diagnostics);
    object.insert(QStringLiteral("timing_diagnostics"), m_timing_diagnostics);
    return write_event(std::move(object));
}

bool Terminal_transcript_recorder::write_event(QJsonObject object)
{
    Timing_context timing_context;
    return write_event(std::move(object), timing_context);
}

bool Terminal_transcript_recorder::write_event(
    QJsonObject           object,
    const Timing_context& timing_context)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_failed) {
        return false;
    }

    const QString kind = object.value(QStringLiteral("kind")).toString();
    if (!m_timing_diagnostics || kind == QStringLiteral("header")) {
        return write_event_locked(std::move(object), nullptr);
    }

    const QJsonObject record_object = object;
    Write_timing timing;
    if (!write_event_locked(std::move(object), &timing)) {
        return false;
    }

    if (!should_record_transcript_timing(timing_context, timing)) {
        return true;
    }

    return write_event_locked(
        transcript_timing_object(timing_context, timing, record_object),
        nullptr);
}

bool Terminal_transcript_recorder::write_event_locked(
    QJsonObject    object,
    Write_timing*  timing)
{
    QElapsedTimer total_timer;
    if (timing != nullptr) {
        total_timer.start();
        timing->record_event_index = m_next_event_index;
    }

    insert_u64(object, QStringLiteral("event_index"), m_next_event_index);
    ++m_next_event_index;

    QByteArray json;
    if (timing != nullptr) {
        QElapsedTimer timer;
        timer.start();
        json = QJsonDocument(object).toJson(QJsonDocument::Compact);
        timing->json_serialization_elapsed_ns = timer.nsecsElapsed();
    }
    else {
        json = QJsonDocument(object).toJson(QJsonDocument::Compact);
    }

    QByteArray line;
    if (timing != nullptr) {
        QElapsedTimer timer;
        timer.start();
        line = std::move(json);
        line += QByteArrayLiteral("\n");
        timing->ndjson_append_elapsed_ns = timer.nsecsElapsed();
        timing->ndjson_byte_count        = line.size();
    }
    else {
        line = std::move(json);
        line += QByteArrayLiteral("\n");
    }

    qint64 written = 0;
    if (timing != nullptr) {
        QElapsedTimer timer;
        timer.start();
        written = m_file.write(line);
        timing->file_write_elapsed_ns = timer.nsecsElapsed();
    }
    else {
        written = m_file.write(line);
    }

    if (written != line.size()) {
        m_failed = true;
        m_error_message = QStringLiteral("transcript capture write failed for %1: %2")
            .arg(m_path, m_file.errorString());
        return false;
    }

    // Transcript capture favors crash-adjacent repro fidelity over throughput.
    // Keep the per-event flush so the last causal event is usually on disk.
    bool flushed = false;
    if (timing != nullptr) {
        QElapsedTimer timer;
        timer.start();
        flushed = m_file.flush();
        timing->file_flush_elapsed_ns  = timer.nsecsElapsed();
        timing->write_event_elapsed_ns = total_timer.nsecsElapsed();
    }
    else {
        flushed = m_file.flush();
    }

    if (!flushed) {
        m_failed = true;
        m_error_message = QStringLiteral("transcript capture flush failed for %1: %2")
            .arg(m_path, m_file.errorString());
        return false;
    }

    return true;
}

std::optional<std::vector<Terminal_transcript_event>> read_terminal_transcript(
    const QString& path,
    QString*       out_error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (out_error != nullptr) {
            *out_error = QStringLiteral("could not read transcript %1: %2")
                .arg(QFileInfo(path).absoluteFilePath(), file.errorString());
        }
        return std::nullopt;
    }

    std::vector<Terminal_transcript_event> events;
    std::uint64_t expected_event_index = 0U;
    int line_number = 0;
    while (!file.atEnd()) {
        ++line_number;
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty()) {
            if (out_error != nullptr) {
                *out_error = QStringLiteral("empty transcript line at %1").arg(line_number);
            }
            return std::nullopt;
        }

        QJsonParseError parse_error;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
            if (out_error != nullptr) {
                *out_error = QStringLiteral("invalid transcript JSON at line %1: %2")
                    .arg(line_number)
                    .arg(parse_error.errorString());
            }
            return std::nullopt;
        }

        QJsonObject object = document.object();
        const QString kind = object.value(QStringLiteral("kind")).toString();
        std::uint64_t event_index = 0U;
        if (kind.isEmpty() || !read_event_index(object, &event_index)) {
            if (out_error != nullptr) {
                *out_error = QStringLiteral("transcript line %1 missing kind or event_index")
                    .arg(line_number);
            }
            return std::nullopt;
        }

        if (expected_event_index == 0U && kind != QStringLiteral("header")) {
            if (out_error != nullptr) {
                *out_error = QStringLiteral("transcript must begin with header event_index 0");
            }
            return std::nullopt;
        }
        if (expected_event_index > 0U && kind == QStringLiteral("header")) {
            if (out_error != nullptr) {
                *out_error = QStringLiteral("transcript line %1 contains a duplicate header")
                    .arg(line_number);
            }
            return std::nullopt;
        }

        if (event_index != expected_event_index) {
            if (out_error != nullptr) {
                *out_error = QStringLiteral(
                    "transcript event_index is not contiguous at line %1: expected %2, got %3")
                    .arg(line_number)
                    .arg(static_cast<qulonglong>(expected_event_index))
                    .arg(static_cast<qulonglong>(event_index));
            }
            return std::nullopt;
        }

        if (!validate_event_object(object, kind, line_number, out_error)) {
            return std::nullopt;
        }

        apply_transcript_compatibility_defaults(object, kind);
        ++expected_event_index;
        events.push_back({event_index, kind, object});
    }

    if (events.empty() || events.front().event_index != 0U ||
        events.front().kind != QStringLiteral("header"))
    {
        if (out_error != nullptr) {
            *out_error = QStringLiteral("transcript must begin with header event_index 0");
        }
        return std::nullopt;
    }

    return events;
}

}
