#include "vnm_terminal/internal/qsg_terminal_render_frame.h"
#include "vnm_terminal/internal/terminal_session.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSizeF>
#include <QStringList>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

constexpr const char* k_schema = "vnm_terminal_lazy_snapshot_evidence_benchmark";
constexpr int k_schema_version = 4;

enum class Case_kind
{
    MOSTLY_CLEAN_SCROLLBACK,
    SPARSE_DIRTY_ROWS,
    FULL_REPAINT,
    STYLE_FALLBACK,
    HYPERLINK_FALLBACK,
    WIDE_COMBINING,
    RESIZE_BOUNDARY,
    VIEWPORT_BOUNDARY,
};

enum class Text_pattern
{
    ASCII,
    BLOCK,
    WIDE_COMBINING,
    HYPERLINK,
};

struct options_t
{
    int         frames        = 18;
    int         warmup_frames = 3;
    int         repeats       = 3;
    QStringList cases;
    QString     output_path;
    bool        quiet         = false;
    bool        validate_json = false;
    bool        help          = false;
};

struct case_t
{
    QString                                  name;
    Case_kind                                kind = Case_kind::SPARSE_DIRTY_ROWS;
    Text_pattern                             pattern = Text_pattern::ASCII;
    term::terminal_grid_size_t               grid{24, 80};
    int                                      scrollback_lines = 0;
    int                                      dirty_rows = 3;
    int                                      stride = 7;
    int                                      style_period = 0;
    bool                                     expect_eligible = true;
    term::Terminal_lazy_snapshot_fallback_reason expected_fallback =
        term::Terminal_lazy_snapshot_fallback_reason::NONE;
};

struct counters_t
{
    std::uint64_t frames = 0U;
    std::uint64_t full_validation_failures = 0U;
    std::uint64_t lazy_validation_failures = 0U;
    std::uint64_t materialization_mismatches = 0U;
    std::uint64_t full_walk_frame_mismatches = 0U;
    std::uint64_t candidate_consumer_materialization_frames = 0U;
    std::uint64_t unexpected_eligibility = 0U;
    std::uint64_t unexpected_fallback = 0U;
    std::uint64_t missing_snapshots = 0U;
};

struct fallback_counts_t
{
    std::uint64_t missing_previous_content_snapshot = 0U;
    std::uint64_t grid_mismatch = 0U;
    std::uint64_t viewport_mismatch = 0U;
    std::uint64_t active_buffer_mismatch = 0U;
    std::uint64_t public_projection = 0U;
    std::uint64_t row_origin_generation_mismatch = 0U;
    std::uint64_t style_color_mode_incompatibility = 0U;
    std::uint64_t hyperlink_namespace_incompatibility = 0U;
    std::uint64_t unstable_dirty_row_mutation_identity = 0U;
    std::uint64_t no_borrowable_rows = 0U;
    std::uint64_t unsupported_geometry_or_detached_snapshot_path = 0U;
};

struct timings_t
{
    qint64 full_update_ns = 0;
    qint64 full_frame_ns = 0;
    qint64 parity_compose_ns = 0;
    qint64 candidate_compose_ns = 0;
    qint64 lazy_sparse_frame_ns = 0;
    qint64 lazy_full_walk_frame_ns = 0;
};

struct metrics_t
{
    std::uint64_t full_snapshot_cells = 0U;
    std::uint64_t full_snapshot_capacity_bytes = 0U;
    std::uint64_t dirty_rows_visible = 0U;
    std::uint64_t full_frame_input_cells = 0U;
    std::uint64_t full_frame_row_descriptors = 0U;
    std::uint64_t full_frame_layer_descriptors = 0U;
    std::uint64_t borrowed_rows = 0U;
    std::uint64_t producer_owned_rows = 0U;
    std::uint64_t producer_materialized_rows = 0U;
    std::uint64_t producer_cells_scanned = 0U;
    std::uint64_t producer_cells_emitted = 0U;
    std::uint64_t consumer_materialization_calls = 0U;
    std::uint64_t consumer_materialization_rows = 0U;
    std::uint64_t consumer_materialization_cells = 0U;
    std::uint64_t lazy_sparse_frame_input_cells = 0U;
    std::uint64_t lazy_sparse_frame_row_descriptors = 0U;
    std::uint64_t lazy_sparse_frame_layer_descriptors = 0U;
    std::uint64_t lazy_full_walk_frame_input_cells = 0U;
    std::uint64_t lazy_full_walk_frame_row_descriptors = 0U;
    std::uint64_t eligible_lazy_candidates = 0U;
    std::uint64_t memory_capacity_failed_candidates = 0U;
    std::uint64_t lazy_flat_cell_capacity_bytes = 0U;
    std::uint64_t lazy_payload_row_capacity_bytes = 0U;
    std::uint64_t lazy_unique_owner_retained_bytes = 0U;
    std::uint64_t retained_previous_snapshot_flat_cell_capacity_bytes = 0U;
    std::uint64_t retained_previous_snapshot_total_capacity_bytes = 0U;
};

struct repeat_t
{
    int               index = 0;
    QString           status = QStringLiteral("ok");
    counters_t        counters;
    fallback_counts_t fallbacks;
    timings_t         timings;
    metrics_t         metrics;
};

struct case_result_t
{
    case_t                config;
    QString               status = QStringLiteral("ok");
    std::vector<repeat_t> repeats;
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
        m_writes.push_back(std::move(bytes));
        return term::backend_accept();
    }

    term::Terminal_backend_result resize(
        term::Terminal_backend_resize_request request) override
    {
        m_resizes.push_back(request);
        return term::backend_accept();
    }

    term::Terminal_backend_result set_output_paused(bool paused) override
    {
        m_pause_requests.push_back(paused);
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
        if (!m_running || !m_callbacks.output_received) {
            return false;
        }

        m_callbacks.output_received(std::move(bytes));
        return true;
    }

private:
    bool                             m_running = false;
    term::Terminal_backend_callbacks m_callbacks;
    std::vector<QByteArray>          m_writes;
    std::vector<term::Terminal_backend_resize_request> m_resizes;
    std::vector<bool>                m_pause_requests;
};

struct session_fixture_t
{
    std::unique_ptr<term::Terminal_session> session;
    Replay_backend*                         backend = nullptr;
};

QString kind_name(Case_kind kind)
{
    switch (kind) {
        case Case_kind::MOSTLY_CLEAN_SCROLLBACK: return QStringLiteral("mostly_clean_scrollback");
        case Case_kind::SPARSE_DIRTY_ROWS:       return QStringLiteral("sparse_dirty_rows");
        case Case_kind::FULL_REPAINT:            return QStringLiteral("full_repaint");
        case Case_kind::STYLE_FALLBACK:          return QStringLiteral("style_fallback");
        case Case_kind::HYPERLINK_FALLBACK:      return QStringLiteral("hyperlink_fallback");
        case Case_kind::WIDE_COMBINING:          return QStringLiteral("wide_combining");
        case Case_kind::RESIZE_BOUNDARY:         return QStringLiteral("resize_boundary");
        case Case_kind::VIEWPORT_BOUNDARY:       return QStringLiteral("viewport_boundary");
    }

    return QStringLiteral("unknown");
}

QString pattern_name(Text_pattern pattern)
{
    switch (pattern) {
        case Text_pattern::ASCII:          return QStringLiteral("ascii");
        case Text_pattern::BLOCK:          return QStringLiteral("block");
        case Text_pattern::WIDE_COMBINING: return QStringLiteral("wide_combining");
        case Text_pattern::HYPERLINK:      return QStringLiteral("hyperlink");
    }

    return QStringLiteral("unknown");
}

QString fallback_name(term::Terminal_lazy_snapshot_fallback_reason reason)
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

void record_fallback(
    fallback_counts_t& counts,
    term::Terminal_lazy_snapshot_fallback_reason reason)
{
    switch (reason) {
        case term::Terminal_lazy_snapshot_fallback_reason::NONE:
            return;
        case term::Terminal_lazy_snapshot_fallback_reason::MISSING_PREVIOUS_CONTENT_SNAPSHOT:
            ++counts.missing_previous_content_snapshot;
            return;
        case term::Terminal_lazy_snapshot_fallback_reason::GRID_MISMATCH:
            ++counts.grid_mismatch;
            return;
        case term::Terminal_lazy_snapshot_fallback_reason::VIEWPORT_MISMATCH:
            ++counts.viewport_mismatch;
            return;
        case term::Terminal_lazy_snapshot_fallback_reason::ACTIVE_BUFFER_MISMATCH:
            ++counts.active_buffer_mismatch;
            return;
        case term::Terminal_lazy_snapshot_fallback_reason::PUBLIC_PROJECTION:
            ++counts.public_projection;
            return;
        case term::Terminal_lazy_snapshot_fallback_reason::ROW_ORIGIN_GENERATION_MISMATCH:
            ++counts.row_origin_generation_mismatch;
            return;
        case term::Terminal_lazy_snapshot_fallback_reason::STYLE_COLOR_MODE_INCOMPATIBILITY:
            ++counts.style_color_mode_incompatibility;
            return;
        case term::Terminal_lazy_snapshot_fallback_reason::HYPERLINK_NAMESPACE_INCOMPATIBILITY:
            ++counts.hyperlink_namespace_incompatibility;
            return;
        case term::Terminal_lazy_snapshot_fallback_reason::UNSTABLE_DIRTY_ROW_MUTATION_IDENTITY:
            ++counts.unstable_dirty_row_mutation_identity;
            return;
        case term::Terminal_lazy_snapshot_fallback_reason::NO_BORROWABLE_ROWS:
            ++counts.no_borrowable_rows;
            return;
        case term::Terminal_lazy_snapshot_fallback_reason::
            UNSUPPORTED_GEOMETRY_OR_DETACHED_SNAPSHOT_PATH:
            ++counts.unsupported_geometry_or_detached_snapshot_path;
            return;
    }
}

