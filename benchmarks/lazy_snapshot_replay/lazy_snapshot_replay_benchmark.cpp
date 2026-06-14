#include "vnm_terminal/internal/terminal_session.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

constexpr int k_schema_version = 2;
constexpr int k_initial_rows   = 10;
constexpr int k_initial_cols   = 48;
constexpr int k_resized_rows   = 12;
constexpr int k_resized_cols   = 52;

const QString k_schema_name = QStringLiteral("vnm_terminal_lazy_snapshot_replay_benchmark");
const QString k_measurement_boundary = QStringLiteral(
    "deterministic_terminal_session_event_to_full_vs_lazy_snapshot_comparison");

using Clock = std::chrono::steady_clock;

enum class Replay_event_kind
{
    BACKEND_OUTPUT,
    HOST_INPUT,
    RESIZE,
    VIEWPORT_SCROLL,
};

enum class Expected_result
{
    NO_SNAPSHOT,
    ELIGIBLE,
    FALLBACK,
};

struct Expected_outcome
{
    Expected_result                              result = Expected_result::ELIGIBLE;
    term::Terminal_lazy_snapshot_fallback_reason fallback =
        term::Terminal_lazy_snapshot_fallback_reason::NONE;
};

struct Replay_event
{
    QString                    workload;
    QString                    name;
    Replay_event_kind          kind = Replay_event_kind::BACKEND_OUTPUT;
    QByteArray                 bytes;
    term::terminal_grid_size_t resize_grid;
    int                        scroll_delta       = 0;
    int                        causal_input_epoch = 0;
    Expected_outcome           expected;
};

struct Options
{
    QString output_path;
    bool    quiet         = false;
    bool    validate_json = false;
    bool    help          = false;
};

struct Compare_result
{
    bool        matches = false;
    QStringList mismatch_fields;
};

struct Summary
{
    int events                        = 0;
    int snapshot_events               = 0;
    int no_snapshot_events            = 0;
    int eligible_events               = 0;
    int expected_fallback_events      = 0;
    int unexpected_fallback_events    = 0;
    int unexpected_eligible_events    = 0;
    int parity_mismatch_events        = 0;
    int candidate_materialized_events = 0;
    int fallback_reason_mismatch_events = 0;
    std::uint64_t candidate_materialization_calls = 0U;
    std::uint64_t candidate_materialization_rows  = 0U;
    std::uint64_t candidate_materialization_cells = 0U;
    std::uint64_t full_cells          = 0U;
    std::uint64_t lazy_cells_scanned  = 0U;
    std::uint64_t lazy_cells_emitted  = 0U;
    std::uint64_t lazy_dirty_rows     = 0U;
    std::uint64_t lazy_borrowed_rows  = 0U;
    term::Terminal_lazy_snapshot_fallback_reason_counters
        expected_fallback_reason_counts;
    term::Terminal_lazy_snapshot_fallback_reason_counters
        observed_fallback_reason_counts;
};

struct Run_result
{
    QJsonArray events;
    Summary    summary;
    bool       passed = true;
};

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
        writes.push_back(std::move(bytes));
        return term::backend_accept();
    }

    term::Terminal_backend_result resize(
        term::Terminal_backend_resize_request request) override
    {
        resize_requests.push_back(request);
        return term::backend_accept();
    }

    term::Terminal_backend_result set_output_paused(bool paused) override
    {
        pause_requests.push_back(paused);
        return term::backend_accept();
    }

    term::Terminal_backend_result interrupt() override
    {
        m_running = false;
        return term::backend_accept();
    }

    term::Terminal_backend_result terminate() override
    {
        m_running = false;
        return term::backend_accept();
    }

    bool emit_output(QByteArray bytes)
    {
        if (!m_running) {
            return false;
        }

        m_callbacks.output_received(std::move(bytes));
        return true;
    }

    std::vector<QByteArray>                            writes;
    std::vector<term::Terminal_backend_resize_request> resize_requests;
    std::vector<bool>                                  pause_requests;

private:
    bool                             m_running = false;
    term::Terminal_backend_callbacks m_callbacks;
};

qint64 bounded_json_i64(std::uint64_t value)
{
    constexpr std::uint64_t k_max =
        static_cast<std::uint64_t>(std::numeric_limits<qint64>::max());
    return value > k_max
        ? std::numeric_limits<qint64>::max()
        : static_cast<qint64>(value);
}

void insert_u64(QJsonObject& object, const QString& key, std::uint64_t value)
{
    object.insert(key, bounded_json_i64(value));
}

qint64 elapsed_ns(Clock::time_point begin, Clock::time_point end)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
}

