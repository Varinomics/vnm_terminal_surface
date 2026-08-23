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
#include <QTemporaryDir>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

constexpr int k_surface_default_scrollback_limit = 10000;
// A deadline-driven backend-output command is released in 4 KiB slices by
// Terminal_session while retaining one command sequence for its final slice.
constexpr qsizetype k_replay_backend_callback_slice_bytes = 4096;
constexpr std::uint64_t k_fnv1a64_basis = 14695981039346656037ULL;
constexpr std::uint64_t k_fnv1a64_prime = 1099511628211ULL;

QString usage_text()
{
    return QStringLiteral(
        "usage: vnm_terminal_transcript_replay [--strict-all-snapshots] <transcript.ndjson>\n"
        "\n"
        "  --strict-all-snapshots  validate causal ownership and compare every recorded\n"
        "                          semantic checkpoint inside its owning group (default)\n");
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

std::uint64_t fnv1a64_append(std::uint64_t hash, QByteArrayView bytes)
{
    for (char byte : bytes) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= k_fnv1a64_prime;
    }
    return hash;
}

std::uint64_t fnv1a64(QByteArrayView bytes)
{
    return fnv1a64_append(k_fnv1a64_basis, bytes);
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
    object.remove(QStringLiteral("snapshot_sequence"));
    object.remove(QStringLiteral("dirty_row_range_count"));
    object.remove(QStringLiteral("dirty_row_ranges"));
    return object;
}

struct Semantic_digest
{
    std::uint64_t hash64     = 0U;
    std::uint64_t byte_count = 0U;

    bool operator==(const Semantic_digest&) const = default;
};

struct Semantic_digest_hash
{
    std::size_t operator()(const Semantic_digest& digest) const noexcept
    {
        return static_cast<std::size_t>(
            digest.hash64 ^ (digest.byte_count + 0x9e3779b97f4a7c15ULL));
    }
};