std::uint64_t fallback_count(
    const fallback_counts_t& counts,
    term::Terminal_lazy_snapshot_fallback_reason reason)
{
    switch (reason) {
        case term::Terminal_lazy_snapshot_fallback_reason::GRID_MISMATCH:
            return counts.grid_mismatch;
        case term::Terminal_lazy_snapshot_fallback_reason::VIEWPORT_MISMATCH:
            return counts.viewport_mismatch;
        case term::Terminal_lazy_snapshot_fallback_reason::STYLE_COLOR_MODE_INCOMPATIBILITY:
            return counts.style_color_mode_incompatibility;
        case term::Terminal_lazy_snapshot_fallback_reason::HYPERLINK_NAMESPACE_INCOMPATIBILITY:
            return counts.hyperlink_namespace_incompatibility;
        case term::Terminal_lazy_snapshot_fallback_reason::NO_BORROWABLE_ROWS:
            return counts.no_borrowable_rows;
        case term::Terminal_lazy_snapshot_fallback_reason::
            UNSUPPORTED_GEOMETRY_OR_DETACHED_SNAPSHOT_PATH:
            return counts.unsupported_geometry_or_detached_snapshot_path;
        default:
            return 0U;
    }
}

term::terminal_cell_metrics_t cell_metrics()
{
    term::terminal_cell_metrics_t metrics;
    metrics.width   = 8.0;
    metrics.height  = 16.0;
    metrics.ascent  = 12.0;
    metrics.descent = 4.0;
    return metrics;
}

QSizeF logical_size(term::terminal_grid_size_t grid)
{
    const term::terminal_cell_metrics_t metrics = cell_metrics();
    return QSizeF(
        metrics.width * static_cast<qreal>(grid.columns),
        metrics.height * static_cast<qreal>(grid.rows));
}

QByteArray cursor_to(int row, int column)
{
    return QStringLiteral("\x1b[%1;%2H").arg(row).arg(column).toLatin1();
}

QByteArray clear_row(int row)
{
    return cursor_to(row, 1) + QByteArrayLiteral("\x1b[2K");
}

int positive_mod(int value, int modulus)
{
    const int result = value % modulus;
    return result < 0 ? result + modulus : result;
}

QChar ascii_char(int seed)
{
    return QChar(static_cast<ushort>(u'A' + positive_mod(seed, 26)));
}

QString wide_glyph()
{
    return QString::fromUtf8("\xE7\x95\x8C");
}

QString combining_glyph()
{
    return QString::fromUtf8("e\xCC\x81");
}

void append_text_unit(QString& text, int& column, int columns, QString unit, int width)
{
    if (column + width <= columns) {
        text += unit;
        column += width;
        return;
    }

    text += QChar(u' ');
    ++column;
}

QString row_text(const case_t& config, int frame, int row)
{
    QString text;
    text.reserve(config.grid.columns);
    int column = 0;
    while (column < config.grid.columns) {
        const int seed = frame * 19 + row * 31 + column * 7;
        switch (config.pattern) {
            case Text_pattern::ASCII:
            case Text_pattern::HYPERLINK:
                text += ascii_char(seed);
                ++column;
                break;
            case Text_pattern::BLOCK:
                text += QChar(static_cast<ushort>(0x2588U));
                ++column;
                break;
            case Text_pattern::WIDE_COMBINING: {
                const int selector = positive_mod(seed, 13);
                if (selector == 0) {
                    append_text_unit(text, column, config.grid.columns, wide_glyph(), 2);
                }
                else
                if (selector == 1) {
                    append_text_unit(text, column, config.grid.columns, combining_glyph(), 1);
                }
                else {
                    text += ascii_char(seed);
                    ++column;
                }
                break;
            }
        }
    }
    return text;
}

QByteArray row_write(const case_t& config, int frame, int row)
{
    QByteArray out = clear_row(row + 1);
    if (config.style_period > 0) {
        const int color =
            30 + positive_mod(row + frame, config.style_period) % 8;
        out += "\x1b[";
        out += QByteArray::number(color);
        out += 'm';
    }

    if (config.pattern == Text_pattern::HYPERLINK) {
        out += "\x1b]8;id=stable-link;https://lazy.varinomics.example/";
        out += QByteArray::number(row);
        out += "\x1b\\";
        out += row_text(config, frame, row).toUtf8();
        out += "\x1b]8;;\x1b\\";
    }
    else {
        out += row_text(config, frame, row).toUtf8();
    }
    out += "\x1b[0m";
    return out;
}

QByteArray baseline_output(const case_t& config)
{
    QByteArray out;
    for (int row = 0; row < config.scrollback_lines; ++row) {
        out += QStringLiteral("history-%1\r\n")
            .arg(row, 4, 10, QLatin1Char('0'))
            .toLatin1();
    }
    for (int row = 0; row < config.grid.rows; ++row) {
        out += row_write(config, 0, row);
    }
    return out;
}