std::uint64_t fnv1a64(QByteArrayView bytes, std::uint64_t hash = 14695981039346656037ULL)
{
    for (char byte : bytes) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

QString hex_u64(std::uint64_t value)
{
    return QStringLiteral("%1").arg(
        static_cast<qulonglong>(value),
        16,
        16,
        QLatin1Char('0'));
}

QString bytes_hash64(QByteArrayView bytes)
{
    return hex_u64(fnv1a64(bytes));
}

QString snapshot_text_hash64(const term::Terminal_render_snapshot& snapshot)
{
    std::uint64_t hash = 14695981039346656037ULL;
    const term::Terminal_render_snapshot_row_content_view rows(snapshot);
    for (int row = 0; row < rows.row_count(); ++row) {
        const QByteArray text =
            rows.row_text(row, 0, snapshot.grid_size.columns, false).toUtf8();
        hash = fnv1a64(QByteArrayView(text), hash);
        const char separator = '\n';
        hash = fnv1a64(QByteArrayView(&separator, 1), hash);
    }
    return hex_u64(hash);
}

std::uint64_t row_view_cell_count(const term::Terminal_render_snapshot& snapshot)
{
    const term::Terminal_render_snapshot_row_content_view rows(snapshot);
    return static_cast<std::uint64_t>(rows.cell_count());
}

std::uint64_t dirty_row_count(const term::Terminal_render_snapshot& snapshot)
{
    std::uint64_t count = 0U;
    for (const term::Terminal_render_dirty_row_range& range : snapshot.dirty_row_ranges) {
        count += static_cast<std::uint64_t>(std::max(range.row_count, 0));
    }
    return count;
}

QString fallback_reason_name(term::Terminal_lazy_snapshot_fallback_reason reason)
{
    if (reason == term::Terminal_lazy_snapshot_fallback_reason::NONE) {
        return QStringLiteral("none");
    }

    for (const term::terminal_lazy_snapshot_fallback_reason_descriptor_t& descriptor :
        term::terminal_lazy_snapshot_fallback_reason_descriptors())
    {
        if (descriptor.reason == reason) {
            return QString::fromLatin1(descriptor.key);
        }
    }
    return QStringLiteral("unknown");
}

void increment_fallback_reason_counter(
    term::Terminal_lazy_snapshot_fallback_reason_counters& counters,
    term::Terminal_lazy_snapshot_fallback_reason           reason)
{
    if (reason == term::Terminal_lazy_snapshot_fallback_reason::NONE) {
        return;
    }

    for (const term::terminal_lazy_snapshot_fallback_reason_descriptor_t& descriptor :
        term::terminal_lazy_snapshot_fallback_reason_descriptors())
    {
        if (descriptor.reason == reason) {
            ++(counters.*descriptor.counter);
            return;
        }
    }
}

QJsonObject fallback_reason_counts_json(
    const term::Terminal_lazy_snapshot_fallback_reason_counters& counters)
{
    QJsonObject object;
    for (const term::terminal_lazy_snapshot_fallback_reason_descriptor_t& descriptor :
        term::terminal_lazy_snapshot_fallback_reason_descriptors())
    {
        insert_u64(
            object,
            QString::fromLatin1(descriptor.key),
            term::terminal_lazy_snapshot_fallback_reason_counter(
                counters,
                descriptor));
    }
    return object;
}

QString event_kind_name(Replay_event_kind kind)
{
    switch (kind) {
        case Replay_event_kind::BACKEND_OUTPUT:
            return QStringLiteral("backend.output");
        case Replay_event_kind::HOST_INPUT:
            return QStringLiteral("host.input");
        case Replay_event_kind::RESIZE:
            return QStringLiteral("session.resize");
        case Replay_event_kind::VIEWPORT_SCROLL:
            return QStringLiteral("surface.scroll");
    }

    return QStringLiteral("unknown");
}

QString expected_result_name(Expected_result result)
{
    switch (result) {
        case Expected_result::NO_SNAPSHOT:
            return QStringLiteral("no_snapshot");
        case Expected_result::ELIGIBLE:
            return QStringLiteral("eligible");
        case Expected_result::FALLBACK:
            return QStringLiteral("fallback");
    }

    return QStringLiteral("unknown");
}

QString validation_status_name(term::Terminal_render_snapshot_status status)
{
    switch (status) {
        case term::Terminal_render_snapshot_status::OK:
            return QStringLiteral("ok");
        case term::Terminal_render_snapshot_status::INVALID_GRID_SIZE:
            return QStringLiteral("invalid_grid_size");
        case term::Terminal_render_snapshot_status::INVALID_CELL_POSITION:
            return QStringLiteral("invalid_cell_position");
        case term::Terminal_render_snapshot_status::INVALID_CELL_WIDTH:
            return QStringLiteral("invalid_cell_width");
        case term::Terminal_render_snapshot_status::INVALID_CELL_TEXT_CATEGORY:
            return QStringLiteral("invalid_cell_text_category");
        case term::Terminal_render_snapshot_status::INVALID_CELL_OVERLAP:
            return QStringLiteral("invalid_cell_overlap");
        case term::Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION:
            return QStringLiteral("invalid_wide_cell_continuation");
        case term::Terminal_render_snapshot_status::INVALID_WIDE_CELL_STYLE:
            return QStringLiteral("invalid_wide_cell_style");
        case term::Terminal_render_snapshot_status::INVALID_STYLE_ID:
            return QStringLiteral("invalid_style_id");
        case term::Terminal_render_snapshot_status::INVALID_CURSOR_POSITION:
            return QStringLiteral("invalid_cursor_position");
        case term::Terminal_render_snapshot_status::INVALID_VIEWPORT:
            return QStringLiteral("invalid_viewport");
        case term::Terminal_render_snapshot_status::INVALID_DIRTY_ROW_RANGE:
            return QStringLiteral("invalid_dirty_row_range");
        case term::Terminal_render_snapshot_status::INVALID_SELECTION_SPAN:
            return QStringLiteral("invalid_selection_span");
        case term::Terminal_render_snapshot_status::INVALID_LINE_PROVENANCE:
            return QStringLiteral("invalid_line_provenance");
        case term::Terminal_render_snapshot_status::INVALID_HYPERLINK_METADATA:
            return QStringLiteral("invalid_hyperlink_metadata");
        case term::Terminal_render_snapshot_status::INVALID_SNAPSHOT_BASIS_PURPOSE:
            return QStringLiteral("invalid_snapshot_basis_purpose");
        case term::Terminal_render_snapshot_status::INVALID_CELL_ORDER:
            return QStringLiteral("invalid_cell_order");
    }

    return QStringLiteral("unknown");
}

bool color_states_match(
    const term::Terminal_color_state& left,
    const term::Terminal_color_state& right)
{
    return
        left.default_foreground_rgba == right.default_foreground_rgba &&
        left.default_background_rgba == right.default_background_rgba &&
        left.cursor_rgba             == right.cursor_rgba             &&
        left.palette_rgba            == right.palette_rgba;
}

bool viewport_states_match(
    const term::Terminal_viewport_state& left,
    const term::Terminal_viewport_state& right)
{
    return
        left.active_buffer   == right.active_buffer   &&
        left.scrollback_rows == right.scrollback_rows &&
        left.visible_rows    == right.visible_rows    &&
        left.offset_from_tail == right.offset_from_tail &&
        left.follow_tail     == right.follow_tail     &&
        left.alternate_screen_scroll_policy ==
            right.alternate_screen_scroll_policy;
}

bool cursors_match(
    const term::Terminal_render_cursor& left,
    const term::Terminal_render_cursor& right)
{
    return
        left.position      == right.position      &&
        left.shape         == right.shape         &&
        left.visible       == right.visible       &&
        left.blink_enabled == right.blink_enabled;
}

bool metadata_match(
    const term::Terminal_render_metadata& left,
    const term::Terminal_render_metadata& right)
{
    return
        left.sequence                     == right.sequence                     &&
        left.row_origin_generation        == right.row_origin_generation        &&
        left.backend_geometry_in_sync     == right.backend_geometry_in_sync     &&
        left.visual_bell_active           == right.visual_bell_active           &&
        left.mouse_reporting_mode_changed == right.mouse_reporting_mode_changed;
}

bool modes_match(
    const term::Terminal_mode_state& left,
    const term::Terminal_mode_state& right)
{
    return
        left.application_cursor_keys == right.application_cursor_keys &&
        left.reverse_video           == right.reverse_video           &&
        left.origin_mode             == right.origin_mode             &&
        left.autowrap                == right.autowrap                &&
        left.cursor_visible          == right.cursor_visible          &&
        left.mouse_tracking          == right.mouse_tracking          &&
        left.focus_reporting         == right.focus_reporting         &&
        left.sgr_mouse_encoding      == right.sgr_mouse_encoding      &&
        left.alternate_scroll        == right.alternate_scroll        &&
        left.bracketed_paste         == right.bracketed_paste         &&
        left.synchronized_output     == right.synchronized_output;
}

bool dirty_ranges_match(
    const std::vector<term::Terminal_render_dirty_row_range>& left,
    const std::vector<term::Terminal_render_dirty_row_range>& right)
{
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (left[index].first_row != right[index].first_row ||
            left[index].row_count != right[index].row_count)
        {
            return false;
        }
    }
    return true;
}

bool hyperlinks_match(
    const std::vector<term::Terminal_render_hyperlink_metadata>& left,
    const std::vector<term::Terminal_render_hyperlink_metadata>& right)
{
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (left[index].hyperlink_id != right[index].hyperlink_id ||
            left[index].identity_key != right[index].identity_key ||
            left[index].uri          != right[index].uri)
        {
            return false;
        }
    }
    return true;
}

void append_mismatch(QStringList& fields, const QString& field, bool matches)
{
    if (!matches) {
        fields.push_back(field);
    }
}