Semantic_digest semantic_digest(const QJsonObject& comparable_object)
{
    const QByteArray canonical =
        QJsonDocument(comparable_object).toJson(QJsonDocument::Compact);
    return {
        fnv1a64(QByteArrayView(canonical)),
        static_cast<std::uint64_t>(canonical.size()),
    };
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

bool viewport_matches_object(
    const term::Terminal_viewport_state& viewport,
    const QJsonObject&                   object)
{
    const term::Terminal_viewport_state recorded = viewport_from_object(object);
    return
        viewport.active_buffer == recorded.active_buffer &&
        viewport.scrollback_rows == recorded.scrollback_rows &&
        viewport.visible_rows == recorded.visible_rows &&
        viewport.offset_from_tail == recorded.offset_from_tail &&
        viewport.follow_tail == recorded.follow_tail &&
        viewport.alternate_screen_scroll_policy ==
            recorded.alternate_screen_scroll_policy;
}

term::Terminal_viewport_state visible_viewport_state(
    const term::Terminal_session& session)
{
    const std::optional<term::Terminal_render_snapshot> snapshot =
        session.latest_render_snapshot();
    return snapshot.has_value() ? snapshot->viewport : session.viewport_state();
}

bool session_viewport_matches_object(
    const term::Terminal_session& session,
    const QJsonObject&            object)
{
    return viewport_matches_object(session.viewport_state(), object) ||
        viewport_matches_object(visible_viewport_state(session), object);
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
    int replayed_snapshot_events           = 0;
    int matching_snapshot_events           = 0;
    int divergent_snapshot_events          = 0;
    int dirty_mismatch_snapshot_events     = 0;
    int unpaired_recorded_dirty_snapshot_events = 0;
    int unpaired_replayed_dirty_snapshot_events = 0;
    int recorded_snapshot_runs             = 0;
    int replayed_snapshot_runs             = 0;
    int matching_snapshot_runs             = 0;
    int divergent_snapshot_runs            = 0;
    int surplus_replayed_snapshot_runs     = 0;
    int snapshot_alignment_comparison_work = 0;
    int semantic_digest_object_checks      = 0;
    int recorded_causal_groups             = 0;
    int replayed_causal_groups             = 0;
    int causal_driver_divergences          = 0;
    int causal_protocol_divergences        = 0;
    int public_projection_scroll_snapshot_events = 0;
    int semantic_selection_events          = 0;
    int surface_scroll_intents             = 0;
    std::optional<std::uint64_t> first_divergent_event_index;
    bool first_divergent_recorded_snapshot_missing = false;
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

void record_first_surplus_replayed_snapshot_divergence(
    Replay_result&                         replay,
    const term::Terminal_transcript_event& replayed)
{
    if (replay.first_divergent_event_index.has_value()) {
        return;
    }

    replay.first_divergent_event_index = replayed.event_index;
    replay.first_divergent_recorded_snapshot_missing = true;
    replay.first_divergent_recorded_selected_text_result = QStringLiteral("missing");
    replay.first_divergent_recorded_snapshot_sequence = std::nullopt;
    replay.first_divergent_replayed_snapshot_sequence =
        snapshot_sequence_from_object(replayed.object);
    replay.first_divergent_replayed_selected_text_result =
        selected_text_result_from_snapshot_object(replayed.object);
    replay.first_divergent_fields = {QStringLiteral("surplus_replayed_snapshot")};
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

bool snapshot_is_public_projection_scroll(const QJsonObject& object)
{
    return
        object.value(QStringLiteral("snapshot_basis")).toString() ==
            QStringLiteral("PUBLIC_PROJECTION") &&
        object.value(QStringLiteral("snapshot_purpose")).toString() ==
            QStringLiteral("SCROLL");
}

bool is_causal_driver_event(const term::Terminal_transcript_event& event)
{
    return
        event.kind != QStringLiteral("header") &&
        event.kind != QStringLiteral("snapshot") &&
        !is_replay_transparent_diagnostic_event(event.kind);
}

std::optional<std::uint64_t> event_session_sequence(
    const term::Terminal_transcript_event& event)
{
    const QJsonValue value = event.object.value(QStringLiteral("session_sequence"));
    if (!value.isDouble()) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(value.toDouble());
}

struct Snapshot_run
{
    Semantic_digest                                     digest;
    QJsonObject                                         comparable_object;
    std::vector<const term::Terminal_transcript_event*> publications;

    const term::Terminal_transcript_event& representative() const
    {
        return *publications.back();
    }
};

struct Causal_snapshot_group
{
    std::vector<Snapshot_run> runs;
};

struct Causal_event_signature
{
    QString                                           kind;
    Semantic_digest                                   digest;
    const term::Terminal_transcript_event*            event = nullptr;
};

struct Causal_group_signature
{
    QString                                           owner_kind;
    Semantic_digest                                   backend_output_digest{
        k_fnv1a64_basis,
        0U,
    };
    std::vector<const term::Terminal_transcript_event*> backend_output_events;
    std::vector<Causal_event_signature>                other_events;
    std::optional<Semantic_digest>                     host_resize_semantic_digest;
    const term::Terminal_transcript_event*             host_resize_result = nullptr;
};

struct Causal_driver_layout
{
    bool                                                    valid = true;
    QString                                                 error;
    std::vector<const term::Terminal_transcript_event*> drivers;
    std::vector<std::size_t>                                group_for_driver;
    std::vector<QString>                                    group_driver_kinds;
    std::vector<Causal_group_signature>                     group_signatures;
    std::vector<std::vector<const term::Terminal_transcript_event*>>
                                                            snapshots_by_group;
};

void invalidate_causal_layout(Causal_driver_layout& layout, QString error)
{
    if (layout.valid) {
        layout.valid = false;
        layout.error = std::move(error);
    }
}

bool optional_json_field_equal(
    const QJsonObject& left,
    const QJsonObject& right,
    const QString&     field)
{
    return left.contains(field) == right.contains(field) &&
        (!left.contains(field) || left.value(field) == right.value(field));
}

bool recorded_scroll_result_matches_intent(
    const term::Terminal_transcript_event& result,
    const term::Terminal_transcript_event& intent)
{
    return
        result.object.value(QStringLiteral("source")) ==
            intent.object.value(QStringLiteral("source")) &&
        result.object.value(QStringLiteral("requested_line_delta")) ==
            intent.object.value(QStringLiteral("requested_line_delta")) &&
        optional_json_field_equal(
            result.object,
            intent.object,
            QStringLiteral("requested_offset_from_tail")) &&
        result.object.value(QStringLiteral("viewport_before")) ==
            intent.object.value(QStringLiteral("viewport_before"));
}

bool known_sequence_event(const QString& kind)
{
    return
        kind == QStringLiteral("session.start")                    ||
        kind == QStringLiteral("backend.output")                   ||
        kind == QStringLiteral("host.write")                       ||
        kind == QStringLiteral("session.resize_request")           ||
        kind == QStringLiteral("session.resize")                   ||
        kind == QStringLiteral("session.backend_error")            ||
        kind == QStringLiteral("session.process_exit")             ||
        kind == QStringLiteral("session.text_area_resize_request");
}

struct Sequence_owner
{
    QString     kind;
    std::size_t group = 0U;
};

QJsonObject comparable_causal_event_object(QJsonObject object)
{
    object.remove(QStringLiteral("event_index"));
    object.remove(QStringLiteral("session_sequence"));
    return object;
}

QJsonObject host_resize_semantic_object(QJsonObject object)
{
    object = comparable_causal_event_object(std::move(object));
    object.insert(QStringLiteral("kind"), QStringLiteral("host.resize"));
    return object;
}

bool resize_result_matches_request(
    const term::Terminal_transcript_event& result,
    const term::Terminal_transcript_event& request)
{
    for (const QString& field : {
             QStringLiteral("transaction_id"),
             QStringLiteral("target_grid_size"),
             QStringLiteral("source_width"),
             QStringLiteral("source_height"),
             QStringLiteral("active_buffer")})
    {
        if (result.object.value(field) != request.object.value(field)) {
            return false;
        }
    }
    return true;
}

void append_causal_event_signature(
    Causal_driver_layout&                    layout,
    std::size_t                              group,
    const term::Terminal_transcript_event&   event)
{
    Causal_group_signature& signature = layout.group_signatures[group];
    if (event.kind == QStringLiteral("backend.output")) {
        const QByteArray bytes = event_bytes(event);
        signature.backend_output_digest.hash64 = fnv1a64_append(
            signature.backend_output_digest.hash64,
            QByteArrayView(bytes));
        signature.backend_output_digest.byte_count +=
            static_cast<std::uint64_t>(bytes.size());
        signature.backend_output_events.push_back(&event);
        return;
    }

    const QJsonObject comparable = comparable_causal_event_object(event.object);
    signature.other_events.push_back({
        event.kind,
        semantic_digest(comparable),
        &event,
    });
    if (event.kind == QStringLiteral("session.resize")) {
        const QJsonObject host_resize = host_resize_semantic_object(event.object);
        signature.host_resize_semantic_digest = semantic_digest(host_resize);
        signature.host_resize_result = &event;
    }
}

Causal_driver_layout validated_causal_driver_layout(
    const std::vector<term::Terminal_transcript_event>& events,
    bool require_dense_sequence_ownership = true)
{
    Causal_driver_layout layout;
    if (events.empty() || events.front().kind != QStringLiteral("header")) {
        invalidate_causal_layout(layout, QStringLiteral("transcript header is not first"));
        return layout;
    }

    const bool has_resize_request = std::any_of(
        events.begin(),
        events.end(),
        [](const term::Terminal_transcript_event& event) {
            return event.kind == QStringLiteral("session.resize_request");
        });
    std::map<std::uint64_t, Sequence_owner> sequence_owners;
    std::map<std::uint64_t, const term::Terminal_transcript_event*> resize_requests;
    std::optional<std::size_t> current_group;
    std::optional<std::uint64_t> open_backend_sequence;
    const term::Terminal_transcript_event* pending_scroll_intent = nullptr;
    bool started = false;

    const auto add_group = [
        &layout,
        &current_group,
        &open_backend_sequence](const QString& kind) {
        layout.group_driver_kinds.push_back(kind);
        layout.group_signatures.push_back({kind});
        layout.snapshots_by_group.emplace_back();
        current_group = layout.group_driver_kinds.size() - 1U;
        open_backend_sequence.reset();
        return *current_group;
    };

    for (std::size_t event_position = 0U;
         event_position < events.size() && layout.valid;
         ++event_position)
    {
        const term::Terminal_transcript_event& event = events[event_position];
        if (event.kind == QStringLiteral("header")) {
            if (event_position != 0U) {
                invalidate_causal_layout(
                    layout,
                    QStringLiteral("duplicate transcript header at event %1")
                        .arg(static_cast<qulonglong>(event.event_index)));
            }
            continue;
        }
        if (is_replay_transparent_diagnostic_event(event.kind)) {
            continue;
        }

        if (event.kind == QStringLiteral("snapshot")) {
            if (!current_group.has_value()) {
                invalidate_causal_layout(
                    layout,
                    QStringLiteral("snapshot has no causal owner at event %1")
                        .arg(static_cast<qulonglong>(event.event_index)));
                continue;
            }
            const std::optional<std::uint64_t> sequence = event_session_sequence(event);
            if (!sequence.has_value()) {
                invalidate_causal_layout(
                    layout,
                    QStringLiteral("snapshot has no session sequence at event %1")
                        .arg(static_cast<qulonglong>(event.event_index)));
                continue;
            }

            std::size_t snapshot_group = *current_group;
            const auto owner = sequence_owners.find(*sequence);
            if (owner != sequence_owners.end()) {
                snapshot_group = owner->second.group;
            }
            else {
                const QString& current_kind =
                    layout.group_driver_kinds[*current_group];
                if (current_kind != QStringLiteral("surface.scroll_intent") &&
                    current_kind != QStringLiteral("surface.selection_drag"))
                {
                    invalidate_causal_layout(
                        layout,
                        QStringLiteral("snapshot sequence has no command owner at event %1")
                            .arg(static_cast<qulonglong>(event.event_index)));
                    continue;
                }
                sequence_owners.emplace(
                    *sequence,
                    Sequence_owner{
                        QStringLiteral("surface.publication"),
                        snapshot_group,
                    });
            }
            layout.snapshots_by_group[snapshot_group].push_back(&event);
            continue;
        }

        if (!started && event.kind != QStringLiteral("session.start")) {
            invalidate_causal_layout(
                layout,
                QStringLiteral("causal event appeared before session.start at event %1")
                    .arg(static_cast<qulonglong>(event.event_index)));
            continue;
        }
        if (pending_scroll_intent != nullptr &&
            event.kind != QStringLiteral("surface.scroll"))
        {
            // Old captures may omit a result for an intent that did not move
            // the viewport. Such an intent cannot authorize a later result.
            pending_scroll_intent = nullptr;
        }

        std::optional<std::size_t> event_group;
        if (event.kind == QStringLiteral("session.start")) {
            if (started || !layout.drivers.empty()) {
                invalidate_causal_layout(
                    layout,
                    QStringLiteral("duplicate or reordered session.start at event %1")
                        .arg(static_cast<qulonglong>(event.event_index)));
                continue;
            }
            const std::optional<std::uint64_t> sequence = event_session_sequence(event);
            if (!sequence.has_value() || *sequence != 1U) {
                invalidate_causal_layout(
                    layout,
                    QStringLiteral("session.start does not own sequence 1"));
                continue;
            }
            event_group = add_group(event.kind);
            sequence_owners.emplace(
                *sequence,
                Sequence_owner{event.kind, *event_group});
            started = true;
        }
        else
        if (event.kind == QStringLiteral("surface.scroll_intent")) {
            if (pending_scroll_intent != nullptr) {
                invalidate_causal_layout(
                    layout,
                    QStringLiteral("duplicate surface.scroll_intent at event %1")
                        .arg(static_cast<qulonglong>(event.event_index)));
                continue;
            }
            event_group = add_group(event.kind);
            pending_scroll_intent = &event;
        }
        else
        if (event.kind == QStringLiteral("surface.scroll")) {
            if (pending_scroll_intent == nullptr || !current_group.has_value() ||
                !recorded_scroll_result_matches_intent(event, *pending_scroll_intent))
            {
                invalidate_causal_layout(
                    layout,
                    QStringLiteral("orphan or corrupt surface.scroll result at event %1")
                        .arg(static_cast<qulonglong>(event.event_index)));
                continue;
            }
            event_group = current_group;
            pending_scroll_intent = nullptr;
        }
        else
        if (event.kind == QStringLiteral("surface.selection_drag")) {
            event_group = add_group(event.kind);
        }
        else {
            if (!known_sequence_event(event.kind)) {
                invalidate_causal_layout(
                    layout,
                    QStringLiteral("unknown causal event kind %1 at event %2")
                        .arg(event.kind)
                        .arg(static_cast<qulonglong>(event.event_index)));
                continue;
            }
            const std::optional<std::uint64_t> sequence = event_session_sequence(event);
            if (!sequence.has_value()) {
                invalidate_causal_layout(
                    layout,
                    QStringLiteral("causal event has no session sequence at event %1")
                        .arg(static_cast<qulonglong>(event.event_index)));
                continue;
            }
            const auto owner = sequence_owners.find(*sequence);
            const bool resize_result =
                event.kind == QStringLiteral("session.resize") && has_resize_request;
            const bool text_area_resize_request =
                event.kind == QStringLiteral("session.text_area_resize_request");
            const bool terminal_reply =
                event.kind == QStringLiteral("host.write") &&
                event.object.value(QStringLiteral("source")).toString() ==
                    QStringLiteral("terminal_reply");
            if (owner != sequence_owners.end()) {
                const bool owner_is_current =
                    current_group.has_value() && *current_group == owner->second.group;
                const auto resize_request = resize_requests.find(*sequence);
                const bool backend_continuation =
                    event.kind == QStringLiteral("backend.output") &&
                    owner->second.kind == QStringLiteral("backend.output") &&
                    owner_is_current && open_backend_sequence == sequence;
                const bool matching_resize_result =
                    resize_result && owner_is_current &&
                    owner->second.kind == QStringLiteral("session.resize_request") &&
                    resize_request != resize_requests.end() &&
                    resize_result_matches_request(event, *resize_request->second);
                const bool matching_text_area_resize_request =
                    text_area_resize_request && owner_is_current &&
                    owner->second.kind == QStringLiteral("backend.output");
                const bool matching_backend_error =
                    event.kind == QStringLiteral("session.backend_error");
                if (!backend_continuation && !matching_resize_result &&
                    !matching_text_area_resize_request && !matching_backend_error)
                {
                    invalidate_causal_layout(
                        layout,
                        QStringLiteral(
                            "session sequence has an invalid continuation or derived owner at "
                            "event %1")
                            .arg(static_cast<qulonglong>(event.event_index)));
                    continue;
                }
                event_group = owner->second.group;
                current_group = event_group;
            }
            else
            if (resize_result || text_area_resize_request) {
                invalidate_causal_layout(
                    layout,
                    QStringLiteral("derived event has no session-sequence owner at event %1")
                        .arg(static_cast<qulonglong>(event.event_index)));
                continue;
            }
            else
            if (terminal_reply) {
                if (!current_group.has_value() ||
                    layout.group_driver_kinds[*current_group] !=
                        QStringLiteral("backend.output"))
                {
                    invalidate_causal_layout(
                        layout,
                        QStringLiteral("terminal reply has no backend causal owner at event %1")
                            .arg(static_cast<qulonglong>(event.event_index)));
                    continue;
                }
                event_group = current_group;
                sequence_owners.emplace(
                    *sequence,
                    Sequence_owner{
                        QStringLiteral("host.write.terminal_reply"),
                        *event_group,
                    });
            }
            else {
                event_group = add_group(event.kind);
                sequence_owners.emplace(
                    *sequence,
                    Sequence_owner{event.kind, *event_group});
            }
        }

        layout.drivers.push_back(&event);
        layout.group_for_driver.push_back(*event_group);
        append_causal_event_signature(layout, *event_group, event);
        if (event.kind == QStringLiteral("session.resize_request")) {
            resize_requests.emplace(*event_session_sequence(event), &event);
        }
        if (event.kind == QStringLiteral("backend.output")) {
            const std::optional<std::uint64_t> sequence = event_session_sequence(event);
            open_backend_sequence =
                sequence.has_value() &&
                    event_bytes(event).size() == k_replay_backend_callback_slice_bytes
                ? sequence
                : std::nullopt;
        }
        else
        if (layout.group_driver_kinds[*event_group] != QStringLiteral("backend.output") ||
            (event.kind != QStringLiteral("host.write") &&
             event.kind != QStringLiteral("session.text_area_resize_request")))
        {
            open_backend_sequence.reset();
        }
    }

    if (layout.valid && !started) {
        invalidate_causal_layout(layout, QStringLiteral("transcript contains no session.start"));
    }
    if (layout.valid && require_dense_sequence_ownership) {
        std::uint64_t expected_sequence = 1U;
        for (const auto& [sequence, owner] : sequence_owners) {
            (void)owner;
            if (sequence != expected_sequence) {
                invalidate_causal_layout(
                    layout,
                    QStringLiteral(
                        "session sequence ownership is split: expected %1, observed %2")
                        .arg(static_cast<qulonglong>(expected_sequence))
                        .arg(static_cast<qulonglong>(sequence)));
                break;
            }
            ++expected_sequence;
        }
    }
    return layout;
}

void append_snapshot_run(
    Causal_snapshot_group&                  group,
    const term::Terminal_transcript_event& event,
    Replay_result&                         replay)
{
    QJsonObject comparable = comparable_model_snapshot_object(event.object);
    const Semantic_digest digest = semantic_digest(comparable);
    if (!group.runs.empty() && group.runs.back().digest == digest) {
        ++replay.semantic_digest_object_checks;
        if (group.runs.back().comparable_object == comparable) {
            group.runs.back().publications.push_back(&event);
            return;
        }
    }
    group.runs.push_back({digest, std::move(comparable), {&event}});
}

std::vector<Causal_snapshot_group> grouped_snapshot_runs(
    const std::vector<std::vector<const term::Terminal_transcript_event*>>&
        snapshots_by_group,
    Replay_result& replay)
{
    std::vector<Causal_snapshot_group> groups(snapshots_by_group.size());
    for (std::size_t group_index = 0U;
         group_index < snapshots_by_group.size();
         ++group_index)
    {
        for (const term::Terminal_transcript_event* event :
             snapshots_by_group[group_index])
        {
            append_snapshot_run(groups[group_index], *event, replay);
        }
    }
    return groups;
}

bool backend_output_streams_equal(
    const std::vector<const term::Terminal_transcript_event*>& left,
    const std::vector<const term::Terminal_transcript_event*>& right)
{
    std::size_t left_index = 0U;
    std::size_t right_index = 0U;
    qsizetype left_offset = 0;
    qsizetype right_offset = 0;
    QByteArray left_bytes;
    QByteArray right_bytes;
    while (left_index < left.size() && right_index < right.size()) {
        if (left_offset == 0) {
            left_bytes = event_bytes(*left[left_index]);
        }
        if (right_offset == 0) {
            right_bytes = event_bytes(*right[right_index]);
        }
        const qsizetype count = std::min(
            left_bytes.size() - left_offset,
            right_bytes.size() - right_offset);
        if (left_bytes.sliced(left_offset, count) !=
            right_bytes.sliced(right_offset, count))
        {
            return false;
        }
        left_offset += count;
        right_offset += count;
        if (left_offset == left_bytes.size()) {
            ++left_index;
            left_offset = 0;
        }
        if (right_offset == right_bytes.size()) {
            ++right_index;
            right_offset = 0;
        }
    }
    return left_index == left.size() && right_index == right.size();
}

bool is_legacy_direct_resize_signature(const Causal_group_signature& signature)
{
    return
        signature.owner_kind == QStringLiteral("session.resize") &&
        signature.backend_output_events.empty() &&
        signature.other_events.size() == 1U &&
        signature.other_events.front().kind == QStringLiteral("session.resize") &&
        signature.host_resize_semantic_digest.has_value() &&
        signature.host_resize_result != nullptr;
}

bool is_modern_resize_pair_signature(const Causal_group_signature& signature)
{
    return
        signature.owner_kind == QStringLiteral("session.resize_request") &&
        signature.backend_output_events.empty() &&
        signature.other_events.size() == 2U &&
        signature.other_events[0].kind == QStringLiteral("session.resize_request") &&
        signature.other_events[1].kind == QStringLiteral("session.resize") &&
        signature.host_resize_semantic_digest.has_value() &&
        signature.host_resize_result != nullptr;
}

bool legacy_and_modern_resize_signatures_equal(
    const Causal_group_signature& left,
    const Causal_group_signature& right,
    Replay_result&                replay)
{
    const bool compatible_shapes =
        (is_legacy_direct_resize_signature(left) &&
         is_modern_resize_pair_signature(right)) ||
        (is_modern_resize_pair_signature(left) &&
         is_legacy_direct_resize_signature(right));
    if (!compatible_shapes ||
        left.host_resize_semantic_digest != right.host_resize_semantic_digest)
    {
        return false;
    }
    ++replay.semantic_digest_object_checks;
    return
        host_resize_semantic_object(left.host_resize_result->object) ==
        host_resize_semantic_object(right.host_resize_result->object);
}

bool causal_group_signatures_equal(
    const Causal_group_signature& recorded,
    const Causal_group_signature& replayed,
    Replay_result&                replay)
{
    if (recorded.owner_kind != replayed.owner_kind) {
        return legacy_and_modern_resize_signatures_equal(recorded, replayed, replay);
    }
    if (recorded.backend_output_digest != replayed.backend_output_digest ||
        recorded.other_events.size() != replayed.other_events.size())
    {
        return false;
    }
    if (!recorded.backend_output_events.empty()) {
        ++replay.semantic_digest_object_checks;
        if (!backend_output_streams_equal(
                recorded.backend_output_events,
                replayed.backend_output_events))
        {
            return false;
        }
    }
    for (std::size_t index = 0U; index < recorded.other_events.size(); ++index) {
        const Causal_event_signature& recorded_event = recorded.other_events[index];
        const Causal_event_signature& replayed_event = replayed.other_events[index];
        if (recorded_event.kind != replayed_event.kind ||
            recorded_event.digest != replayed_event.digest)
        {
            return false;
        }
        ++replay.semantic_digest_object_checks;
        if (comparable_causal_event_object(recorded_event.event->object) !=
            comparable_causal_event_object(replayed_event.event->object))
        {
            return false;
        }
    }
    return true;
}

bool semantic_runs_equal(
    const Snapshot_run& recorded,
    const Snapshot_run& replayed,
    Replay_result&      replay)
{
    if (recorded.digest != replayed.digest) {
        return false;
    }
    ++replay.semantic_digest_object_checks;
    return recorded.comparable_object == replayed.comparable_object;
}

void compare_dirty_publications(
    Replay_result&      replay,
    const Snapshot_run& recorded,
    const Snapshot_run& replayed)
{
    const std::size_t paired_count = std::min(
        recorded.publications.size(),
        replayed.publications.size());
    replay.unpaired_recorded_dirty_snapshot_events += static_cast<int>(
        recorded.publications.size() - paired_count);
    replay.unpaired_replayed_dirty_snapshot_events += static_cast<int>(
        replayed.publications.size() - paired_count);

    const std::size_t recorded_start = recorded.publications.size() - paired_count;
    const std::size_t replayed_start = replayed.publications.size() - paired_count;
    for (std::size_t pair_index = 0U; pair_index < paired_count; ++pair_index) {
        const term::Terminal_transcript_event& recorded_event =
            *recorded.publications[recorded_start + pair_index];
        const term::Terminal_transcript_event& replayed_event =
            *replayed.publications[replayed_start + pair_index];
        if (!dirty_snapshot_fields_differ(recorded_event.object, replayed_event.object)) {
            continue;
        }
        ++replay.dirty_mismatch_snapshot_events;
        record_first_dirty_mismatch(replay, recorded_event, replayed_event.object);
    }
}

void record_semantic_match(
    Replay_result&      replay,
    const Snapshot_run& recorded,
    const Snapshot_run& replayed)
{
    ++replay.matching_snapshot_runs;
    replay.matching_snapshot_events +=
        static_cast<int>(recorded.publications.size());
    if (snapshot_is_public_projection_scroll(recorded.representative().object)) {
        replay.public_projection_scroll_snapshot_events +=
            static_cast<int>(recorded.publications.size());
    }
    compare_dirty_publications(replay, recorded, replayed);
}

struct Semantic_positions
{
    QJsonObject              comparable_object;
    std::vector<std::size_t> positions;
    std::size_t              cursor = 0U;
};

using Semantic_position_index = std::unordered_map<
    Semantic_digest,
    std::vector<Semantic_positions>,
    Semantic_digest_hash>;

Semantic_position_index index_semantic_positions(
    const std::vector<Snapshot_run>& runs,
    std::size_t                      count,
    Replay_result&                   replay)
{
    Semantic_position_index index;
    index.reserve(count);
    for (std::size_t position = 0U; position < count; ++position) {
        const Snapshot_run& run = runs[position];
        std::vector<Semantic_positions>& classes = index[run.digest];
        auto semantic_class = classes.end();
        for (auto candidate = classes.begin(); candidate != classes.end(); ++candidate) {
            ++replay.semantic_digest_object_checks;
            if (candidate->comparable_object == run.comparable_object) {
                semantic_class = candidate;
                break;
            }
        }
        if (semantic_class == classes.end()) {
            classes.push_back({run.comparable_object, {}, 0U});
            semantic_class = std::prev(classes.end());
        }
        semantic_class->positions.push_back(position);
    }
    return index;
}

Semantic_positions* find_semantic_positions(
    Semantic_position_index& index,
    const Snapshot_run&      run,
    Replay_result&           replay)
{
    const auto digest = index.find(run.digest);
    if (digest == index.end()) {
        return nullptr;
    }
    for (Semantic_positions& candidate : digest->second) {
        ++replay.semantic_digest_object_checks;
        if (candidate.comparable_object == run.comparable_object) {
            return &candidate;
        }
    }
    return nullptr;
}

void compare_snapshot_group(
    Replay_result&                        replay,
    const Causal_snapshot_group&          recorded_group,
    const Causal_snapshot_group&          replayed_group)
{
    const std::vector<Snapshot_run>& recorded_runs = recorded_group.runs;
    const std::vector<Snapshot_run>& replayed_runs = replayed_group.runs;
    if (recorded_runs.empty() && replayed_runs.empty()) {
        return;
    }
    if (recorded_runs.empty()) {
        replay.divergent_snapshot_runs += static_cast<int>(replayed_runs.size());
        for (const Snapshot_run& run : replayed_runs) {
            replay.divergent_snapshot_events +=
                static_cast<int>(run.publications.size());
        }
        record_first_surplus_replayed_snapshot_divergence(
            replay,
            replayed_runs.back().representative());
        return;
    }
    if (replayed_runs.empty()) {
        replay.divergent_snapshot_runs += static_cast<int>(recorded_runs.size());
        for (const Snapshot_run& run : recorded_runs) {
            replay.divergent_snapshot_events +=
                static_cast<int>(run.publications.size());
        }
        record_first_snapshot_divergence(
            replay,
            recorded_runs.back().representative(),
            std::nullopt);
        return;
    }

    const std::size_t recorded_prefix_count = recorded_runs.size() - 1U;
    const std::size_t replayed_prefix_count = replayed_runs.size() - 1U;
    ++replay.snapshot_alignment_comparison_work;
    const bool final_matches = semantic_runs_equal(
        recorded_runs.back(), replayed_runs.back(), replay);
    if (final_matches) {
        record_semantic_match(replay, recorded_runs.back(), replayed_runs.back());
    }
    else {
        ++replay.divergent_snapshot_runs;
        replay.divergent_snapshot_events += static_cast<int>(
            recorded_runs.back().publications.size());
        record_first_snapshot_divergence(
            replay,
            recorded_runs.back().representative(),
            replayed_runs.back().representative().object);
    }

    Semantic_position_index positions = index_semantic_positions(
        replayed_runs,
        replayed_prefix_count,
        replay);
    std::size_t replayed_cursor = 0U;
    for (std::size_t recorded_index = 0U;
         recorded_index < recorded_prefix_count;
         ++recorded_index)
    {
        const Snapshot_run& recorded_run = recorded_runs[recorded_index];
        ++replay.snapshot_alignment_comparison_work;
        Semantic_positions* matching_positions =
            find_semantic_positions(positions, recorded_run, replay);
        if (matching_positions != nullptr) {
            while (matching_positions->cursor < matching_positions->positions.size() &&
                matching_positions->positions[matching_positions->cursor] < replayed_cursor)
            {
                ++matching_positions->cursor;
            }
        }
        if (matching_positions == nullptr ||
            matching_positions->cursor == matching_positions->positions.size())
        {
            ++replay.divergent_snapshot_runs;
            replay.divergent_snapshot_events += static_cast<int>(
                recorded_run.publications.size());
            const std::optional<QJsonObject> candidate =
                replayed_cursor < replayed_prefix_count
                    ? std::optional<QJsonObject>(
                        replayed_runs[replayed_cursor].representative().object)
                    : std::nullopt;
            record_first_snapshot_divergence(
                replay,
                recorded_run.representative(),
                candidate);
            continue;
        }

        const std::size_t matched_position =
            matching_positions->positions[matching_positions->cursor++];
        const std::size_t skipped_count = matched_position - replayed_cursor;
        replay.surplus_replayed_snapshot_runs += static_cast<int>(skipped_count);
        replay.snapshot_alignment_comparison_work += static_cast<int>(skipped_count);
        replayed_cursor = matched_position + 1U;
        record_semantic_match(
            replay,
            recorded_run,
            replayed_runs[matched_position]);
    }
    const std::size_t remaining_count = replayed_prefix_count - replayed_cursor;
    replay.surplus_replayed_snapshot_runs += static_cast<int>(remaining_count);
    replay.snapshot_alignment_comparison_work += static_cast<int>(remaining_count);
}

void compare_recorded_snapshots(
    Replay_result&                                      replay,
    const std::vector<term::Terminal_transcript_event>& recorded_events,
    const Causal_driver_layout&                         recorded_layout,
    const std::vector<term::Terminal_transcript_event>& replayed_events)
{
    replay.recorded_snapshot_events = static_cast<int>(std::count_if(
        recorded_events.begin(),
        recorded_events.end(),
        [](const term::Terminal_transcript_event& event) {
            return event.kind == QStringLiteral("snapshot");
        }));
    replay.replayed_snapshot_events = static_cast<int>(std::count_if(
        replayed_events.begin(),
        replayed_events.end(),
        [](const term::Terminal_transcript_event& event) {
            return event.kind == QStringLiteral("snapshot");
        }));
    if (replay.recorded_snapshot_events == 0) {
        ++replay.divergent_snapshot_events;
        ++replay.divergent_snapshot_runs;
        replay.error = QStringLiteral("recorded transcript contains no snapshot diagnostics");
        return;
    }

    const Causal_driver_layout replayed_layout =
        validated_causal_driver_layout(replayed_events, false);
    replay.recorded_causal_groups = static_cast<int>(
        recorded_layout.group_signatures.size());
    replay.replayed_causal_groups = static_cast<int>(
        replayed_layout.group_signatures.size());
    if (!replayed_layout.valid) {
        replay.causal_protocol_divergences = 1;
        replay.error = QStringLiteral("replay causal protocol invalid: %1")
            .arg(replayed_layout.error);
        return;
    }
    if (recorded_layout.group_signatures.size() !=
        replayed_layout.group_signatures.size())
    {
        replay.causal_driver_divergences = 1;
        ++replay.divergent_snapshot_events;
        ++replay.divergent_snapshot_runs;
        replay.error = QStringLiteral(
            "recorded and replayed normalized causal group counts diverged");
        return;
    }
    for (std::size_t group_index = 0U;
         group_index < recorded_layout.group_signatures.size();
         ++group_index)
    {
        if (!causal_group_signatures_equal(
                recorded_layout.group_signatures[group_index],
                replayed_layout.group_signatures[group_index],
                replay))
        {
            replay.causal_driver_divergences = 1;
            ++replay.divergent_snapshot_events;
            ++replay.divergent_snapshot_runs;
            replay.error = QStringLiteral(
                "recorded and replayed causal group signatures diverged at group %1")
                .arg(static_cast<qulonglong>(group_index));
            return;
        }
    }

    const std::vector<Causal_snapshot_group> recorded_groups = grouped_snapshot_runs(
        recorded_layout.snapshots_by_group,
        replay);
    const std::vector<Causal_snapshot_group> replayed_groups = grouped_snapshot_runs(
        replayed_layout.snapshots_by_group,
        replay);
    for (const Causal_snapshot_group& group : recorded_groups) {
        replay.recorded_snapshot_runs += static_cast<int>(group.runs.size());
    }
    for (const Causal_snapshot_group& group : replayed_groups) {
        replay.replayed_snapshot_runs += static_cast<int>(group.runs.size());
    }
    for (std::size_t group_index = 0U;
         group_index < recorded_groups.size();
         ++group_index)
    {
        compare_snapshot_group(
            replay,
            recorded_groups[group_index],
            replayed_groups[group_index]);
    }
}

struct Pending_scroll_intent
{
    QString                               source;
    int                                   requested_line_delta = 0;
    std::optional<int>                    requested_offset_from_tail;
    term::Terminal_viewport_scroll_result result;
    term::Terminal_viewport_state         viewport_before;
};

Pending_scroll_intent pending_scroll_intent_from_event(
    const term::Terminal_transcript_event&       event,
    const term::Terminal_viewport_scroll_result& result,
    const term::Terminal_viewport_state&         viewport_before)
{
    Pending_scroll_intent pending;
    pending.source               = event.object.value(QStringLiteral("source")).toString();
    pending.requested_line_delta = event.object.value(QStringLiteral("requested_line_delta")).toInt();
    pending.result               = result;
    pending.viewport_before      = viewport_before;
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
            scroll_action_name(pending.result.action) ||
        !viewport_matches_object(
            pending.viewport_before,
            event.object.value(QStringLiteral("viewport_before")).toObject()))
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

std::optional<term::terminal_grid_position_t> optional_position_from_event(
    const term::Terminal_transcript_event& event,
    const QString&                         field_name)
{
    if (!event.object.contains(field_name)) {
        return std::nullopt;
    }
    return position_from_object(event.object.value(field_name).toObject());
}

Replay_result replay_events(const std::vector<term::Terminal_transcript_event>& events)
{
    Replay_result replay;
    replay.recorded_snapshot_events = static_cast<int>(std::count_if(
        events.begin(),
        events.end(),
        [](const term::Terminal_transcript_event& event) {
            return event.kind == QStringLiteral("snapshot");
        }));
    const Causal_driver_layout recorded_layout =
        validated_causal_driver_layout(events);
    if (!recorded_layout.valid) {
        replay.causal_protocol_divergences = 1;
        replay.error = QStringLiteral("recorded causal protocol invalid: %1")
            .arg(recorded_layout.error);
        return replay;
    }

    QTemporaryDir replay_transcript_dir;
    if (!replay_transcript_dir.isValid()) {
        replay.error = QStringLiteral("could not create temporary replay transcript directory");
        return replay;
    }

    const QString replay_transcript_path =
        replay_transcript_dir.filePath(QStringLiteral("replayed.ndjson"));
    QString replay_transcript_error;
    std::shared_ptr<term::Terminal_transcript_recorder> replay_recorder =
        term::Terminal_transcript_recorder::create(
            replay_transcript_path,
            true,
            &replay_transcript_error);
    if (replay_recorder == nullptr) {
        replay.error = replay_transcript_error;
        return replay;
    }

    auto backend = std::make_unique<Replay_backend>();
    Replay_backend* backend_ptr = backend.get();
    term::Terminal_session_config config = replay_session_config(events);
    config.transcript_recorder = replay_recorder;
    config.backend_event_notifier = []() {};
    auto session = std::make_unique<term::Terminal_session>(std::move(backend), config);

    const bool has_resize_request = std::any_of(
        events.begin(),
        events.end(),
        [](const term::Terminal_transcript_event& event) {
            return event.kind == QStringLiteral("session.resize_request");
        });

    bool started = false;
    std::optional<Pending_scroll_intent> pending_scroll_intent;
    std::vector<QByteArray> backend_bytes_by_group(
        recorded_layout.group_driver_kinds.size());
    std::vector<std::size_t> backend_event_count_by_group(
        recorded_layout.group_driver_kinds.size(),
        0U);
    for (std::size_t driver_index = 0U;
         driver_index < recorded_layout.drivers.size();
         ++driver_index)
    {
        if (recorded_layout.drivers[driver_index]->kind ==
            QStringLiteral("backend.output"))
        {
            const std::size_t group = recorded_layout.group_for_driver[driver_index];
            const QByteArray bytes = event_bytes(*recorded_layout.drivers[driver_index]);
            backend_bytes_by_group[group] += bytes;
            ++backend_event_count_by_group[group];
        }
    }
    std::set<std::size_t> emitted_backend_groups;
    std::size_t driver_index = 0U;
    for (const term::Terminal_transcript_event& event : events) {
        if (event.kind == QStringLiteral("header") ||
            is_replay_transparent_diagnostic_event(event.kind))
        {
            continue;
        }

        std::optional<std::size_t> event_group;
        if (is_causal_driver_event(event)) {
            event_group = recorded_layout.group_for_driver[driver_index++];
        }

        if (event.kind == QStringLiteral("session.start")) {
            const term::Terminal_session_result result =
                session->start(launch_config_from_event(event));
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
            if (!emitted_backend_groups.insert(*event_group).second) {
                continue;
            }
            backend_ptr->emit_output(backend_bytes_by_group[*event_group]);
            if (backend_event_count_by_group[*event_group] > 1U) {
                while (session->process_backend_callback_events_for(
                        std::chrono::steady_clock::duration::zero()) !=
                    term::Backend_callback_drain_stop::COMPLETE)
                {
                }
            }
            else {
                session->process_backend_callback_events();
            }
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
                    ? session->write_focus_event(*focused).result
                    : session->write_user_bytes(bytes);
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
            const term::Terminal_session_result result = session->resize(
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
            session->process_backend_callback_events();
            ++replay.backend_error_events;
        }
        else
        if (event.kind == QStringLiteral("session.process_exit")) {
            backend_ptr->emit_exit({term::Terminal_exit_reason::EXITED, 0});
            session->process_backend_callback_events();
            ++replay.process_exit_events;
        }
        else
        if (event.kind == QStringLiteral("session.text_area_resize_request")) {
            ++replay.text_area_resize_request_events;
        }
        else
        if (event.kind == QStringLiteral("surface.scroll_intent")) {
            if (!session_viewport_matches_object(
                    *session,
                    event.object.value(QStringLiteral("viewport_before")).toObject()))
            {
                const term::Terminal_viewport_state recorded_viewport = viewport_from_object(
                    event.object.value(QStringLiteral("viewport_before")).toObject());
                const term::Terminal_viewport_state actual_viewport =
                    visible_viewport_state(*session);
                replay.causal_protocol_divergences = 1;
                replay.error = QStringLiteral(
                    "surface.scroll_intent viewport_before diverged at event %1 "
                    "(recorded scrollback=%2 offset=%3, replayed scrollback=%4 offset=%5)")
                    .arg(static_cast<qulonglong>(event.event_index))
                    .arg(recorded_viewport.scrollback_rows)
                    .arg(recorded_viewport.offset_from_tail)
                    .arg(actual_viewport.scrollback_rows)
                    .arg(actual_viewport.offset_from_tail);
                return replay;
            }
            const term::Terminal_viewport_state viewport_before =
                visible_viewport_state(*session);
            const QString source = event.object.value(QStringLiteral("source")).toString();
            const int requested_line_delta =
                event.object.value(QStringLiteral("requested_line_delta")).toInt();
            std::optional<int> requested_offset_from_tail;
            if (event.object.contains(QStringLiteral("requested_offset_from_tail"))) {
                requested_offset_from_tail =
                    event.object.value(QStringLiteral("requested_offset_from_tail")).toInt();
            }
            (void)replay_recorder->record_surface_scroll_intent({
                source,
                requested_line_delta,
                requested_offset_from_tail,
                viewport_before,
            });
            term::Terminal_viewport_scroll_result result;
            if (!apply_scroll_event(*session, event, &result, &replay.error)) {
                replay.causal_protocol_divergences = 1;
                return replay;
            }
            pending_scroll_intent = pending_scroll_intent_from_event(
                event,
                result,
                viewport_before);
            ++replay.surface_scroll_intents;
        }
        else
        if (event.kind == QStringLiteral("surface.scroll")) {
            if (!pending_scroll_intent.has_value() ||
                !scroll_event_matches_pending_intent(event, *pending_scroll_intent) ||
                !session_viewport_matches_object(
                    *session,
                    event.object.value(QStringLiteral("viewport_after")).toObject()))
            {
                replay.causal_protocol_divergences = 1;
                replay.error = QStringLiteral(
                    "surface.scroll result did not match its applied intent at event %1")
                    .arg(static_cast<qulonglong>(event.event_index));
                return replay;
            }
            const term::Terminal_viewport_scroll_result result =
                pending_scroll_intent->result;
            const term::Terminal_viewport_state viewport_before =
                pending_scroll_intent->viewport_before;
            pending_scroll_intent.reset();
            std::optional<int> requested_offset_from_tail;
            if (event.object.contains(QStringLiteral("requested_offset_from_tail"))) {
                requested_offset_from_tail =
                    event.object.value(QStringLiteral("requested_offset_from_tail")).toInt();
            }
            (void)replay_recorder->record_surface_scroll({
                event.object.value(QStringLiteral("source")).toString(),
                event.object.value(QStringLiteral("requested_line_delta")).toInt(),
                requested_offset_from_tail,
                result,
                viewport_before,
                visible_viewport_state(*session),
            });
        }
        else
        if (event.kind == QStringLiteral("surface.selection_drag")) {
            (void)replay_recorder->record_surface_selection_drag({
                event.object.value(QStringLiteral("phase")).toString(),
                optional_position_from_event(event, QStringLiteral("anchor")),
                optional_position_from_event(event, QStringLiteral("focus")),
                selection_range_from_event(event),
                event.object.value(QStringLiteral("moved")).toBool(),
            });
            apply_selection_event(*session, event);
            ++replay.semantic_selection_events;
        }
        else
        if (event.kind == QStringLiteral("snapshot")) {
            continue;
        }
    }

    replay.host_writes   = backend_ptr->writes;
    replay.snapshot      = session->latest_render_snapshot();
    replay.viewport      = session->viewport_state();
    replay.selected_text = session->selected_text();
    const bool replay_capture_failed = replay_recorder->failed();
    const QString replay_capture_error = replay_recorder->error_message();
    session.reset();
    replay_recorder.reset();

    if (replay_capture_failed) {
        replay.error = replay_capture_error;
        return replay;
    }

    const std::optional<std::vector<term::Terminal_transcript_event>> replayed_events =
        term::read_terminal_transcript(replay_transcript_path, &replay_transcript_error);
    if (!replayed_events.has_value()) {
        replay.error = QStringLiteral("temporary replay transcript invalid: %1")
            .arg(replay_transcript_error);
        return replay;
    }

    compare_recorded_snapshots(replay, events, recorded_layout, *replayed_events);
    if (replay.divergent_snapshot_events != 0 && replay.error.isEmpty()) {
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
    QString transcript_path;
    for (int index = 1; index < arguments.size(); ++index) {
        const QString argument = arguments.at(index);
        if (argument == QStringLiteral("--strict-all-snapshots")) {
            continue;
        }

        if (argument.startsWith(QStringLiteral("--")) || !transcript_path.isEmpty()) {
            std::cerr << usage_text().toStdString();
            return 2;
        }
        transcript_path = argument;
    }

    if (transcript_path.isEmpty()) {
        std::cerr << usage_text().toStdString();
        return 2;
    }

    QString error;
    const std::optional<std::vector<term::Terminal_transcript_event>> events =
        term::read_terminal_transcript(transcript_path, &error);
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
        << " replayed_snapshot_events=" << replay.replayed_snapshot_events
        << " matching_snapshot_events=" << replay.matching_snapshot_events
        << " divergent_snapshot_events=" << replay.divergent_snapshot_events << '\n'
        << "recorded_snapshot_runs=" << replay.recorded_snapshot_runs
        << " replayed_snapshot_runs=" << replay.replayed_snapshot_runs
        << " matching_snapshot_runs=" << replay.matching_snapshot_runs
        << " divergent_snapshot_runs=" << replay.divergent_snapshot_runs
        << " surplus_replayed_snapshot_runs="
        << replay.surplus_replayed_snapshot_runs << '\n'
        << "snapshot_alignment_comparison_work="
        << replay.snapshot_alignment_comparison_work
        << " semantic_digest_object_checks="
        << replay.semantic_digest_object_checks << '\n'
        << "recorded_causal_groups=" << replay.recorded_causal_groups
        << " replayed_causal_groups=" << replay.replayed_causal_groups << '\n'
        << "causal_driver_divergences=" << replay.causal_driver_divergences
        << " causal_protocol_divergences="
        << replay.causal_protocol_divergences << '\n'
        << "dirty_mismatch_snapshot_events=" << replay.dirty_mismatch_snapshot_events
        << " unpaired_recorded_dirty_snapshot_events="
        << replay.unpaired_recorded_dirty_snapshot_events
        << " unpaired_replayed_dirty_snapshot_events="
        << replay.unpaired_replayed_dirty_snapshot_events << '\n'
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
        else
        if (replay.first_divergent_recorded_snapshot_missing) {
            std::cout << "missing";
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