std::vector<int> dirty_rows_for_frame(const case_t& config, int frame)
{
    if (config.kind == Case_kind::FULL_REPAINT || config.dirty_rows >= config.grid.rows) {
        std::vector<int> rows(static_cast<std::size_t>(config.grid.rows));
        for (int row = 0; row < config.grid.rows; ++row) {
            rows[static_cast<std::size_t>(row)] = row;
        }
        return rows;
    }

    std::vector<bool> used(static_cast<std::size_t>(config.grid.rows), false);
    std::vector<int> rows;
    rows.reserve(static_cast<std::size_t>(config.dirty_rows));
    int probe = (frame + 1) * 17;
    while (static_cast<int>(rows.size()) < config.dirty_rows) {
        const int candidate =
            positive_mod(
                probe * config.stride + (frame + 3) * 11,
                config.grid.rows);
        int row = candidate;
        if (used[static_cast<std::size_t>(row)]) {
            for (int offset = 1; offset < config.grid.rows; ++offset) {
                const int fallback = (candidate + offset) % config.grid.rows;
                if (!used[static_cast<std::size_t>(fallback)]) {
                    row = fallback;
                    break;
                }
            }
        }
        if (!used[static_cast<std::size_t>(row)]) {
            used[static_cast<std::size_t>(row)] = true;
            rows.push_back(row);
        }
        ++probe;
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

QByteArray frame_output(const case_t& config, int frame)
{
    QByteArray out;
    for (const int row : dirty_rows_for_frame(config, frame)) {
        out += row_write(config, frame + 1, row);
    }
    return out;
}

session_fixture_t make_session(term::terminal_grid_size_t grid)
{
    auto backend = std::make_unique<Replay_backend>();
    Replay_backend* backend_ptr = backend.get();

    term::Terminal_session_config config;
    config.trace_notification_limit = 128U;
    config.trace_resize_limit = 32U;
    config.scrollback_limit = 256;
    config.recover_scrollback_from_primary_repaints = false;

    auto session =
        std::make_unique<term::Terminal_session>(std::move(backend), config);

    term::Terminal_launch_config launch;
    launch.argv = {QStringLiteral("lazy-snapshot-evidence")};
    launch.working_directory = QStringLiteral("C:/workspace");
    launch.initial_grid_size = grid;

    const term::Terminal_session_result start = session->start(launch);
    if (start.code != term::Terminal_session_result_code::ACCEPTED) {
        return {};
    }
    return {std::move(session), backend_ptr};
}

std::uint64_t dirty_rows_visible(const term::Terminal_render_snapshot& snapshot)
{
    std::uint64_t total = 0U;
    for (const term::Terminal_render_dirty_row_range& range : snapshot.dirty_row_ranges) {
        const int first = std::clamp(range.first_row, 0, snapshot.grid_size.rows);
        const int last = std::clamp(range.first_row + range.row_count, first, snapshot.grid_size.rows);
        total += static_cast<std::uint64_t>(last - first);
    }
    return total;
}

std::uint64_t flat_capacity_bytes(const term::Terminal_render_snapshot& snapshot)
{
    return
        static_cast<std::uint64_t>(snapshot.cells.capacity()) *
        static_cast<std::uint64_t>(sizeof(term::Terminal_render_cell));
}

std::uint64_t byte_array_capacity_bytes(const QByteArray& value)
{
    return static_cast<std::uint64_t>(value.capacity());
}

std::uint64_t string_capacity_bytes(const QString& value)
{
    return
        static_cast<std::uint64_t>(sizeof(QString)) +
        static_cast<std::uint64_t>(value.capacity()) *
            static_cast<std::uint64_t>(sizeof(QChar));
}

std::uint64_t cell_text_capacity_bytes(const term::Terminal_render_cell_text& text)
{
    const QString* fallback = text.fallback_qstring_or_null();
    return fallback != nullptr ? string_capacity_bytes(*fallback) : 0U;
}

std::uint64_t snapshot_retained_capacity_bytes(
    const term::Terminal_render_snapshot& snapshot)
{
    std::uint64_t bytes = flat_capacity_bytes(snapshot);
    bytes +=
        static_cast<std::uint64_t>(snapshot.styles.capacity()) *
        static_cast<std::uint64_t>(sizeof(term::Terminal_text_style));
    bytes +=
        static_cast<std::uint64_t>(snapshot.visible_line_provenance.capacity()) *
        static_cast<std::uint64_t>(sizeof(term::Terminal_render_line_provenance));
    bytes +=
        static_cast<std::uint64_t>(snapshot.dirty_row_ranges.capacity()) *
        static_cast<std::uint64_t>(sizeof(term::Terminal_render_dirty_row_range));
    bytes +=
        static_cast<std::uint64_t>(snapshot.hyperlinks.capacity()) *
        static_cast<std::uint64_t>(sizeof(term::Terminal_render_hyperlink_metadata));
    bytes +=
        static_cast<std::uint64_t>(snapshot.selection_spans.capacity()) *
        static_cast<std::uint64_t>(sizeof(term::Terminal_render_selection_span));
    for (const term::Terminal_render_hyperlink_metadata& hyperlink : snapshot.hyperlinks) {
        bytes += byte_array_capacity_bytes(hyperlink.identity_key);
        bytes += byte_array_capacity_bytes(hyperlink.uri);
    }

    const term::Terminal_render_snapshot_row_content_view rows(snapshot);
    for (const term::Terminal_render_snapshot_row_content row : rows) {
        for (const term::Terminal_render_cell& cell : row) {
            bytes += cell_text_capacity_bytes(cell.text);
        }
    }
    return bytes;
}

std::uint64_t lazy_payload_row_capacity_bytes(
    const term::Terminal_render_snapshot& snapshot)
{
    if (snapshot.lazy_row_payloads == nullptr) {
        return 0U;
    }
    return
        static_cast<std::uint64_t>(snapshot.lazy_row_payloads->rows.capacity()) *
        static_cast<std::uint64_t>(
            sizeof(term::Terminal_render_snapshot_lazy_row_payload));
}

std::uint64_t lazy_owner_bytes(const term::Terminal_render_snapshot& snapshot)
{
    if (snapshot.lazy_row_payloads == nullptr) {
        return 0U;
    }

    std::set<const term::Terminal_render_snapshot_row_payload_owner*> owners;
    std::uint64_t bytes = 0U;
    for (const term::Terminal_render_snapshot_lazy_row_payload& row :
        snapshot.lazy_row_payloads->rows)
    {
        const term::Terminal_render_snapshot_row_payload_owner* owner =
            row.source.owner.get();
        if (owner != nullptr && owners.insert(owner).second) {
            bytes += static_cast<std::uint64_t>(owner->retained_bytes());
        }
        if (row.cells_in_receiving_namespace != nullptr) {
            bytes +=
                static_cast<std::uint64_t>(
                    row.cells_in_receiving_namespace->capacity()) *
                static_cast<std::uint64_t>(sizeof(term::Terminal_render_cell));
        }
    }
    return bytes;
}

struct previous_snapshot_capacity_t
{
    std::uint64_t flat_cell_capacity_bytes = 0U;
    std::uint64_t total_capacity_bytes     = 0U;
};

previous_snapshot_capacity_t retained_previous_snapshot_capacity(
    const term::Terminal_render_snapshot& snapshot)
{
    previous_snapshot_capacity_t result;
    if (snapshot.lazy_row_payloads == nullptr) {
        return result;
    }

    std::set<const term::Terminal_render_snapshot*> snapshots;
    for (const term::Terminal_render_snapshot_lazy_row_payload& row :
        snapshot.lazy_row_payloads->rows)
    {
        const term::Terminal_render_snapshot* source = row.source_snapshot.get();
        if (source != nullptr && snapshots.insert(source).second) {
            result.flat_cell_capacity_bytes += flat_capacity_bytes(*source);
            result.total_capacity_bytes += snapshot_retained_capacity_bytes(*source);
        }
    }
    return result;
}

std::optional<std::uint64_t> first_hyperlink_id(
    const term::Terminal_render_snapshot& snapshot)
{
    for (const term::Terminal_render_hyperlink_metadata& hyperlink : snapshot.hyperlinks) {
        if (hyperlink.hyperlink_id != 0U) {
            return hyperlink.hyperlink_id;
        }
    }
    return std::nullopt;
}

term::Terminal_render_snapshot snapshot_for_case(
    const case_t&                         config,
    const term::Terminal_render_snapshot& snapshot)
{
    term::Terminal_render_snapshot adjusted = snapshot;
    if (config.kind == Case_kind::STYLE_FALLBACK) {
        adjusted.color_state.default_foreground_rgba ^= 0x00010101U;
    }
    else
    if (config.kind == Case_kind::HYPERLINK_FALLBACK) {
        const std::optional<std::uint64_t> source_id = first_hyperlink_id(adjusted);
        if (source_id.has_value()) {
            const std::uint64_t target_id = *source_id + 1000U;
            for (term::Terminal_render_hyperlink_metadata& hyperlink : adjusted.hyperlinks) {
                if (hyperlink.hyperlink_id == *source_id) {
                    hyperlink.hyperlink_id = target_id;
                }
            }
            for (term::Terminal_render_cell& cell : adjusted.cells) {
                if (cell.hyperlink_id == *source_id) {
                    cell.hyperlink_id = target_id;
                }
            }
        }
    }
    return adjusted;
}

bool descriptors_match(
    const term::Terminal_render_frame& full,
    const term::Terminal_render_frame& lazy)
{
    if (full.row_descriptors.size() != lazy.row_descriptors.size() ||
        full.layer_descriptors.text_key        != lazy.layer_descriptors.text_key ||
        full.layer_descriptors.background_key  != lazy.layer_descriptors.background_key ||
        full.layer_descriptors.graphic_key     != lazy.layer_descriptors.graphic_key ||
        full.layer_descriptors.decoration_key  != lazy.layer_descriptors.decoration_key)
    {
        return false;
    }

    for (std::size_t index = 0U; index < full.row_descriptors.size(); ++index) {
        const term::Terminal_render_row_descriptor& left  = full.row_descriptors[index];
        const term::Terminal_render_row_descriptor& right = lazy.row_descriptors[index];
        if (left.row                  != right.row                  ||
            left.content_identity_key != right.content_identity_key ||
            left.text_key             != right.text_key             ||
            left.background_key       != right.background_key       ||
            left.graphic_key          != right.graphic_key          ||
            left.decoration_key       != right.decoration_key)
        {
            return false;
        }
    }
    return true;
}

bool counters_failed(const counters_t& counters)
{
    return
        counters.full_validation_failures    != 0U ||
        counters.lazy_validation_failures    != 0U ||
        counters.materialization_mismatches  != 0U ||
        counters.full_walk_frame_mismatches  != 0U ||
        counters.candidate_consumer_materialization_frames != 0U ||
        counters.unexpected_eligibility      != 0U ||
        counters.unexpected_fallback         != 0U ||
        counters.missing_snapshots           != 0U;
}

void add_lazy_metrics(
    metrics_t&                                                 metrics,
    const term::Terminal_session_lazy_snapshot_composer_result& result)
{
    metrics.borrowed_rows += result.previous_snapshot_borrowed_rows;
    metrics.producer_owned_rows += result.producer_owned_rows;
    metrics.producer_materialized_rows += result.producer_materialized_rows;
    metrics.producer_cells_scanned += result.producer_cells_scanned;
    metrics.producer_cells_emitted += result.producer_cells_emitted;
    metrics.consumer_materialization_calls += result.consumer_materialization_calls;
    metrics.consumer_materialization_rows += result.consumer_materialization_rows;
    metrics.consumer_materialization_cells += result.consumer_materialization_cells;
}

void add_full_frame_metrics(
    metrics_t&                            metrics,
    const term::Terminal_render_snapshot& snapshot,
    const term::Terminal_render_frame&    frame)
{
    metrics.full_snapshot_cells += static_cast<std::uint64_t>(snapshot.cells.size());
    metrics.full_snapshot_capacity_bytes += flat_capacity_bytes(snapshot);
    metrics.dirty_rows_visible += dirty_rows_visible(snapshot);
    metrics.full_frame_input_cells +=
        static_cast<std::uint64_t>(std::max(frame.stats.cell_pass_input_cells, 0));
    metrics.full_frame_row_descriptors +=
        static_cast<std::uint64_t>(std::max(frame.stats.row_descriptors_built, 0));
    metrics.full_frame_layer_descriptors +=
        static_cast<std::uint64_t>(std::max(frame.stats.layer_descriptors_built, 0));
}

std::shared_ptr<const term::Terminal_render_snapshot> run_update(
    session_fixture_t& fixture,
    const case_t&      config,
    int                frame,
    qint64*            out_ns)
{
    QElapsedTimer timer;
    timer.start();
    if (config.kind == Case_kind::RESIZE_BOUNDARY) {
        const term::terminal_grid_size_t grid{
            config.grid.rows + 1,
            config.grid.columns,
        };
        (void)fixture.session->resize(logical_size(grid), grid);
    }
    else
    if (config.kind == Case_kind::VIEWPORT_BOUNDARY) {
        (void)fixture.session->scroll_viewport_lines(1);
    }
    else {
        (void)fixture.backend->emit_output(frame_output(config, frame));
    }
    *out_ns = timer.nsecsElapsed();
    return fixture.session->latest_render_snapshot_handle();
}

void measure_frame(
    session_fixture_t&                                  fixture,
    const case_t&                                       config,
    std::shared_ptr<const term::Terminal_render_snapshot>& previous,
    int                                                 frame,
    repeat_t&                                           repeat)
{
    qint64 update_ns = 0;
    const std::shared_ptr<const term::Terminal_render_snapshot> current =
        run_update(fixture, config, frame, &update_ns);
    repeat.timings.full_update_ns += update_ns;
    ++repeat.counters.frames;

    if (current == nullptr) {
        ++repeat.counters.missing_snapshots;
        return;
    }

    const term::Terminal_render_snapshot candidate_full =
        snapshot_for_case(config, *current);
    const term::Terminal_render_snapshot_validation full_validation =
        term::validate_render_snapshot(candidate_full);
    if (full_validation.status != term::Terminal_render_snapshot_status::OK) {
        ++repeat.counters.full_validation_failures;
    }

    const term::Terminal_render_options render_options;
    const term::terminal_cell_metrics_t metrics = cell_metrics();

    QElapsedTimer full_frame_timer;
    full_frame_timer.start();
    const term::Terminal_render_frame full_frame =
        term::build_terminal_render_frame(
            &candidate_full,
            logical_size(candidate_full.grid_size),
            metrics,
            render_options,
            false);
    repeat.timings.full_frame_ns += full_frame_timer.nsecsElapsed();
    add_full_frame_metrics(repeat.metrics, candidate_full, full_frame);

    QElapsedTimer parity_timer;
    parity_timer.start();
    const term::Terminal_session_lazy_snapshot_composer_result parity =
        fixture.session->compose_lazy_render_snapshot_for_testing(
            previous,
            candidate_full);
    repeat.timings.parity_compose_ns += parity_timer.nsecsElapsed();
    if (config.expect_eligible) {
        if (!parity.eligible || !parity.materialization_matches_full_snapshot) {
            ++repeat.counters.materialization_mismatches;
        }
        if (!parity.lazy_snapshot.has_value() ||
            term::validate_render_snapshot(*parity.lazy_snapshot).status !=
                term::Terminal_render_snapshot_status::OK)
        {
            ++repeat.counters.lazy_validation_failures;
        }
    }

    QElapsedTimer candidate_timer;
    candidate_timer.start();
    const term::Terminal_session_lazy_snapshot_composer_result candidate =
        fixture.session->compose_lazy_render_snapshot_for_benchmark_evidence(
            previous,
            candidate_full,
            term::Terminal_lazy_snapshot_evidence_mode::
                PUBLICATION_CANDIDATE_NO_MATERIALIZATION);
    repeat.timings.candidate_compose_ns += candidate_timer.nsecsElapsed();

    if (candidate.eligible != config.expect_eligible) {
        ++repeat.counters.unexpected_eligibility;
    }
    if (!candidate.eligible) {
        record_fallback(repeat.fallbacks, candidate.fallback_reason);
        if (config.expect_eligible ||
            candidate.fallback_reason != config.expected_fallback)
        {
            ++repeat.counters.unexpected_fallback;
        }
    }
    else
    if (config.expected_fallback != term::Terminal_lazy_snapshot_fallback_reason::NONE) {
        ++repeat.counters.unexpected_fallback;
    }

    add_lazy_metrics(repeat.metrics, candidate);
    if (candidate.consumer_materialization_calls != 0U) {
        ++repeat.counters.candidate_consumer_materialization_frames;
    }
    if (candidate.eligible && candidate.lazy_snapshot.has_value()) {
        const term::Terminal_render_snapshot& lazy = *candidate.lazy_snapshot;
        const std::uint64_t eager_flat_cell_capacity = flat_capacity_bytes(candidate_full);
        const std::uint64_t lazy_flat_cell_capacity = flat_capacity_bytes(lazy);
        const std::uint64_t lazy_payload_row_capacity =
            lazy_payload_row_capacity_bytes(lazy);
        const std::uint64_t lazy_owner_retained = lazy_owner_bytes(lazy);
        const previous_snapshot_capacity_t previous_capacity =
            retained_previous_snapshot_capacity(lazy);
        const std::uint64_t lazy_total_retained_capacity =
            lazy_flat_cell_capacity +
            lazy_payload_row_capacity +
            lazy_owner_retained +
            previous_capacity.total_capacity_bytes;

        ++repeat.metrics.eligible_lazy_candidates;
        repeat.metrics.lazy_flat_cell_capacity_bytes += lazy_flat_cell_capacity;
        repeat.metrics.lazy_payload_row_capacity_bytes += lazy_payload_row_capacity;
        repeat.metrics.lazy_unique_owner_retained_bytes += lazy_owner_retained;
        repeat.metrics.retained_previous_snapshot_flat_cell_capacity_bytes +=
            previous_capacity.flat_cell_capacity_bytes;
        repeat.metrics.retained_previous_snapshot_total_capacity_bytes +=
            previous_capacity.total_capacity_bytes;
        if (lazy_flat_cell_capacity != 0U ||
            lazy_total_retained_capacity >= eager_flat_cell_capacity)
        {
            ++repeat.metrics.memory_capacity_failed_candidates;
        }

        if (term::validate_render_snapshot(lazy).status !=
            term::Terminal_render_snapshot_status::OK)
        {
            ++repeat.counters.lazy_validation_failures;
        }

        QElapsedTimer sparse_timer;
        sparse_timer.start();
        const term::Terminal_render_frame sparse_frame =
            term::build_terminal_render_frame(
                &lazy,
                logical_size(lazy.grid_size),
                metrics,
                render_options,
                false);
        repeat.timings.lazy_sparse_frame_ns += sparse_timer.nsecsElapsed();
        repeat.metrics.lazy_sparse_frame_input_cells +=
            static_cast<std::uint64_t>(
                std::max(sparse_frame.stats.cell_pass_input_cells, 0));
        repeat.metrics.lazy_sparse_frame_row_descriptors +=
            static_cast<std::uint64_t>(
                std::max(sparse_frame.stats.row_descriptors_built, 0));
        repeat.metrics.lazy_sparse_frame_layer_descriptors +=
            static_cast<std::uint64_t>(
                std::max(sparse_frame.stats.layer_descriptors_built, 0));

        QElapsedTimer full_walk_timer;
        full_walk_timer.start();
        const term::Terminal_render_frame full_walk_frame =
            term::build_terminal_render_frame(
                &lazy,
                logical_size(lazy.grid_size),
                metrics,
                render_options,
                false,
                nullptr,
                true);
        repeat.timings.lazy_full_walk_frame_ns += full_walk_timer.nsecsElapsed();
        repeat.metrics.lazy_full_walk_frame_input_cells +=
            static_cast<std::uint64_t>(
                std::max(full_walk_frame.stats.cell_pass_input_cells, 0));
        repeat.metrics.lazy_full_walk_frame_row_descriptors +=
            static_cast<std::uint64_t>(
                std::max(full_walk_frame.stats.row_descriptors_built, 0));
        if (!descriptors_match(full_frame, full_walk_frame)) {
            ++repeat.counters.full_walk_frame_mismatches;
        }
    }

    previous = current;
    fixture.session->mark_render_snapshot_synced(
        fixture.session->render_snapshot_generation());
}

bool prepare_baseline(
    session_fixture_t&                                  fixture,
    const case_t&                                       config,
    std::shared_ptr<const term::Terminal_render_snapshot>& previous)
{
    if (fixture.backend == nullptr || fixture.session == nullptr) {
        return false;
    }
    if (!fixture.backend->emit_output(baseline_output(config))) {
        return false;
    }
    previous = fixture.session->latest_render_snapshot_handle();
    if (previous == nullptr) {
        return false;
    }
    fixture.session->mark_render_snapshot_synced(
        fixture.session->render_snapshot_generation());
    return true;
}

bool boundary_case(Case_kind kind)
{
    return kind == Case_kind::RESIZE_BOUNDARY || kind == Case_kind::VIEWPORT_BOUNDARY;
}

repeat_t run_repeat(const options_t& options, const case_t& config, int repeat_index)
{
    repeat_t repeat;
    repeat.index = repeat_index;
    session_fixture_t fixture = make_session(config.grid);

    std::shared_ptr<const term::Terminal_render_snapshot> previous;
    if (!prepare_baseline(fixture, config, previous)) {
        repeat.status = QStringLiteral("correctness_failed");
        ++repeat.counters.missing_snapshots;
        return repeat;
    }

    if (!boundary_case(config.kind)) {
        for (int warmup = 0; warmup < options.warmup_frames; ++warmup) {
            qint64 ignored_ns = 0;
            const std::shared_ptr<const term::Terminal_render_snapshot> current =
                run_update(fixture, config, -options.warmup_frames + warmup, &ignored_ns);
            if (current != nullptr) {
                previous = current;
                fixture.session->mark_render_snapshot_synced(
                    fixture.session->render_snapshot_generation());
            }
        }
    }

    const int frames = boundary_case(config.kind) ? 1 : options.frames;
    for (int frame = 0; frame < frames; ++frame) {
        measure_frame(fixture, config, previous, frame, repeat);
    }

    if (counters_failed(repeat.counters)) {
        repeat.status = QStringLiteral("correctness_failed");
    }
    return repeat;
}

std::vector<case_t> all_cases()
{
    return {
        {QStringLiteral("mostly_clean_scrollback_sparse_ascii_48x160"),
            Case_kind::MOSTLY_CLEAN_SCROLLBACK, Text_pattern::ASCII, {48, 160},
            96, 2, 17, 0, true,
            term::Terminal_lazy_snapshot_fallback_reason::NONE},
        {QStringLiteral("sparse_dirty_rows_block_48x160"),
            Case_kind::SPARSE_DIRTY_ROWS, Text_pattern::BLOCK, {48, 160},
            0, 5, 11, 8, true,
            term::Terminal_lazy_snapshot_fallback_reason::NONE},
        {QStringLiteral("full_repaint_ascii_48x160"),
            Case_kind::FULL_REPAINT, Text_pattern::ASCII, {48, 160},
            0, 48, 1, 8, false,
            term::Terminal_lazy_snapshot_fallback_reason::NO_BORROWABLE_ROWS},
        {QStringLiteral("style_mismatch_fallback_24x80"),
            Case_kind::STYLE_FALLBACK, Text_pattern::ASCII, {24, 80},
            0, 3, 7, 4, false,
            term::Terminal_lazy_snapshot_fallback_reason::
                STYLE_COLOR_MODE_INCOMPATIBILITY},
        {QStringLiteral("hyperlink_namespace_fallback_24x80"),
            Case_kind::HYPERLINK_FALLBACK, Text_pattern::HYPERLINK, {24, 80},
            0, 3, 5, 0, false,
            term::Terminal_lazy_snapshot_fallback_reason::
                HYPERLINK_NAMESPACE_INCOMPATIBILITY},
        {QStringLiteral("wide_combining_sparse_48x160"),
            Case_kind::WIDE_COMBINING, Text_pattern::WIDE_COMBINING, {48, 160},
            0, 5, 13, 6, true,
            term::Terminal_lazy_snapshot_fallback_reason::NONE},
        {QStringLiteral("resize_grid_mismatch_boundary_24x80"),
            Case_kind::RESIZE_BOUNDARY, Text_pattern::ASCII, {24, 80},
            0, 3, 7, 0, false,
            term::Terminal_lazy_snapshot_fallback_reason::GRID_MISMATCH},
        {QStringLiteral("viewport_mismatch_boundary_24x80"),
            Case_kind::VIEWPORT_BOUNDARY, Text_pattern::ASCII, {24, 80},
            64, 3, 7, 0, false,
            term::Terminal_lazy_snapshot_fallback_reason::VIEWPORT_MISMATCH},
    };
}

std::vector<case_t> selected_cases(const options_t& options)
{
    const std::vector<case_t> cases = all_cases();
    if (options.cases.empty()) {
        return cases;
    }

    std::vector<case_t> selected;
    for (const QString& name : options.cases) {
        const auto found =
            std::find_if(
                cases.begin(),
                cases.end(),
                [&name](const case_t& config) { return config.name == name; });
        if (found != cases.end()) {
            selected.push_back(*found);
        }
    }
    return selected;
}

case_result_t run_case(const options_t& options, const case_t& config)
{
    case_result_t result;
    result.config = config;
    for (int repeat = 0; repeat < options.repeats; ++repeat) {
        result.repeats.push_back(run_repeat(options, config, repeat));
        if (result.repeats.back().status != QStringLiteral("ok")) {
            result.status = QStringLiteral("correctness_failed");
        }
    }
    return result;
}

void add(counters_t& total, const counters_t& value)
{
    total.frames += value.frames;
    total.full_validation_failures += value.full_validation_failures;
    total.lazy_validation_failures += value.lazy_validation_failures;
    total.materialization_mismatches += value.materialization_mismatches;
    total.full_walk_frame_mismatches += value.full_walk_frame_mismatches;
    total.candidate_consumer_materialization_frames +=
        value.candidate_consumer_materialization_frames;
    total.unexpected_eligibility += value.unexpected_eligibility;
    total.unexpected_fallback += value.unexpected_fallback;
    total.missing_snapshots += value.missing_snapshots;
}

void add(fallback_counts_t& total, const fallback_counts_t& value)
{
    total.missing_previous_content_snapshot += value.missing_previous_content_snapshot;
    total.grid_mismatch += value.grid_mismatch;
    total.viewport_mismatch += value.viewport_mismatch;
    total.active_buffer_mismatch += value.active_buffer_mismatch;
    total.public_projection += value.public_projection;
    total.row_origin_generation_mismatch += value.row_origin_generation_mismatch;
    total.style_color_mode_incompatibility += value.style_color_mode_incompatibility;
    total.hyperlink_namespace_incompatibility += value.hyperlink_namespace_incompatibility;
    total.unstable_dirty_row_mutation_identity += value.unstable_dirty_row_mutation_identity;
    total.no_borrowable_rows += value.no_borrowable_rows;
    total.unsupported_geometry_or_detached_snapshot_path +=
        value.unsupported_geometry_or_detached_snapshot_path;
}

void add(timings_t& total, const timings_t& value)
{
    total.full_update_ns += value.full_update_ns;
    total.full_frame_ns += value.full_frame_ns;
    total.parity_compose_ns += value.parity_compose_ns;
    total.candidate_compose_ns += value.candidate_compose_ns;
    total.lazy_sparse_frame_ns += value.lazy_sparse_frame_ns;
    total.lazy_full_walk_frame_ns += value.lazy_full_walk_frame_ns;
}

void add(metrics_t& total, const metrics_t& value)
{
    total.full_snapshot_cells += value.full_snapshot_cells;
    total.full_snapshot_capacity_bytes += value.full_snapshot_capacity_bytes;
    total.dirty_rows_visible += value.dirty_rows_visible;
    total.full_frame_input_cells += value.full_frame_input_cells;
    total.full_frame_row_descriptors += value.full_frame_row_descriptors;
    total.full_frame_layer_descriptors += value.full_frame_layer_descriptors;
    total.borrowed_rows += value.borrowed_rows;
    total.producer_owned_rows += value.producer_owned_rows;
    total.producer_materialized_rows += value.producer_materialized_rows;
    total.producer_cells_scanned += value.producer_cells_scanned;
    total.producer_cells_emitted += value.producer_cells_emitted;
    total.consumer_materialization_calls += value.consumer_materialization_calls;
    total.consumer_materialization_rows += value.consumer_materialization_rows;
    total.consumer_materialization_cells += value.consumer_materialization_cells;
    total.lazy_sparse_frame_input_cells += value.lazy_sparse_frame_input_cells;
    total.lazy_sparse_frame_row_descriptors += value.lazy_sparse_frame_row_descriptors;
    total.lazy_sparse_frame_layer_descriptors += value.lazy_sparse_frame_layer_descriptors;
    total.lazy_full_walk_frame_input_cells += value.lazy_full_walk_frame_input_cells;
    total.lazy_full_walk_frame_row_descriptors += value.lazy_full_walk_frame_row_descriptors;
    total.eligible_lazy_candidates += value.eligible_lazy_candidates;
    total.memory_capacity_failed_candidates += value.memory_capacity_failed_candidates;
    total.lazy_flat_cell_capacity_bytes += value.lazy_flat_cell_capacity_bytes;
    total.lazy_payload_row_capacity_bytes += value.lazy_payload_row_capacity_bytes;
    total.lazy_unique_owner_retained_bytes += value.lazy_unique_owner_retained_bytes;
    total.retained_previous_snapshot_flat_cell_capacity_bytes +=
        value.retained_previous_snapshot_flat_cell_capacity_bytes;
    total.retained_previous_snapshot_total_capacity_bytes +=
        value.retained_previous_snapshot_total_capacity_bytes;
}

double ratio(std::uint64_t numerator, std::uint64_t denominator)
{
    return denominator == 0U
        ? 0.0
        : static_cast<double>(numerator) / static_cast<double>(denominator);
}

double reduction(std::uint64_t value, std::uint64_t baseline)
{
    return baseline == 0U || value >= baseline
        ? 0.0
        : static_cast<double>(baseline - value) / static_cast<double>(baseline);
}

QJsonValue json_int(std::uint64_t value)
{
    return QJsonValue(static_cast<qint64>(value));
}

QJsonObject counters_json(const counters_t& counters, const fallback_counts_t& fallbacks)
{
    QJsonObject fallback;
    fallback.insert(
        QStringLiteral("missing_previous_content_snapshot"),
        json_int(fallbacks.missing_previous_content_snapshot));
    fallback.insert(QStringLiteral("grid_mismatch"), json_int(fallbacks.grid_mismatch));
    fallback.insert(
        QStringLiteral("viewport_mismatch"),
        json_int(fallbacks.viewport_mismatch));
    fallback.insert(
        QStringLiteral("active_buffer_mismatch"),
        json_int(fallbacks.active_buffer_mismatch));
    fallback.insert(
        QStringLiteral("public_projection"),
        json_int(fallbacks.public_projection));
    fallback.insert(
        QStringLiteral("row_origin_generation_mismatch"),
        json_int(fallbacks.row_origin_generation_mismatch));
    fallback.insert(
        QStringLiteral("style_color_mode_incompatibility"),
        json_int(fallbacks.style_color_mode_incompatibility));
    fallback.insert(
        QStringLiteral("hyperlink_namespace_incompatibility"),
        json_int(fallbacks.hyperlink_namespace_incompatibility));
    fallback.insert(
        QStringLiteral("unstable_dirty_row_mutation_identity"),
        json_int(fallbacks.unstable_dirty_row_mutation_identity));
    fallback.insert(
        QStringLiteral("no_borrowable_rows"),
        json_int(fallbacks.no_borrowable_rows));
    fallback.insert(
        QStringLiteral("unsupported_geometry_or_detached_snapshot_path"),
        json_int(fallbacks.unsupported_geometry_or_detached_snapshot_path));

    QJsonObject object;
    object.insert(QStringLiteral("measured_frames"), json_int(counters.frames));
    object.insert(
        QStringLiteral("full_snapshot_validation_failures"),
        json_int(counters.full_validation_failures));
    object.insert(
        QStringLiteral("lazy_snapshot_validation_failures"),
        json_int(counters.lazy_validation_failures));
    object.insert(
        QStringLiteral("materialization_mismatch_frames"),
        json_int(counters.materialization_mismatches));
    object.insert(
        QStringLiteral("frame_full_walk_mismatch_frames"),
        json_int(counters.full_walk_frame_mismatches));
    object.insert(
        QStringLiteral("candidate_consumer_materialization_frames"),
        json_int(counters.candidate_consumer_materialization_frames));
    object.insert(
        QStringLiteral("unexpected_eligibility_frames"),
        json_int(counters.unexpected_eligibility));
    object.insert(
        QStringLiteral("unexpected_fallback_frames"),
        json_int(counters.unexpected_fallback));
    object.insert(
        QStringLiteral("missing_snapshot_frames"),
        json_int(counters.missing_snapshots));
    object.insert(QStringLiteral("fallback_reasons"), fallback);
    return object;
}

QJsonObject timings_json(const timings_t& timings, std::uint64_t frames)
{
    const qint64 divisor = static_cast<qint64>(std::max<std::uint64_t>(frames, 1U));
    QJsonObject total;
    total.insert(QStringLiteral("full_update_ns"), timings.full_update_ns);
    total.insert(QStringLiteral("full_frame_ns"), timings.full_frame_ns);
    total.insert(QStringLiteral("parity_compose_ns"), timings.parity_compose_ns);
    total.insert(QStringLiteral("candidate_compose_ns"), timings.candidate_compose_ns);
    total.insert(QStringLiteral("lazy_sparse_frame_ns"), timings.lazy_sparse_frame_ns);
    total.insert(
        QStringLiteral("lazy_full_walk_frame_ns"),
        timings.lazy_full_walk_frame_ns);

    QJsonObject per_frame;
    per_frame.insert(QStringLiteral("full_update_ns"), timings.full_update_ns / divisor);
    per_frame.insert(QStringLiteral("full_frame_ns"), timings.full_frame_ns / divisor);
    per_frame.insert(
        QStringLiteral("parity_compose_ns"),
        timings.parity_compose_ns / divisor);
    per_frame.insert(
        QStringLiteral("candidate_compose_ns"),
        timings.candidate_compose_ns / divisor);
    per_frame.insert(
        QStringLiteral("lazy_sparse_frame_ns"),
        timings.lazy_sparse_frame_ns / divisor);
    per_frame.insert(
        QStringLiteral("lazy_full_walk_frame_ns"),
        timings.lazy_full_walk_frame_ns / divisor);

    QJsonObject object;
    object.insert(QStringLiteral("total"), total);
    object.insert(QStringLiteral("per_frame"), per_frame);
    return object;
}

QJsonObject metrics_json(const metrics_t& metrics)
{
    QJsonObject full_snapshot;
    full_snapshot.insert(QStringLiteral("cells"), json_int(metrics.full_snapshot_cells));
    full_snapshot.insert(
        QStringLiteral("cell_capacity_bytes"),
        json_int(metrics.full_snapshot_capacity_bytes));
    full_snapshot.insert(
        QStringLiteral("dirty_rows_visible"),
        json_int(metrics.dirty_rows_visible));

    QJsonObject full_frame;
    full_frame.insert(
        QStringLiteral("input_cells"),
        json_int(metrics.full_frame_input_cells));
    full_frame.insert(
        QStringLiteral("row_descriptors_built"),
        json_int(metrics.full_frame_row_descriptors));
    full_frame.insert(
        QStringLiteral("layer_descriptors_built"),
        json_int(metrics.full_frame_layer_descriptors));

    QJsonObject lazy;
    lazy.insert(QStringLiteral("borrowed_rows"), json_int(metrics.borrowed_rows));
    lazy.insert(
        QStringLiteral("borrowed_row_ratio"),
        ratio(metrics.borrowed_rows, metrics.borrowed_rows + metrics.producer_owned_rows));
    lazy.insert(
        QStringLiteral("producer_owned_rows"),
        json_int(metrics.producer_owned_rows));
    lazy.insert(
        QStringLiteral("producer_materialized_rows"),
        json_int(metrics.producer_materialized_rows));
    lazy.insert(
        QStringLiteral("producer_cells_scanned"),
        json_int(metrics.producer_cells_scanned));
    lazy.insert(
        QStringLiteral("producer_cells_emitted"),
        json_int(metrics.producer_cells_emitted));
    lazy.insert(
        QStringLiteral("consumer_materialization_calls"),
        json_int(metrics.consumer_materialization_calls));
    lazy.insert(
        QStringLiteral("consumer_materialization_rows"),
        json_int(metrics.consumer_materialization_rows));
    lazy.insert(
        QStringLiteral("consumer_materialization_cells"),
        json_int(metrics.consumer_materialization_cells));

    QJsonObject sparse_frame;
    sparse_frame.insert(
        QStringLiteral("input_cells"),
        json_int(metrics.lazy_sparse_frame_input_cells));
    sparse_frame.insert(
        QStringLiteral("row_descriptors_built"),
        json_int(metrics.lazy_sparse_frame_row_descriptors));
    sparse_frame.insert(
        QStringLiteral("layer_descriptors_built"),
        json_int(metrics.lazy_sparse_frame_layer_descriptors));
    sparse_frame.insert(
        QStringLiteral("input_cell_reduction_ratio"),
        reduction(metrics.lazy_sparse_frame_input_cells, metrics.full_frame_input_cells));
    sparse_frame.insert(
        QStringLiteral("row_descriptor_reduction_ratio"),
        reduction(metrics.lazy_sparse_frame_row_descriptors, metrics.full_frame_row_descriptors));

    QJsonObject full_walk;
    full_walk.insert(
        QStringLiteral("input_cells"),
        json_int(metrics.lazy_full_walk_frame_input_cells));
    full_walk.insert(
        QStringLiteral("row_descriptors_built"),
        json_int(metrics.lazy_full_walk_frame_row_descriptors));

    QJsonObject memory;
    const std::uint64_t lazy_total_retained_capacity =
        metrics.lazy_flat_cell_capacity_bytes +
        metrics.lazy_payload_row_capacity_bytes +
        metrics.lazy_unique_owner_retained_bytes +
        metrics.retained_previous_snapshot_total_capacity_bytes;
    const bool capacity_less_than_eager_flat =
        metrics.eligible_lazy_candidates > 0U &&
        metrics.memory_capacity_failed_candidates == 0U;

    memory.insert(
        QStringLiteral("eligible_lazy_candidates"),
        json_int(metrics.eligible_lazy_candidates));
    memory.insert(
        QStringLiteral("memory_capacity_failed_candidates"),
        json_int(metrics.memory_capacity_failed_candidates));
    memory.insert(
        QStringLiteral("eager_flat_cell_capacity_bytes"),
        json_int(metrics.full_snapshot_capacity_bytes));
    memory.insert(
        QStringLiteral("lazy_flat_cell_capacity_bytes"),
        json_int(metrics.lazy_flat_cell_capacity_bytes));
    memory.insert(
        QStringLiteral("lazy_payload_row_capacity_bytes"),
        json_int(metrics.lazy_payload_row_capacity_bytes));
    memory.insert(
        QStringLiteral("lazy_unique_owner_retained_bytes"),
        json_int(metrics.lazy_unique_owner_retained_bytes));
    memory.insert(
        QStringLiteral("retained_previous_snapshot_flat_cell_capacity_bytes"),
        json_int(metrics.retained_previous_snapshot_flat_cell_capacity_bytes));
    memory.insert(
        QStringLiteral("retained_previous_snapshot_total_capacity_bytes"),
        json_int(metrics.retained_previous_snapshot_total_capacity_bytes));
    memory.insert(
        QStringLiteral("lazy_total_capacity_bytes"),
        json_int(lazy_total_retained_capacity));
    memory.insert(
        QStringLiteral("lazy_total_retained_capacity_bytes"),
        json_int(lazy_total_retained_capacity));
    memory.insert(
        QStringLiteral("capacity_less_than_eager_flat"),
        capacity_less_than_eager_flat);
    memory.insert(
        QStringLiteral("lazy_to_full_capacity_ratio"),
        ratio(
            lazy_total_retained_capacity,
            metrics.full_snapshot_capacity_bytes));

    QJsonObject object;
    object.insert(QStringLiteral("full_snapshot"), full_snapshot);
    object.insert(QStringLiteral("full_frame"), full_frame);
    object.insert(QStringLiteral("lazy_candidate"), lazy);
    object.insert(QStringLiteral("lazy_sparse_frame"), sparse_frame);
    object.insert(QStringLiteral("lazy_full_walk_frame"), full_walk);
    object.insert(QStringLiteral("memory"), memory);
    return object;
}

QJsonObject repeat_json(const repeat_t& repeat)
{
    QJsonObject object;
    object.insert(QStringLiteral("repeat_index"), repeat.index);
    object.insert(QStringLiteral("status"), repeat.status);
    object.insert(
        QStringLiteral("correctness"),
        counters_json(repeat.counters, repeat.fallbacks));
    object.insert(
        QStringLiteral("timing_ns"),
        timings_json(repeat.timings, repeat.counters.frames));
    object.insert(QStringLiteral("metrics"), metrics_json(repeat.metrics));
    return object;
}

QJsonObject case_config_json(const case_t& config)
{
    QJsonObject object;
    object.insert(QStringLiteral("name"), config.name);
    object.insert(QStringLiteral("kind"), kind_name(config.kind));
    object.insert(QStringLiteral("text_pattern"), pattern_name(config.pattern));
    object.insert(QStringLiteral("rows"), config.grid.rows);
    object.insert(QStringLiteral("columns"), config.grid.columns);
    object.insert(QStringLiteral("scrollback_lines"), config.scrollback_lines);
    object.insert(QStringLiteral("dirty_rows_per_frame"), config.dirty_rows);
    object.insert(QStringLiteral("dirty_row_stride"), config.stride);
    object.insert(QStringLiteral("style_period"), config.style_period);
    return object;
}

QJsonObject expected_json(const case_t& config)
{
    QJsonObject object;
    object.insert(QStringLiteral("eligible"), config.expect_eligible);
    object.insert(
        QStringLiteral("fallback_reason"),
        fallback_name(config.expected_fallback));
    object.insert(
        QStringLiteral("performance_applicable"),
        config.expect_eligible);
    return object;
}

QJsonObject case_summary_json(const case_result_t& result)
{
    counters_t counters;
    fallback_counts_t fallbacks;
    timings_t timings;
    metrics_t metrics;
    for (const repeat_t& repeat : result.repeats) {
        add(counters, repeat.counters);
        add(fallbacks, repeat.fallbacks);
        add(timings, repeat.timings);
        add(metrics, repeat.metrics);
    }

    QJsonObject object;
    object.insert(QStringLiteral("correctness"), counters_json(counters, fallbacks));
    object.insert(QStringLiteral("timing_ns"), timings_json(timings, counters.frames));
    object.insert(QStringLiteral("metrics"), metrics_json(metrics));
    return object;
}

QJsonObject case_json(const case_result_t& result)
{
    QJsonArray repeats;
    for (const repeat_t& repeat : result.repeats) {
        repeats.push_back(repeat_json(repeat));
    }

    QJsonObject object;
    object.insert(QStringLiteral("name"), result.config.name);
    object.insert(QStringLiteral("status"), result.status);
    object.insert(QStringLiteral("config"), case_config_json(result.config));
    object.insert(QStringLiteral("expected"), expected_json(result.config));
    object.insert(QStringLiteral("repeats"), repeats);
    object.insert(QStringLiteral("summary"), case_summary_json(result));
    return object;
}

bool sparse_reduction_ready(const case_result_t& result)
{
    if (!result.config.expect_eligible ||
        result.config.kind == Case_kind::FULL_REPAINT ||
        result.status != QStringLiteral("ok"))
    {
        return true;
    }

    metrics_t metrics;
    for (const repeat_t& repeat : result.repeats) {
        add(metrics, repeat.metrics);
    }

    return
        metrics.borrowed_rows > 0U &&
        metrics.lazy_sparse_frame_input_cells < metrics.full_frame_input_cells &&
        metrics.lazy_sparse_frame_row_descriptors < metrics.full_frame_row_descriptors;
}

bool memory_ready(const std::vector<case_result_t>& results)
{
    for (const case_result_t& result : results) {
        if (!result.config.expect_eligible) {
            continue;
        }

        metrics_t metrics;
        for (const repeat_t& repeat : result.repeats) {
            add(metrics, repeat.metrics);
        }
        if (metrics.eligible_lazy_candidates == 0U ||
            metrics.memory_capacity_failed_candidates != 0U ||
            metrics.lazy_flat_cell_capacity_bytes != 0U)
        {
            return false;
        }
    }
    return true;
}

QJsonObject decision_json(const QString& status, const std::vector<case_result_t>& results)
{
    bool sparse_ready = true;
    for (const case_result_t& result : results) {
        sparse_ready = sparse_ready && sparse_reduction_ready(result);
    }
    const bool correctness_ready = status == QStringLiteral("ok");
    const bool capacity_ready = memory_ready(results);
    const bool evidence_only_ready = correctness_ready && sparse_ready;

    QJsonObject observed;
    observed.insert(QStringLiteral("correctness_ready"), correctness_ready);
    observed.insert(QStringLiteral("sparse_reduction_ready"), sparse_ready);
    observed.insert(QStringLiteral("memory_capacity_ready"), capacity_ready);
    observed.insert(QStringLiteral("evidence_only_ready"), evidence_only_ready);
    observed.insert(QStringLiteral("production_enablement_ready"), false);
    observed.insert(
        QStringLiteral("recommendation"),
        QStringLiteral("retain_evidence_only_no_production_switch"));

    QJsonObject criteria;
    criteria.insert(
        QStringLiteral("correctness_parity"),
        QStringLiteral("eligible cases require zero validation, materialization, and full-walk descriptor mismatches"));
    criteria.insert(
        QStringLiteral("fallback_counts"),
        QStringLiteral("fallback cases require exact expected reasons; eligible cases require none"));
    criteria.insert(
        QStringLiteral("materialization_counts"),
        QStringLiteral("evidence candidates require zero consumer materialization; parity materialization is separate"));
    criteria.insert(
        QStringLiteral("borrowed_row_ratio"),
        QStringLiteral("sparse cases must borrow clean rows; full repaint is a no-benefit control"));
    criteria.insert(
        QStringLiteral("frame_input_cell_reduction"),
        QStringLiteral("sparse lazy frames must reduce input cells and row descriptor churn"));
    criteria.insert(
        QStringLiteral("time_distribution"),
        QStringLiteral("use timings only after correctness status is ok; do not classify validation failures as performance results"));
    criteria.insert(
        QStringLiteral("memory_capacity"),
        QStringLiteral("eligible evidence candidates must retain zero lazy flat-cell capacity; retained previous snapshots currently prevent production enablement"));

    QJsonObject final_state;
    final_state.insert(QStringLiteral("state"), QStringLiteral("evidence-only"));
    final_state.insert(
        QStringLiteral("production_enablement_status"),
        QStringLiteral("rejected"));
    final_state.insert(
        QStringLiteral("production_publication_path"),
        QStringLiteral("full_snapshot_only"));
    final_state.insert(
        QStringLiteral("lazy_composer_reachability"),
        QStringLiteral("internal_testing_and_benchmark_evidence_api"));
    final_state.insert(QStringLiteral("default_production_enabled"), false);
    final_state.insert(QStringLiteral("no_production_switch"), true);

    QJsonObject input_echo;
    input_echo.insert(
        QStringLiteral("non_delay_fix"),
        QStringLiteral("prove event ordering: drain already queued backend callbacks before first post-input frame sync instead of sleeping"));
    input_echo.insert(
        QStringLiteral("proof_metrics"),
        QStringLiteral("first post-input frame must contain echoed input, stale pre-echo frame count zero, catch-up frame count zero, delay budget zero"));

    QJsonObject object;
    object.insert(QStringLiteral("observed"), observed);
    object.insert(QStringLiteral("evidence_contract"), criteria);
    object.insert(QStringLiteral("final_lazy_state"), final_state);
    object.insert(
        QStringLiteral("evidence_only_settlement"),
        QStringLiteral("lazy composition remains a testing and benchmark evidence fixture; production publication remains full snapshots"));
    object.insert(QStringLiteral("input_echo_event_order_fix"), input_echo);
    return object;
}

QJsonObject root_json(const options_t& options, const std::vector<case_result_t>& results)
{
    QString status = QStringLiteral("ok");
    QJsonArray cases;
    for (const case_result_t& result : results) {
        cases.push_back(case_json(result));
        if (result.status != QStringLiteral("ok")) {
            status = QStringLiteral("correctness_failed");
        }
    }

    QJsonObject options_json;
    options_json.insert(QStringLiteral("frames"), options.frames);
    options_json.insert(QStringLiteral("warmup_frames"), options.warmup_frames);
    options_json.insert(QStringLiteral("repeats"), options.repeats);

    QJsonObject object;
    object.insert(QStringLiteral("schema"), QString::fromLatin1(k_schema));
    object.insert(QStringLiteral("schema_version"), k_schema_version);
    object.insert(QStringLiteral("status"), status);
    object.insert(
        QStringLiteral("measurement_boundary"),
        QStringLiteral("real Terminal_session full update/publication versus benchmark-only lazy composition; production publication remains full"));
    object.insert(
        QStringLiteral("correctness_boundary"),
        QStringLiteral("snapshot validation, fallback reason checks, parity materialization, and full-walk descriptor parity are hard failures separate from timings"));
    object.insert(
        QStringLiteral("previous_matrix_instability_likely_source"),
        QStringLiteral("the earlier matrix applied sparse K=0 all-pass validation to fallback/boundary/default cases and omitted flat-cell capacity evidence"));
    object.insert(QStringLiteral("options"), options_json);
    object.insert(QStringLiteral("cases"), cases);
    object.insert(QStringLiteral("decision_criteria"), decision_json(status, results));
    return object;
}

bool parse_positive(const QString& text, int* out)
{
    bool ok = false;
    const int value = text.toInt(&ok);
    if (!ok || value <= 0) {
        return false;
    }
    *out = value;
    return true;
}

bool take_arg(const QStringList& args, int& index, QString* out, QString* error)
{
    if (index + 1 >= args.size()) {
        *error = QStringLiteral("%1 requires a value").arg(args[index]);
        return false;
    }
    *out = args[index + 1];
    index += 2;
    return true;
}

bool parse_options(const QStringList& args, options_t* out, QString* error)
{
    options_t options;
    int index = 1;
    while (index < args.size()) {
        const QString arg = args[index];
        if (arg == QStringLiteral("--help")) {
            options.help = true;
            ++index;
            continue;
        }
        if (arg == QStringLiteral("--quiet")) {
            options.quiet = true;
            ++index;
            continue;
        }
        if (arg == QStringLiteral("--validate-json")) {
            options.validate_json = true;
            ++index;
            continue;
        }

        QString value;
        if (arg == QStringLiteral("--frames") ||
            arg == QStringLiteral("--warmup-frames") ||
            arg == QStringLiteral("--repeats"))
        {
            if (!take_arg(args, index, &value, error)) {
                return false;
            }
            int parsed = 0;
            if (!parse_positive(value, &parsed)) {
                *error = QStringLiteral("%1 requires a positive integer").arg(arg);
                return false;
            }
            if (arg == QStringLiteral("--frames")) {
                options.frames = parsed;
            }
            else
            if (arg == QStringLiteral("--warmup-frames")) {
                options.warmup_frames = parsed;
            }
            else {
                options.repeats = parsed;
            }
            continue;
        }

        if (arg == QStringLiteral("--case")) {
            if (!take_arg(args, index, &value, error)) {
                return false;
            }
            options.cases.push_back(value);
            continue;
        }

        if (arg == QStringLiteral("--output")) {
            if (!take_arg(args, index, &value, error)) {
                return false;
            }
            options.output_path = value;
            continue;
        }

        *error = QStringLiteral("unknown argument: %1").arg(arg);
        return false;
    }

    *out = options;
    return true;
}

void print_help()
{
    std::cout
        << "Usage: vnm_terminal_lazy_snapshot_evidence_benchmark [options]\n"
        << "  --frames <n>\n"
        << "  --warmup-frames <n>\n"
        << "  --repeats <n>\n"
        << "  --case <name>\n"
        << "  --output <path>\n"
        << "  --quiet\n"
        << "  --validate-json\n";
}

bool validate_case_names(const options_t& options, QString* error)
{
    const std::vector<case_t> cases = all_cases();
    for (const QString& name : options.cases) {
        const auto found =
            std::find_if(
                cases.begin(),
                cases.end(),
                [&name](const case_t& config) { return config.name == name; });
        if (found == cases.end()) {
            *error = QStringLiteral("unknown case: %1").arg(name);
            return false;
        }
    }
    return true;
}

bool write_file(const QString& path, const QByteArray& bytes, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("failed to open output: %1").arg(path);
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        *error = QStringLiteral("failed to write output: %1").arg(path);
        return false;
    }
    return true;
}

bool read_bool_field(
    const QJsonObject& object,
    const QString&     key,
    bool*              value,
    QString*           error)
{
    const QJsonValue field = object.value(key);
    if (!field.isBool()) {
        *error =
            QStringLiteral("missing or non-boolean decision field: %1").arg(key);
        return false;
    }

    *value = field.toBool();
    return true;
}

bool validate_decision_contract(const QJsonObject& root, QString* error)
{
    if (root.value(QStringLiteral("schema")).toString() !=
            QString::fromLatin1(k_schema) ||
        root.value(QStringLiteral("schema_version")).toInt() != k_schema_version)
    {
        *error = QStringLiteral("schema identity mismatch");
        return false;
    }

    const QString status = root.value(QStringLiteral("status")).toString();
    if (status != QStringLiteral("ok")) {
        *error = QStringLiteral("root status is not ok");
        return false;
    }

    const QJsonValue decision_value = root.value(QStringLiteral("decision_criteria"));
    if (!decision_value.isObject()) {
        *error = QStringLiteral("missing decision_criteria object");
        return false;
    }

    const QJsonValue observed_value =
        decision_value.toObject().value(QStringLiteral("observed"));
    if (!observed_value.isObject()) {
        *error = QStringLiteral("missing decision_criteria.observed object");
        return false;
    }

    const QJsonObject decision = decision_value.toObject();
    if (decision.contains(QStringLiteral("enable_lazy_publication"))) {
        *error = QStringLiteral("stale enable_lazy_publication decision field present");
        return false;
    }

    const QJsonObject observed = observed_value.toObject();
    if (observed.contains(QStringLiteral("enablement_ready"))) {
        *error = QStringLiteral("stale enablement_ready decision field present");
        return false;
    }

    bool correctness_ready = false;
    bool sparse_reduction_ready = false;
    bool memory_capacity_ready = false;
    bool evidence_only_ready = false;
    bool production_enablement_ready = false;
    if (!read_bool_field(
            observed,
            QStringLiteral("correctness_ready"),
            &correctness_ready,
            error) ||
        !read_bool_field(
            observed,
            QStringLiteral("sparse_reduction_ready"),
            &sparse_reduction_ready,
            error) ||
        !read_bool_field(
            observed,
            QStringLiteral("memory_capacity_ready"),
            &memory_capacity_ready,
            error) ||
        !read_bool_field(
            observed,
            QStringLiteral("evidence_only_ready"),
            &evidence_only_ready,
            error) ||
        !read_bool_field(
            observed,
            QStringLiteral("production_enablement_ready"),
            &production_enablement_ready,
            error))
    {
        return false;
    }

    if (!correctness_ready) {
        *error = QStringLiteral("status ok requires correctness_ready");
        return false;
    }

    const bool expected_evidence_only_ready =
        correctness_ready && sparse_reduction_ready;
    if (evidence_only_ready != expected_evidence_only_ready) {
        *error =
            QStringLiteral("evidence_only_ready is inconsistent with readiness fields");
        return false;
    }

    if (production_enablement_ready) {
        *error = QStringLiteral("production enablement must remain rejected");
        return false;
    }

    (void)memory_capacity_ready;

    if (observed.value(QStringLiteral("recommendation")).toString() !=
        QStringLiteral("retain_evidence_only_no_production_switch"))
    {
        *error = QStringLiteral("decision recommendation is inconsistent");
        return false;
    }

    const QJsonValue final_state_value =
        decision.value(QStringLiteral("final_lazy_state"));
    if (!final_state_value.isObject()) {
        *error = QStringLiteral("missing final_lazy_state object");
        return false;
    }

    const QJsonObject final_state = final_state_value.toObject();
    if (final_state.value(QStringLiteral("state")).toString() !=
            QStringLiteral("evidence-only") ||
        final_state.value(QStringLiteral("production_enablement_status")).toString() !=
            QStringLiteral("rejected") ||
        final_state.value(QStringLiteral("production_publication_path")).toString() !=
            QStringLiteral("full_snapshot_only") ||
        final_state.value(QStringLiteral("lazy_composer_reachability")).toString() !=
            QStringLiteral("internal_testing_and_benchmark_evidence_api"))
    {
        *error = QStringLiteral("final lazy state identity is inconsistent");
        return false;
    }

    bool default_production_enabled = true;
    bool no_production_switch = false;
    if (!read_bool_field(
            final_state,
            QStringLiteral("default_production_enabled"),
            &default_production_enabled,
            error) ||
        !read_bool_field(
            final_state,
            QStringLiteral("no_production_switch"),
            &no_production_switch,
            error))
    {
        return false;
    }
    if (default_production_enabled || !no_production_switch) {
        *error = QStringLiteral("final lazy state production switch is inconsistent");
        return false;
    }

    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QStringList args;
    for (int index = 0; index < argc; ++index) {
        args.push_back(QString::fromLocal8Bit(argv[index]));
    }

    options_t options;
    QString error;
    if (!parse_options(args, &options, &error)) {
        std::cerr << error.toUtf8().constData() << '\n';
        return 2;
    }
    if (options.help) {
        print_help();
        return 0;
    }
    if (!validate_case_names(options, &error)) {
        std::cerr << error.toUtf8().constData() << '\n';
        return 2;
    }

    std::vector<case_result_t> results;
    for (const case_t& config : selected_cases(options)) {
        results.push_back(run_case(options, config));
    }

    const QJsonObject root = root_json(options, results);
    const QJsonDocument doc(root);
    const QByteArray output = doc.toJson(QJsonDocument::Indented);

    if (!options.output_path.isEmpty() &&
        !write_file(options.output_path, output, &error))
    {
        std::cerr << error.toUtf8().constData() << '\n';
        return 2;
    }
    if (!options.quiet || options.output_path.isEmpty()) {
        std::cout << output.constData();
    }

    if (options.validate_json) {
        QString validation_error;
        if (!validate_decision_contract(root, &validation_error)) {
            std::cerr
                << "lazy snapshot evidence JSON validation failed: "
                << validation_error.toUtf8().constData()
                << '\n';
            return 3;
        }
    }

    return root.value(QStringLiteral("status")).toString() == QStringLiteral("ok")
        ? 0
        : 1;
}