Compare_result compare_lazy_to_full(
    const term::Terminal_render_snapshot& full,
    const term::Terminal_render_snapshot& lazy)
{
    Compare_result result;
    append_mismatch(
        result.mismatch_fields,
        QStringLiteral("lazy_payload"),
        lazy.lazy_row_payloads != nullptr && lazy.cells.empty());
    append_mismatch(
        result.mismatch_fields,
        QStringLiteral("full_validation"),
        term::validate_render_snapshot(full).status ==
            term::Terminal_render_snapshot_status::OK);
    append_mismatch(
        result.mismatch_fields,
        QStringLiteral("lazy_validation"),
        term::validate_render_snapshot(lazy).status ==
            term::Terminal_render_snapshot_status::OK);
    append_mismatch(result.mismatch_fields, QStringLiteral("basis"), full.basis == lazy.basis);
    append_mismatch(result.mismatch_fields, QStringLiteral("purpose"), full.purpose == lazy.purpose);
    append_mismatch(
        result.mismatch_fields,
        QStringLiteral("grid_size"),
        term::grid_sizes_match(full.grid_size, lazy.grid_size));
    append_mismatch(
        result.mismatch_fields,
        QStringLiteral("viewport"),
        viewport_states_match(full.viewport, lazy.viewport));
    append_mismatch(
        result.mismatch_fields,
        QStringLiteral("color_state"),
        color_states_match(full.color_state, lazy.color_state));
    append_mismatch(result.mismatch_fields, QStringLiteral("styles"), full.styles == lazy.styles);
    append_mismatch(
        result.mismatch_fields,
        QStringLiteral("dirty_ranges"),
        dirty_ranges_match(full.dirty_row_ranges, lazy.dirty_row_ranges));
    append_mismatch(
        result.mismatch_fields,
        QStringLiteral("hyperlinks"),
        hyperlinks_match(full.hyperlinks, lazy.hyperlinks));
    append_mismatch(result.mismatch_fields, QStringLiteral("cursor"), cursors_match(full.cursor, lazy.cursor));
    append_mismatch(result.mismatch_fields, QStringLiteral("metadata"), metadata_match(full.metadata, lazy.metadata));
    append_mismatch(result.mismatch_fields, QStringLiteral("modes"), modes_match(full.modes, lazy.modes));

    const term::Terminal_render_snapshot_row_content_view full_rows(full);
    const term::Terminal_render_snapshot_row_content_view lazy_rows(lazy);
    append_mismatch(
        result.mismatch_fields,
        QStringLiteral("row_count"),
        full_rows.row_count() == lazy_rows.row_count());
    append_mismatch(
        result.mismatch_fields,
        QStringLiteral("cell_count"),
        full_rows.cell_count() == lazy_rows.cell_count());

    const int rows_to_check = std::min(full_rows.row_count(), lazy_rows.row_count());
    for (int row = 0; row < rows_to_check; ++row) {
        const QString label = QStringLiteral("row_%1").arg(row);
        append_mismatch(
            result.mismatch_fields,
            label + QStringLiteral("_text"),
            full_rows.row_text(row, 0, full.grid_size.columns, false) ==
                lazy_rows.row_text(row, 0, lazy.grid_size.columns, false));

        const term::Terminal_render_snapshot_row_content full_row = full_rows.row_at(row);
        const term::Terminal_render_snapshot_row_content lazy_row = lazy_rows.row_at(row);
        append_mismatch(
            result.mismatch_fields,
            label + QStringLiteral("_dirty"),
            full_row.dirty() == lazy_row.dirty());

        const term::Terminal_render_line_provenance* full_provenance =
            full_row.provenance_or_null();
        const term::Terminal_render_line_provenance* lazy_provenance =
            lazy_row.provenance_or_null();
        append_mismatch(
            result.mismatch_fields,
            label + QStringLiteral("_provenance_presence"),
            (full_provenance == nullptr) == (lazy_provenance == nullptr));
        if (full_provenance != nullptr && lazy_provenance != nullptr) {
            append_mismatch(
                result.mismatch_fields,
                label + QStringLiteral("_provenance"),
                *full_provenance == *lazy_provenance);
            append_mismatch(
                result.mismatch_fields,
                label + QStringLiteral("_provenance_content_stamp"),
                full_provenance->content_stamp_ms == lazy_provenance->content_stamp_ms);
        }

        auto full_cell = full_row.begin();
        auto lazy_cell = lazy_row.begin();
        for (; full_cell != full_row.end() && lazy_cell != lazy_row.end();
            ++full_cell, ++lazy_cell)
        {
            append_mismatch(
                result.mismatch_fields,
                label + QStringLiteral("_cells"),
                term::render_snapshot_cells_equal(*full_cell, *lazy_cell));
        }
        append_mismatch(
            result.mismatch_fields,
            label + QStringLiteral("_cell_count"),
            full_cell == full_row.end() && lazy_cell == lazy_row.end());
    }

    result.matches = result.mismatch_fields.empty();
    return result;
}

QJsonArray string_list_json(const QStringList& values)
{
    QJsonArray array;
    for (const QString& value : values) {
        array.append(value);
    }
    return array;
}

QJsonObject grid_json(term::terminal_grid_size_t grid)
{
    return {
        {QStringLiteral("rows"),    grid.rows},
        {QStringLiteral("columns"), grid.columns},
    };
}

QJsonObject viewport_json(const term::Terminal_viewport_state& viewport)
{
    return {
        {QStringLiteral("active_buffer"),
            viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE
                ? QStringLiteral("alternate")
                : QStringLiteral("primary")},
        {QStringLiteral("scrollback_rows"),  viewport.scrollback_rows},
        {QStringLiteral("visible_rows"),     viewport.visible_rows},
        {QStringLiteral("offset_from_tail"), viewport.offset_from_tail},
        {QStringLiteral("follow_tail"),      viewport.follow_tail},
    };
}

QJsonObject snapshot_json(const term::Terminal_render_snapshot& snapshot)
{
    return {
        {QStringLiteral("sequence"), bounded_json_i64(snapshot.metadata.sequence)},
        {QStringLiteral("basis"), term::render_snapshot_basis_name(snapshot.basis)},
        {QStringLiteral("purpose"), term::render_snapshot_purpose_name(snapshot.purpose)},
        {QStringLiteral("grid_size"), grid_json(snapshot.grid_size)},
        {QStringLiteral("viewport"), viewport_json(snapshot.viewport)},
        {QStringLiteral("flat_cell_count"), static_cast<int>(snapshot.cells.size())},
        {QStringLiteral("row_view_cell_count"), bounded_json_i64(row_view_cell_count(snapshot))},
        {QStringLiteral("dirty_row_count"), bounded_json_i64(dirty_row_count(snapshot))},
        {QStringLiteral("style_count"), static_cast<int>(snapshot.styles.size())},
        {QStringLiteral("hyperlink_count"), static_cast<int>(snapshot.hyperlinks.size())},
        {QStringLiteral("visible_text_hash64"), snapshot_text_hash64(snapshot)},
        {QStringLiteral("validation_status"),
            validation_status_name(term::validate_render_snapshot(snapshot).status)},
    };
}

QJsonObject lazy_result_json(
    const term::Terminal_session_lazy_snapshot_composer_result& result,
    const Compare_result*                                       compare)
{
    QJsonObject object;
    object.insert(QStringLiteral("eligible"), result.eligible);
    object.insert(QStringLiteral("fallback_reason"), fallback_reason_name(result.fallback_reason));
    object.insert(
        QStringLiteral("materialization_matches_full_snapshot"),
        result.materialization_matches_full_snapshot);
    object.insert(
        QStringLiteral("materialization_mismatch_for_testing"),
        result.materialization_mismatch_for_testing);
    insert_u64(object, QStringLiteral("dirty_rows_visible"), result.dirty_rows_visible);
    insert_u64(
        object,
        QStringLiteral("previous_snapshot_borrowed_rows"),
        result.previous_snapshot_borrowed_rows);
    insert_u64(object, QStringLiteral("producer_owned_rows"), result.producer_owned_rows);
    insert_u64(object, QStringLiteral("producer_materialized_rows"), result.producer_materialized_rows);
    insert_u64(object, QStringLiteral("producer_cells_scanned"), result.producer_cells_scanned);
    insert_u64(object, QStringLiteral("producer_cells_emitted"), result.producer_cells_emitted);
    insert_u64(
        object,
        QStringLiteral("consumer_materialization_calls"),
        result.consumer_materialization_calls);
    insert_u64(
        object,
        QStringLiteral("consumer_materialization_rows"),
        result.consumer_materialization_rows);
    insert_u64(
        object,
        QStringLiteral("consumer_materialization_cells"),
        result.consumer_materialization_cells);

    if (result.lazy_snapshot.has_value()) {
        object.insert(QStringLiteral("lazy_snapshot"), snapshot_json(*result.lazy_snapshot));
    }
    if (compare != nullptr) {
        object.insert(QStringLiteral("matches_full_observation"), compare->matches);
        object.insert(QStringLiteral("mismatch_fields"), string_list_json(compare->mismatch_fields));
    }
    return object;
}

QJsonObject expected_json(const Expected_outcome& expected)
{
    return {
        {QStringLiteral("result"), expected_result_name(expected.result)},
        {QStringLiteral("fallback_reason"), fallback_reason_name(expected.fallback)},
    };
}

QByteArray cursor_to(int row, int column)
{
    return QStringLiteral("\x1b[%1;%2H").arg(row).arg(column).toLatin1();
}

QByteArray erase_line_at(int row)
{
    return cursor_to(row, 1) + QByteArrayLiteral("\x1b[2K");
}

QByteArray write_row(int row, QByteArray text)
{
    return erase_line_at(row) + std::move(text);
}

QByteArray numbered_lines(const char* prefix, int first, int count)
{
    QByteArray output;
    for (int index = first; index < first + count; ++index) {
        output += QStringLiteral("%1 %2 | stable replay evidence line\r\n")
            .arg(QString::fromLatin1(prefix))
            .arg(index, 3, 10, QLatin1Char('0'))
            .toLatin1();
    }
    return output;
}

QByteArray tui_frame(int frame_index)
{
    QByteArray output = QByteArrayLiteral("\x1b[H");
    for (int row = 1; row <= k_initial_rows; ++row) {
        output += cursor_to(row, 1);
        output += QStringLiteral("TUI frame %1 row %2 | %3")
            .arg(frame_index, 2, 10, QLatin1Char('0'))
            .arg(row, 2, 10, QLatin1Char('0'))
            .arg((row + frame_index) % 2 == 0
                ? QStringLiteral("active pane")
                : QStringLiteral("status pane"))
            .left(k_initial_cols)
            .toLatin1();
    }
    return output;
}

Expected_outcome eligible()
{
    return {Expected_result::ELIGIBLE, term::Terminal_lazy_snapshot_fallback_reason::NONE};
}

Expected_outcome fallback(term::Terminal_lazy_snapshot_fallback_reason reason)
{
    return {Expected_result::FALLBACK, reason};
}

Replay_event output_event(
    const QString&   workload,
    const QString&   name,
    QByteArray       bytes,
    Expected_outcome expected,
    int              causal_input_epoch = 0)
{
    Replay_event event;
    event.workload           = workload;
    event.name               = name;
    event.kind               = Replay_event_kind::BACKEND_OUTPUT;
    event.bytes              = std::move(bytes);
    event.expected           = expected;
    event.causal_input_epoch = causal_input_epoch;
    return event;
}

Replay_event input_event(
    const QString& workload,
    const QString& name,
    QByteArray     bytes)
{
    Replay_event event;
    event.workload        = workload;
    event.name            = name;
    event.kind            = Replay_event_kind::HOST_INPUT;
    event.bytes           = std::move(bytes);
    event.expected.result = Expected_result::NO_SNAPSHOT;
    return event;
}

Replay_event resize_event(
    const QString&             workload,
    const QString&             name,
    term::terminal_grid_size_t grid,
    Expected_outcome           expected)
{
    Replay_event event;
    event.workload    = workload;
    event.name        = name;
    event.kind        = Replay_event_kind::RESIZE;
    event.resize_grid = grid;
    event.expected    = expected;
    return event;
}

Replay_event scroll_event(
    const QString&   workload,
    const QString&   name,
    int              delta,
    Expected_outcome expected)
{
    Replay_event event;
    event.workload     = workload;
    event.name         = name;
    event.kind         = Replay_event_kind::VIEWPORT_SCROLL;
    event.scroll_delta = delta;
    event.expected     = expected;
    return event;
}

std::vector<Replay_event> deterministic_workload()
{
    std::vector<Replay_event> events;

    const QString prompt = QStringLiteral("prompt_typing_echo");
    events.push_back(output_event(
        prompt,
        QStringLiteral("prompt"),
        QByteArrayLiteral("\x1b[2J\x1b[Huser@host C:/work> "),
        fallback(term::Terminal_lazy_snapshot_fallback_reason::MISSING_PREVIOUS_CONTENT_SNAPSHOT)));
    events.push_back(input_event(prompt, QStringLiteral("type_c"), QByteArrayLiteral("c")));
    events.push_back(output_event(prompt, QStringLiteral("echo_c"), QByteArrayLiteral("c"), eligible(), 1));
    events.push_back(input_event(prompt, QStringLiteral("type_o"), QByteArrayLiteral("o")));
    events.push_back(output_event(prompt, QStringLiteral("echo_o"), QByteArrayLiteral("o"), eligible(), 2));
    events.push_back(input_event(prompt, QStringLiteral("type_d"), QByteArrayLiteral("d")));
    events.push_back(output_event(prompt, QStringLiteral("echo_d"), QByteArrayLiteral("d"), eligible(), 3));
    events.push_back(output_event(
        prompt,
        QStringLiteral("command_result"),
        QByteArrayLiteral("\r\naccepted\r\nuser@host C:/work> "),
        eligible(),
        3));

    const QString codex = QStringLiteral("codex_blank_status_rewrites");
    events.push_back(output_event(
        codex,
        QStringLiteral("blank_lines_and_status"),
        QByteArrayLiteral(
            "\r\n\r\n"
            "\x1b[2Kstatus: thinking\r\n"
            "\x1b[1A\x1b[2Kstatus: reading files\r\n"
            "\x1b[2K\r\n"
            "\r\nassistant: ready"),
        eligible()));
    events.push_back(output_event(
        codex,
        QStringLiteral("same_rows_status_rewrite"),
        write_row(6, QByteArrayLiteral("status: replaying deterministic trace")) +
            write_row(8, QByteArrayLiteral("assistant: compared full and lazy frames")),
        eligible()));

    const QString block = QStringLiteral("block_output_scrollback_growth");
    events.push_back(output_event(
        block,
        QStringLiteral("scrollback_burst"),
        numbered_lines("block", 0, 18),
        fallback(term::Terminal_lazy_snapshot_fallback_reason::VIEWPORT_MISMATCH)));
    events.push_back(output_event(
        block,
        QStringLiteral("tail_progress_update"),
        QByteArrayLiteral("\r\x1b[2Kprogress 100%"),
        eligible()));
    events.push_back(scroll_event(
        block,
        QStringLiteral("published_viewport_scroll"),
        3,
        fallback(term::Terminal_lazy_snapshot_fallback_reason::VIEWPORT_MISMATCH)));

    const QString tui = QStringLiteral("fullscreen_tui_updates");
    events.push_back(output_event(
        tui,
        QStringLiteral("enter_alternate_screen"),
        QByteArrayLiteral("\x1b[?1049h\x1b[2J") + tui_frame(0),
        fallback(term::Terminal_lazy_snapshot_fallback_reason::ACTIVE_BUFFER_MISMATCH)));
    events.push_back(output_event(
        tui,
        QStringLiteral("tui_sparse_update"),
        write_row(4, QByteArrayLiteral("TUI frame 01 row 04 | selected item")) +
            write_row(7, QByteArrayLiteral("TUI frame 01 row 07 | logs updated")),
        eligible()));
    events.push_back(output_event(
        tui,
        QStringLiteral("tui_dense_update"),
        tui_frame(2),
        fallback(term::Terminal_lazy_snapshot_fallback_reason::NO_BORROWABLE_ROWS)));
    events.push_back(output_event(
        tui,
        QStringLiteral("exit_alternate_screen"),
        QByteArrayLiteral("\x1b[?1049l"),
        fallback(term::Terminal_lazy_snapshot_fallback_reason::ACTIVE_BUFFER_MISMATCH)));

    const QString metadata = QStringLiteral("hyperlink_style_table_churn");
    events.push_back(output_event(
        metadata,
        QStringLiteral("styled_hyperlink_line"),
        QByteArrayLiteral(
            "\r\n"
            "\x1b[31;1mred\x1b[0m "
            "\x1b]8;id=alpha;https://alpha.example\x1b\\ALPHA\x1b]8;;\x1b\\"),
        fallback(term::Terminal_lazy_snapshot_fallback_reason::VIEWPORT_MISMATCH)));
    events.push_back(output_event(
        metadata,
        QStringLiteral("rewrite_same_line_new_link_style"),
        QByteArrayLiteral(
            "\r\x1b[2K"
            "\x1b[34mblue\x1b[0m "
            "\x1b]8;id=beta;https://beta.example\x1b\\BETA\x1b]8;;\x1b\\"),
        eligible()));

    const QString resize = QStringLiteral("resize_boundary");
    events.push_back(resize_event(
        resize,
        QStringLiteral("grow_grid"),
        {k_resized_rows, k_resized_cols},
        fallback(term::Terminal_lazy_snapshot_fallback_reason::GRID_MISMATCH)));
    events.push_back(output_event(
        resize,
        QStringLiteral("post_resize_content"),
        write_row(2, QByteArrayLiteral("resized grid stable content")) +
            write_row(5, QByteArrayLiteral("lazy replay continues after resize")),
        eligible()));

    return events;
}

term::Terminal_launch_config launch_config()
{
    term::Terminal_launch_config config;
    config.argv              = {QStringLiteral("lazy-snapshot-replay-benchmark")};
    config.working_directory = QStringLiteral("C:/workspace");
    config.initial_grid_size = {k_initial_rows, k_initial_cols};
    return config;
}

std::unique_ptr<term::Terminal_session> make_session(Replay_backend*& out_backend)
{
    auto backend = std::make_unique<Replay_backend>();
    out_backend = backend.get();

    term::Terminal_session_config config;
    config.trace_notification_limit                 = 256U;
    config.trace_resize_limit                       = 64U;
    config.scrollback_limit                         = 256;
    config.recover_scrollback_from_primary_repaints = false;

    auto session = std::make_unique<term::Terminal_session>(std::move(backend), config);
    const term::Terminal_session_result result = session->start(launch_config());
    if (result.code != term::Terminal_session_result_code::ACCEPTED) {
        out_backend = nullptr;
    }
    return session;
}

QSizeF logical_size_for_grid(term::terminal_grid_size_t grid)
{
    return QSizeF(
        static_cast<qreal>(grid.columns) * 8.0,
        static_cast<qreal>(grid.rows)    * 16.0);
}

bool apply_event(
    term::Terminal_session& session,
    Replay_backend&         backend,
    const Replay_event&     event,
    QString*                out_error)
{
    switch (event.kind) {
        case Replay_event_kind::BACKEND_OUTPUT:
            if (!backend.emit_output(event.bytes)) {
                *out_error = QStringLiteral("backend rejected output event %1").arg(event.name);
                return false;
            }
            session.process_backend_callback_events();
            return true;

        case Replay_event_kind::HOST_INPUT: {
            const term::Terminal_session_result result = session.write_user_bytes(event.bytes);
            if (result.code != term::Terminal_session_result_code::ACCEPTED) {
                *out_error = QStringLiteral("session rejected input event %1").arg(event.name);
                return false;
            }
            return true;
        }

        case Replay_event_kind::RESIZE: {
            const term::Terminal_session_result result =
                session.resize(logical_size_for_grid(event.resize_grid), event.resize_grid);
            if (result.code != term::Terminal_session_result_code::ACCEPTED) {
                *out_error = QStringLiteral("session rejected resize event %1").arg(event.name);
                return false;
            }
            return true;
        }

        case Replay_event_kind::VIEWPORT_SCROLL:
            (void)session.scroll_published_viewport_lines(event.scroll_delta);
            return true;
    }

    *out_error = QStringLiteral("unknown event kind");
    return false;
}

QString parity_status(
    const Expected_outcome&                                     expected,
    bool                                                        snapshot_available,
    const term::Terminal_session_lazy_snapshot_composer_result* parity,
    const Compare_result*                                       compare)
{
    if (!snapshot_available) {
        return expected.result == Expected_result::NO_SNAPSHOT
            ? QStringLiteral("expected_no_snapshot")
            : QStringLiteral("missing_snapshot");
    }

    if (parity == nullptr) {
        return QStringLiteral("not_evaluated");
    }

    if (expected.result == Expected_result::FALLBACK) {
        if (!parity->eligible && parity->fallback_reason == expected.fallback) {
            return QStringLiteral("expected_fallback");
        }
        return parity->eligible
            ? QStringLiteral("unexpected_eligible")
            : QStringLiteral("unexpected_fallback");
    }

    if (!parity->eligible) {
        return QStringLiteral("unexpected_fallback");
    }
    if (compare == nullptr || !compare->matches || !parity->materialization_matches_full_snapshot) {
        return QStringLiteral("parity_mismatch");
    }
    return QStringLiteral("eligible_match");
}

bool status_passes(const QString& status)
{
    return
        status == QStringLiteral("eligible_match")        ||
        status == QStringLiteral("expected_fallback")     ||
        status == QStringLiteral("expected_no_snapshot");
}

void update_summary(
    Summary&                                                    summary,
    const Expected_outcome&                                     expected,
    bool                                                        snapshot_available,
    const QString&                                              status,
    const term::Terminal_render_snapshot*                       snapshot,
    const term::Terminal_session_lazy_snapshot_composer_result* parity,
    const term::Terminal_session_lazy_snapshot_composer_result* candidate)
{
    ++summary.events;
    if (!snapshot_available) {
        ++summary.no_snapshot_events;
        return;
    }

    ++summary.snapshot_events;
    if (expected.result == Expected_result::FALLBACK) {
        increment_fallback_reason_counter(
            summary.expected_fallback_reason_counts,
            expected.fallback);
    }
    if (status == QStringLiteral("eligible_match")) {
        ++summary.eligible_events;
    }
    else
    if (status == QStringLiteral("expected_fallback")) {
        ++summary.expected_fallback_events;
        if (parity != nullptr) {
            increment_fallback_reason_counter(
                summary.observed_fallback_reason_counts,
                parity->fallback_reason);
        }
    }
    else
    if (status == QStringLiteral("unexpected_fallback")) {
        ++summary.unexpected_fallback_events;
        ++summary.fallback_reason_mismatch_events;
        if (parity != nullptr) {
            increment_fallback_reason_counter(
                summary.observed_fallback_reason_counts,
                parity->fallback_reason);
        }
    }
    else
    if (status == QStringLiteral("unexpected_eligible")) {
        ++summary.unexpected_eligible_events;
        ++summary.fallback_reason_mismatch_events;
    }
    else
    if (status == QStringLiteral("parity_mismatch")) {
        ++summary.parity_mismatch_events;
    }

    if (snapshot != nullptr) {
        summary.full_cells += row_view_cell_count(*snapshot);
    }
    if (parity != nullptr) {
        summary.lazy_cells_scanned += parity->producer_cells_scanned;
        summary.lazy_cells_emitted += parity->producer_cells_emitted;
        summary.lazy_dirty_rows    += parity->dirty_rows_visible;
        summary.lazy_borrowed_rows += parity->previous_snapshot_borrowed_rows;
    }
    if (candidate != nullptr && candidate->consumer_materialization_calls != 0U) {
        ++summary.candidate_materialized_events;
    }
    if (candidate != nullptr) {
        summary.candidate_materialization_calls +=
            candidate->consumer_materialization_calls;
        summary.candidate_materialization_rows +=
            candidate->consumer_materialization_rows;
        summary.candidate_materialization_cells +=
            candidate->consumer_materialization_cells;
    }
}

QJsonObject event_json_base(
    const Replay_event& event,
    int                 event_index,
    int                 input_epoch,
    int                 output_epoch,
    int                 frame_epoch)
{
    QJsonObject object;
    object.insert(QStringLiteral("event_index"), event_index);
    object.insert(QStringLiteral("workload"), event.workload);
    object.insert(QStringLiteral("name"), event.name);
    object.insert(QStringLiteral("kind"), event_kind_name(event.kind));
    object.insert(QStringLiteral("input_epoch"), input_epoch);
    object.insert(QStringLiteral("output_epoch"), output_epoch);
    object.insert(QStringLiteral("frame_epoch"), frame_epoch);
    object.insert(QStringLiteral("causal_input_epoch"), event.causal_input_epoch);
    object.insert(QStringLiteral("payload_size"), event.bytes.size());
    object.insert(QStringLiteral("payload_hash64"), bytes_hash64(QByteArrayView(event.bytes)));
    object.insert(QStringLiteral("expected"), expected_json(event.expected));
    if (event.kind == Replay_event_kind::RESIZE) {
        object.insert(QStringLiteral("target_grid_size"), grid_json(event.resize_grid));
    }
    if (event.kind == Replay_event_kind::VIEWPORT_SCROLL) {
        object.insert(QStringLiteral("requested_line_delta"), event.scroll_delta);
    }
    return object;
}

Run_result run_replay()
{
    Run_result run;
    Replay_backend* backend = nullptr;
    std::unique_ptr<term::Terminal_session> session = make_session(backend);
    if (backend == nullptr) {
        run.passed = false;
        return run;
    }

    std::shared_ptr<const term::Terminal_render_snapshot> previous_content_snapshot;
    std::shared_ptr<const term::Terminal_render_snapshot> last_snapshot =
        session->latest_render_snapshot_handle();
    int input_epoch  = 0;
    int output_epoch = 0;
    int frame_epoch  = 0;
    const std::vector<Replay_event> events = deterministic_workload();

    for (std::size_t index = 0U; index < events.size(); ++index) {
        const Replay_event& event = events[index];
        if (event.kind == Replay_event_kind::HOST_INPUT) {
            ++input_epoch;
        }
        else
        if (event.kind == Replay_event_kind::BACKEND_OUTPUT) {
            ++output_epoch;
        }

        QJsonObject json = event_json_base(
            event,
            static_cast<int>(index),
            input_epoch,
            output_epoch,
            frame_epoch);

        QString error;
        const Clock::time_point apply_begin = Clock::now();
        const bool applied = apply_event(*session, *backend, event, &error);
        const Clock::time_point apply_end = Clock::now();
        json.insert(QStringLiteral("apply_elapsed_ns"), elapsed_ns(apply_begin, apply_end));
        if (!applied) {
            json.insert(QStringLiteral("status"), QStringLiteral("apply_failed"));
            json.insert(QStringLiteral("error"), error);
            run.events.append(json);
            run.passed = false;
            continue;
        }

        const std::shared_ptr<const term::Terminal_render_snapshot> current =
            session->latest_render_snapshot_handle();
        const bool snapshot_available =
            current != nullptr &&
            (last_snapshot == nullptr ||
             current.get() != last_snapshot.get() ||
             current->metadata.sequence != last_snapshot->metadata.sequence);
        json.insert(QStringLiteral("snapshot_available"), snapshot_available);

        std::optional<term::Terminal_session_lazy_snapshot_composer_result> parity;
        std::optional<term::Terminal_session_lazy_snapshot_composer_result> candidate;
        std::optional<Compare_result> compare;
        QString status;
        if (snapshot_available && current != nullptr) {
            ++frame_epoch;
            json.insert(QStringLiteral("frame_epoch"), frame_epoch);
            json.insert(QStringLiteral("full_snapshot"), snapshot_json(*current));

            const Clock::time_point parity_begin = Clock::now();
            parity =
                session->compose_lazy_render_snapshot_for_testing(
                    previous_content_snapshot,
                    *current);
            const Clock::time_point parity_end = Clock::now();

            const Clock::time_point candidate_begin = Clock::now();
            candidate =
                session->compose_lazy_render_snapshot_for_benchmark_evidence(
                    previous_content_snapshot,
                    *current,
                    term::Terminal_lazy_snapshot_evidence_mode::
                        PUBLICATION_CANDIDATE_NO_MATERIALIZATION);
            const Clock::time_point candidate_end = Clock::now();

            if (parity->lazy_snapshot.has_value()) {
                compare = compare_lazy_to_full(*current, *parity->lazy_snapshot);
            }

            json.insert(QStringLiteral("parity_elapsed_ns"), elapsed_ns(parity_begin, parity_end));
            json.insert(
                QStringLiteral("publication_candidate_elapsed_ns"),
                elapsed_ns(candidate_begin, candidate_end));
            json.insert(QStringLiteral("parity"), lazy_result_json(*parity, compare ? &*compare : nullptr));
            json.insert(QStringLiteral("publication_candidate"), lazy_result_json(*candidate, nullptr));
            status = parity_status(event.expected, true, &*parity, compare ? &*compare : nullptr);

            if (current->basis == term::Terminal_render_snapshot_basis::LIVE_CONTENT &&
                current->purpose == term::Terminal_render_snapshot_purpose::CONTENT)
            {
                previous_content_snapshot = current;
            }
            last_snapshot = current;
            session->mark_render_snapshot_synced(session->render_snapshot_generation());
        }
        else {
            status = parity_status(event.expected, false, nullptr, nullptr);
        }

        json.insert(QStringLiteral("status"), status);
        run.passed = run.passed && status_passes(status);
        if (candidate.has_value() && candidate->consumer_materialization_calls != 0U) {
            run.passed = false;
        }

        update_summary(
            run.summary,
            event.expected,
            snapshot_available,
            status,
            snapshot_available && current != nullptr ? current.get() : nullptr,
            parity ? &*parity : nullptr,
            candidate ? &*candidate : nullptr);
        run.events.append(json);
    }

    return run;
}

QJsonObject summary_json(const Summary& summary, bool passed)
{
    const bool replay_parity_ready =
        summary.unexpected_fallback_events == 0 &&
        summary.unexpected_eligible_events == 0 &&
        summary.parity_mismatch_events == 0;
    const bool fallback_reason_ready =
        summary.fallback_reason_mismatch_events == 0;
    const bool candidate_materialization_ready =
        summary.candidate_materialized_events == 0 &&
        summary.candidate_materialization_calls == 0U &&
        summary.candidate_materialization_rows  == 0U &&
        summary.candidate_materialization_cells == 0U;

    QJsonObject object;
    object.insert(QStringLiteral("passed"), passed);
    object.insert(QStringLiteral("replay_parity_ready"), replay_parity_ready);
    object.insert(QStringLiteral("fallback_reason_ready"), fallback_reason_ready);
    object.insert(
        QStringLiteral("candidate_materialization_ready"),
        candidate_materialization_ready);
    object.insert(QStringLiteral("events"), summary.events);
    object.insert(QStringLiteral("snapshot_events"), summary.snapshot_events);
    object.insert(QStringLiteral("no_snapshot_events"), summary.no_snapshot_events);
    object.insert(QStringLiteral("eligible_events"), summary.eligible_events);
    object.insert(QStringLiteral("expected_fallback_events"), summary.expected_fallback_events);
    object.insert(QStringLiteral("unexpected_fallback_events"), summary.unexpected_fallback_events);
    object.insert(QStringLiteral("unexpected_eligible_events"), summary.unexpected_eligible_events);
    object.insert(QStringLiteral("parity_mismatch_events"), summary.parity_mismatch_events);
    object.insert(
        QStringLiteral("candidate_materialized_events"),
        summary.candidate_materialized_events);
    object.insert(
        QStringLiteral("fallback_reason_mismatch_events"),
        summary.fallback_reason_mismatch_events);
    insert_u64(
        object,
        QStringLiteral("candidate_materialization_calls"),
        summary.candidate_materialization_calls);
    insert_u64(
        object,
        QStringLiteral("candidate_materialization_rows"),
        summary.candidate_materialization_rows);
    insert_u64(
        object,
        QStringLiteral("candidate_materialization_cells"),
        summary.candidate_materialization_cells);
    insert_u64(object, QStringLiteral("full_cells"), summary.full_cells);
    insert_u64(object, QStringLiteral("lazy_cells_scanned"), summary.lazy_cells_scanned);
    insert_u64(object, QStringLiteral("lazy_cells_emitted"), summary.lazy_cells_emitted);
    insert_u64(object, QStringLiteral("lazy_dirty_rows"), summary.lazy_dirty_rows);
    insert_u64(object, QStringLiteral("lazy_borrowed_rows"), summary.lazy_borrowed_rows);
    object.insert(
        QStringLiteral("expected_fallback_reason_counts"),
        fallback_reason_counts_json(summary.expected_fallback_reason_counts));
    object.insert(
        QStringLiteral("observed_fallback_reason_counts"),
        fallback_reason_counts_json(summary.observed_fallback_reason_counts));
    return object;
}

QJsonObject decision_criteria_json()
{
    QJsonObject object;
    object.insert(QStringLiteral("eligible_frame"), QStringLiteral(
        "parity composer must be eligible, validate, materialize to the full snapshot, "
        "and match row text, cells, metadata, styles, hyperlinks, cursor, viewport, "
        "mode, and provenance observations"));
    object.insert(QStringLiteral("fallback_frame"), QStringLiteral(
        "fallback is accepted only when the event expectation names the exact "
        "Terminal_lazy_snapshot_fallback_reason observed by the composer"));
    object.insert(QStringLiteral("publication_candidate"), QStringLiteral(
        "publication_candidate_no_materialization must report zero consumer "
        "materialization calls, rows, and cells for every event"));
    object.insert(QStringLiteral("timing"), QStringLiteral(
        "elapsed_ns fields are evidence, not pass criteria; failures are never hidden "
        "behind aggregate duration"));
    return object;
}

QJsonObject input_echo_contract_json()
{
    QJsonObject object;
    object.insert(QStringLiteral("proposal"), QStringLiteral(
        "input_output_epoch_same_frame_eligibility"));
    object.insert(QStringLiteral("input_epoch"), QStringLiteral(
        "incremented when host input is accepted and written to the backend"));
    object.insert(QStringLiteral("output_epoch"), QStringLiteral(
        "incremented when backend output is accepted into the session"));
    object.insert(QStringLiteral("frame_epoch"), QStringLiteral(
        "incremented only when a new render snapshot is published"));
    object.insert(QStringLiteral("same_frame_eligibility"), QStringLiteral(
        "a frame may include echo output only when all backend output callbacks accepted "
        "before the frame publication cutoff have been drained into the session; output "
        "that declares or infers causal_input_epoch equal to the latest accepted input_epoch "
        "is eligible in that frame"));
    object.insert(QStringLiteral("rejected_fix_shape"), QStringLiteral(
        "do not add production sleeps or arbitrary echo delays; the publication decision "
        "must be based on deterministic callback drain and epoch state"));
    return object;
}

QJsonArray workload_names_json()
{
    QJsonArray array;
    QStringList names;
    for (const Replay_event& event : deterministic_workload()) {
        if (!names.contains(event.workload)) {
            names.push_back(event.workload);
            array.append(event.workload);
        }
    }
    return array;
}

QJsonObject build_root_json(const Run_result& run)
{
    QJsonObject config;
    config.insert(QStringLiteral("initial_grid_size"), grid_json({k_initial_rows, k_initial_cols}));
    config.insert(QStringLiteral("resized_grid_size"), grid_json({k_resized_rows, k_resized_cols}));
    config.insert(QStringLiteral("scrollback_limit"), 256);
    config.insert(QStringLiteral("primary_repaint_recovery"), false);

    QJsonObject root;
    root.insert(QStringLiteral("schema"), k_schema_name);
    root.insert(QStringLiteral("schema_version"), k_schema_version);
    root.insert(
        QStringLiteral("status"),
        run.passed ? QStringLiteral("ok") : QStringLiteral("failed"));
    root.insert(QStringLiteral("measurement_boundary"), k_measurement_boundary);
    root.insert(QStringLiteral("config"), config);
    root.insert(QStringLiteral("workloads"), workload_names_json());
    root.insert(QStringLiteral("decision_criteria"), decision_criteria_json());
    root.insert(QStringLiteral("input_echo_contract_candidate"), input_echo_contract_json());
    root.insert(QStringLiteral("summary"), summary_json(run.summary, run.passed));
    root.insert(QStringLiteral("events"), run.events);
    return root;
}

bool validate_json_root(const QJsonObject& root, QString* out_error)
{
    if (root.value(QStringLiteral("schema")).toString() != k_schema_name ||
        root.value(QStringLiteral("schema_version")).toInt() != k_schema_version)
    {
        *out_error = QStringLiteral("root schema fields are invalid");
        return false;
    }
    if (root.value(QStringLiteral("status")).toString() != QStringLiteral("ok")) {
        *out_error = QStringLiteral("replay root status is not ok");
        return false;
    }

    const QJsonObject summary = root.value(QStringLiteral("summary")).toObject();
    if (!summary.value(QStringLiteral("passed")).toBool()) {
        *out_error = QStringLiteral("replay summary did not pass");
        return false;
    }
    if (!summary.value(QStringLiteral("replay_parity_ready")).toBool() ||
        !summary.value(QStringLiteral("fallback_reason_ready")).toBool() ||
        !summary.value(QStringLiteral("candidate_materialization_ready")).toBool())
    {
        *out_error = QStringLiteral("replay summary readiness flags are not all true");
        return false;
    }
    if (summary.value(QStringLiteral("fallback_reason_mismatch_events")).toInt() != 0) {
        *out_error = QStringLiteral("fallback reason mismatches were recorded");
        return false;
    }
    if (summary.value(QStringLiteral("candidate_materialization_calls")).toInt() != 0 ||
        summary.value(QStringLiteral("candidate_materialization_rows")).toInt()  != 0 ||
        summary.value(QStringLiteral("candidate_materialization_cells")).toInt() != 0)
    {
        *out_error = QStringLiteral("candidate materialization aggregates are nonzero");
        return false;
    }
    const QJsonObject expected_fallbacks =
        summary.value(QStringLiteral("expected_fallback_reason_counts")).toObject();
    const QJsonObject observed_fallbacks =
        summary.value(QStringLiteral("observed_fallback_reason_counts")).toObject();
    for (const term::terminal_lazy_snapshot_fallback_reason_descriptor_t& descriptor :
        term::terminal_lazy_snapshot_fallback_reason_descriptors())
    {
        const QString key = QString::fromLatin1(descriptor.key);
        if (expected_fallbacks.value(key).toInt() != observed_fallbacks.value(key).toInt()) {
            *out_error = QStringLiteral("fallback reason count mismatch for %1").arg(key);
            return false;
        }
    }

    const QJsonArray events = root.value(QStringLiteral("events")).toArray();
    int snapshot_events = 0;
    int eligible_events = 0;
    int fallback_events = 0;
    for (const QJsonValue& value : events) {
        const QJsonObject event = value.toObject();
        const QString status = event.value(QStringLiteral("status")).toString();
        if (!status_passes(status)) {
            *out_error =
                QStringLiteral("event %1 failed parity criteria")
                    .arg(event.value(QStringLiteral("event_index")).toInt());
            return false;
        }
        if (event.value(QStringLiteral("snapshot_available")).toBool()) {
            ++snapshot_events;
            if (status == QStringLiteral("eligible_match")) {
                ++eligible_events;
            }
            if (status == QStringLiteral("expected_fallback")) {
                ++fallback_events;
            }

            const QJsonObject candidate =
                event.value(QStringLiteral("publication_candidate")).toObject();
            if (candidate.value(QStringLiteral("consumer_materialization_calls")).toInt() != 0 ||
                candidate.value(QStringLiteral("consumer_materialization_rows")).toInt()  != 0 ||
                candidate.value(QStringLiteral("consumer_materialization_cells")).toInt() != 0)
            {
                *out_error =
                    QStringLiteral("event %1 candidate materialized rows")
                        .arg(event.value(QStringLiteral("event_index")).toInt());
                return false;
            }
        }
    }

    if (snapshot_events < 12 || eligible_events < 6 || fallback_events < 5) {
        *out_error = QStringLiteral("representative event coverage is too small");
        return false;
    }

    return true;
}

bool parse_options(const QStringList& args, Options* options, QString* out_error)
{
    for (int i = 1; i < args.size(); ++i) {
        const QString arg = args[i];
        const auto require_value = [&]() -> std::optional<QString> {
            if (i + 1 >= args.size()) {
                *out_error = QStringLiteral("%1 requires a value").arg(arg);
                return std::nullopt;
            }
            ++i;
            return args[i];
        };

        if (arg == QStringLiteral("--help") || arg == QStringLiteral("-h")) {
            options->help = true;
            return true;
        }
        else
        if (arg == QStringLiteral("--output")) {
            const std::optional<QString> value = require_value();
            if (!value.has_value()) {
                return false;
            }
            options->output_path = *value;
        }
        else
        if (arg == QStringLiteral("--quiet")) {
            options->quiet = true;
        }
        else
        if (arg == QStringLiteral("--validate-json")) {
            options->validate_json = true;
        }
        else {
            *out_error = QStringLiteral("unknown argument '%1'").arg(arg);
            return false;
        }
    }

    return true;
}

void print_usage()
{
    std::cout
        << "Usage: vnm_terminal_lazy_snapshot_replay_benchmark [options]\n"
        << "  --output PATH     write JSON evidence to PATH instead of stdout\n"
        << "  --quiet           suppress status output on stderr\n"
        << "  --validate-json   fail if schema, coverage, or parity criteria fail\n";
}

bool write_json_output(const QJsonObject& root, const QString& output_path, QString* out_error)
{
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (output_path.isEmpty()) {
        std::cout << json.constData();
        return true;
    }

    QSaveFile file(output_path);
    if (!file.open(QIODevice::WriteOnly)) {
        *out_error = QStringLiteral("failed to open output file '%1'").arg(output_path);
        return false;
    }
    if (file.write(json) != json.size()) {
        *out_error = QStringLiteral("failed to write output file '%1'").arg(output_path);
        return false;
    }
    if (!file.commit()) {
        *out_error = QStringLiteral("failed to commit output file '%1'").arg(output_path);
        return false;
    }

    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    Options options;
    QString error;
    if (!parse_options(app.arguments(), &options, &error)) {
        std::cerr << error.toStdString() << '\n';
        print_usage();
        return 2;
    }
    if (options.help) {
        print_usage();
        return 0;
    }

    if (!options.quiet) {
        std::cerr << "running deterministic lazy snapshot replay benchmark\n";
    }

    const Run_result run = run_replay();
    const QJsonObject root = build_root_json(run);
    if (options.validate_json && !validate_json_root(root, &error)) {
        std::cerr << error.toStdString() << '\n';
        if (!options.output_path.isEmpty()) {
            QString write_error;
            (void)write_json_output(root, options.output_path, &write_error);
        }
        return 1;
    }

    if (!write_json_output(root, options.output_path, &error)) {
        std::cerr << error.toStdString() << '\n';
        return 1;
    }

    return run.passed ? 0 : 1;
}
