#include "vnm_terminal/internal/terminal_session.h"

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(Q_OS_LINUX)
#include <unistd.h>
#endif
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

constexpr int k_default_iterations       = 7;
constexpr int k_default_warmup           = 2;
constexpr int k_default_rows             = 24;
constexpr int k_default_columns          = 80;
constexpr int k_default_scrollback_limit = 10000;
constexpr int k_default_wheel_events     = 240;
constexpr int k_schema_version           = 1;
constexpr const char* k_hidden_text_marker = "HIDDEN";
constexpr const char* k_hidden_hyperlink_uri_marker = "phase7.hidden.invalid";
constexpr const char* k_expected_release_reconciliation_result = "exact_anchor";
constexpr const char* k_expected_release_snapshot_basis        = "LIVE_CONTENT";
constexpr const char* k_expected_release_snapshot_purpose      = "CONTENT";

struct App_options
{
    int     iterations       = k_default_iterations;
    int     warmup           = k_default_warmup;
    int     rows             = k_default_rows;
    int     columns          = k_default_columns;
    int     scrollback_limit = k_default_scrollback_limit;
    int     wheel_events     = k_default_wheel_events;
    bool    validate_json    = false;
    bool    quiet            = false;
    bool    help_requested   = false;
    QString output_path;
    QString command_line;
};

struct Parse_result
{
    App_options options;
    QString     error;
};

struct Attempt_result
{
    bool                 ok                                  = false;
    QString              error;
    qint64               entry_capture_ns                    = 0;
    qint64               wheel_total_ns                      = 0;
    qint64               release_ns                          = 0;
    int                  requested_wheel_events              = 0;
    int                  moved_wheel_events                  = 0;
    int                  public_scroll_snapshot_events       = 0;
    std::uint64_t        snapshot_generation_delta_on_wheel  = 0U;
    int                  safe_scrollback_rows                = 0;
    int                  safe_visible_rows                   = 0;
    std::size_t          projection_stored_rows              = 0U;
    std::size_t          projection_copied_row_bound         = 0U;
    std::size_t          projection_row_capture_snapshots    = 0U;
    std::size_t          projection_copied_cells             = 0U;
    bool                 projection_full_row_source          = false;
    bool                 hidden_text_leak_observed_during_hold      = false;
    bool                 hidden_style_leak_observed_during_hold     = false;
    bool                 hidden_hyperlink_leak_observed_during_hold = false;
    bool                 hidden_cursor_leak_observed_during_hold    = false;
    bool                 hidden_mode_leak_observed_during_hold      = false;
    bool                 hidden_leak_observed_during_hold           = false;
    int                  public_scroll_cursor_visible_snapshots     = 0;
    QString              release_reconciliation_result;
    QString              release_snapshot_basis;
    QString              release_snapshot_purpose;
    int                  release_offset_from_tail            = 0;
    int                  expected_release_offset_from_tail   = 0;
    std::optional<qint64> rss_before_projection_bytes;
    std::optional<qint64> rss_after_projection_bytes;
    std::optional<qint64> rss_after_wheel_bytes;
    std::optional<qint64> rss_after_release_bytes;
};

struct Public_state_oracle_result
{
    bool text_leak      = false;
    bool style_leak     = false;
    bool hyperlink_leak = false;
    bool cursor_leak    = false;
    bool mode_leak      = false;
};

class Benchmark_backend final : public term::Terminal_backend
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
        return m_running
            ? term::backend_accept()
            : term::backend_reject(
                term::Terminal_backend_error_code::WRITE_FAILED,
                QStringLiteral("benchmark backend is not running"));
    }

    term::Terminal_backend_result resize(term::Terminal_backend_resize_request request) override
    {
        return term::is_valid_grid_size(request.grid_size)
            ? term::backend_accept()
            : term::backend_reject(
                term::Terminal_backend_error_code::RESIZE_FAILED,
                QStringLiteral("benchmark backend rejected invalid grid size"));
    }

    term::Terminal_backend_result set_output_paused(bool paused) override
    {
        m_output_paused = paused;
        return term::backend_accept();
    }

    term::Terminal_backend_result interrupt() override
    {
        m_running = false;
        m_callbacks.process_exited({term::Terminal_exit_reason::INTERRUPTED, 130});
        return term::backend_accept();
    }

    term::Terminal_backend_result terminate() override
    {
        m_running = false;
        m_callbacks.process_exited({term::Terminal_exit_reason::TERMINATED, 0});
        return term::backend_accept();
    }

    bool emit_output(QByteArray bytes)
    {
        if (!m_running || m_output_paused) {
            return false;
        }

        m_callbacks.output_received(std::move(bytes));
        return true;
    }

private:
    term::Terminal_backend_callbacks m_callbacks;
    std::vector<QByteArray>          m_writes;
    bool                             m_running       = false;
    bool                             m_output_paused = false;
};

using steady_clock_t = std::chrono::steady_clock;

QStringList raw_arguments(int argc, char** argv)
{
    QStringList arguments;
    for (int i = 0; i < argc; ++i) {
        arguments.push_back(QString::fromLocal8Bit(argv[i]));
    }
    return arguments;
}

QString shell_quoted_argument(QString argument)
{
    if (argument.isEmpty()) {
        return QStringLiteral("\"\"");
    }

    const bool needs_quotes =
        argument.contains(QLatin1Char(' ')) ||
        argument.contains(QLatin1Char('\t')) ||
        argument.contains(QLatin1Char('"'));
    if (!needs_quotes) {
        return argument;
    }

    argument.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("\"%1\"").arg(argument);
}

QString command_line_from_arguments(const QStringList& arguments)
{
    QStringList quoted;
    for (const QString& argument : arguments) {
        quoted.push_back(shell_quoted_argument(argument));
    }
    return quoted.join(QLatin1Char(' '));
}

void print_usage()
{
    std::cout
        << "usage: vnm_terminal_phase7_public_scroll_benchmark [options]\n"
        << "  --iterations <n>          measured attempts, default 7\n"
        << "  --warmup <n>              unreported warmup attempts, default 2\n"
        << "  --scrollback-limit <n>    public scrollback rows, default 10000\n"
        << "  --rows <n>                grid rows, default 24\n"
        << "  --columns <n>             grid columns, default 80\n"
        << "  --wheel-events <n>        held public-scroll requests, default 240\n"
        << "  --output <path>           write JSON report\n"
        << "  --validate-json           validate emitted JSON shape\n"
        << "  --quiet                   suppress stdout when --output is used\n";
}

bool parse_int_value(
    const QString& text,
    int            minimum,
    int            maximum,
    int*           out_value)
{
    bool ok    = false;
    int  value = text.toInt(&ok);
    if (!ok || value < minimum || value > maximum) {
        return false;
    }

    *out_value = value;
    return true;
}

bool option_value(
    const QStringList& arguments,
    int*               index,
    const QString&     option_name,
    QString*           out_value,
    QString*           out_error)
{
    const QString argument = arguments[*index];
    if (argument == option_name) {
        if (*index + 1 >= arguments.size()) {
            *out_error = QStringLiteral("%1 requires a value").arg(option_name);
            return false;
        }

        ++(*index);
        *out_value = arguments[*index];
        return true;
    }

    const QString prefix = option_name + QLatin1Char('=');
    if (argument.startsWith(prefix)) {
        *out_value = argument.mid(prefix.size());
        return true;
    }

    *out_error = QStringLiteral("internal parser error for %1").arg(option_name);
    return false;
}

bool parse_int_option(
    const QStringList& arguments,
    int*               index,
    const QString&     option_name,
    int                minimum,
    int                maximum,
    int*               target,
    QString*           out_error)
{
    QString value;
    if (!option_value(arguments, index, option_name, &value, out_error)) {
        return false;
    }

    if (!parse_int_value(value, minimum, maximum, target)) {
        *out_error = QStringLiteral("%1 requires an integer in [%2, %3]")
            .arg(option_name)
            .arg(minimum)
            .arg(maximum);
        return false;
    }

    return true;
}

Parse_result parse_arguments(const QStringList& arguments)
{
    Parse_result result;
    result.options.command_line = command_line_from_arguments(arguments);

    for (int i = 1; i < arguments.size(); ++i) {
        const QString argument = arguments[i];
        if (argument == QStringLiteral("--help")) {
            result.options.help_requested = true;
        }
        else
        if (argument == QStringLiteral("--validate-json")) {
            result.options.validate_json = true;
        }
        else
        if (argument == QStringLiteral("--quiet")) {
            result.options.quiet = true;
        }
        else
        if (argument == QStringLiteral("--iterations") ||
            argument.startsWith(QStringLiteral("--iterations=")))
        {
            if (!parse_int_option(
                    arguments, &i, QStringLiteral("--iterations"), 1, 1000,
                    &result.options.iterations, &result.error))
            {
                return result;
            }
        }
        else
        if (argument == QStringLiteral("--warmup") ||
            argument.startsWith(QStringLiteral("--warmup=")))
        {
            if (!parse_int_option(
                    arguments, &i, QStringLiteral("--warmup"), 0, 1000,
                    &result.options.warmup, &result.error))
            {
                return result;
            }
        }
        else
        if (argument == QStringLiteral("--scrollback-limit") ||
            argument.startsWith(QStringLiteral("--scrollback-limit=")))
        {
            if (!parse_int_option(
                    arguments, &i, QStringLiteral("--scrollback-limit"), 1, 100000,
                    &result.options.scrollback_limit, &result.error))
            {
                return result;
            }
        }
        else
        if (argument == QStringLiteral("--rows") ||
            argument.startsWith(QStringLiteral("--rows=")))
        {
            if (!parse_int_option(
                    arguments, &i, QStringLiteral("--rows"), 1, 500,
                    &result.options.rows, &result.error))
            {
                return result;
            }
        }
        else
        if (argument == QStringLiteral("--columns") ||
            argument.startsWith(QStringLiteral("--columns=")))
        {
            if (!parse_int_option(
                    arguments, &i, QStringLiteral("--columns"), 1, 1000,
                    &result.options.columns, &result.error))
            {
                return result;
            }
        }
        else
        if (argument == QStringLiteral("--wheel-events") ||
            argument.startsWith(QStringLiteral("--wheel-events=")))
        {
            if (!parse_int_option(
                    arguments, &i, QStringLiteral("--wheel-events"), 1, 100000,
                    &result.options.wheel_events, &result.error))
            {
                return result;
            }
        }
        else
        if (argument == QStringLiteral("--output") ||
            argument.startsWith(QStringLiteral("--output=")))
        {
            if (!option_value(
                    arguments, &i, QStringLiteral("--output"),
                    &result.options.output_path, &result.error))
            {
                return result;
            }
            if (result.options.output_path.isEmpty()) {
                result.error = QStringLiteral("--output requires a non-empty path");
                return result;
            }
        }
        else {
            result.error = QStringLiteral("unknown option: %1").arg(argument);
            return result;
        }
    }

    return result;
}

std::optional<qint64> process_resident_memory_bytes()
{
#if defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS_EX counters;
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters)))
    {
        return static_cast<qint64>(counters.WorkingSetSize);
    }
    return std::nullopt;
#elif defined(Q_OS_LINUX)
    QFile file(QStringLiteral("/proc/self/statm"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return std::nullopt;
    }

    const QList<QByteArray> fields = file.readAll().split(' ');
    if (fields.size() < 2) {
        return std::nullopt;
    }

    bool  ok             = false;
    qint64 resident_pages = fields[1].trimmed().toLongLong(&ok);
    if (!ok) {
        return std::nullopt;
    }

    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return std::nullopt;
    }

    return resident_pages * static_cast<qint64>(page_size);
#else
    return std::nullopt;
#endif
}

qint64 elapsed_nanoseconds(steady_clock_t::time_point start, steady_clock_t::time_point end)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
}

term::Terminal_launch_config launch_config_for_options(const App_options& options)
{
    term::Terminal_launch_config config;
    config.argv              = {QStringLiteral("phase7-benchmark-fixture")};
    config.working_directory = QStringLiteral("C:/workspace");
    config.initial_grid_size = term::terminal_grid_size_t{options.rows, options.columns};
    return config;
}

term::Terminal_session_config session_config_for_options(const App_options& options)
{
    term::Terminal_session_config config;
    config.output_queue_limits.high_water_bytes = 4U * 1024U * 1024U;
    config.output_queue_limits.hard_limit_bytes = 8U * 1024U * 1024U;
    config.scrollback_limit = options.scrollback_limit;
    config.synchronized_output_scroll_policy =
        term::Terminal_synchronized_output_scroll_policy::IMMEDIATE_PUBLIC_PROJECTION;
    config.trace_notification_limit = static_cast<std::size_t>(options.wheel_events + 32);
    return config;
}

QByteArray padded_line(const QString& prefix, int columns)
{
    QString line = prefix;
    const int payload_columns = std::max(1, columns - 1);
    while (line.size() < payload_columns) {
        line += QLatin1Char('p');
    }
    line.truncate(payload_columns);
    line += QStringLiteral("\r\n");
    return line.toUtf8();
}

QByteArray make_public_seed_output(const App_options& options)
{
    QByteArray output;
    const int line_count = options.scrollback_limit + options.rows + 16;
    for (int row = 0; row < line_count; ++row) {
        output += padded_line(
            QStringLiteral("public-row-%1 ").arg(row, 8, 10, QLatin1Char('0')),
            options.columns);
    }
    output += QByteArrayLiteral(
        "\x1b[38;2;34;168;96m"
        "\x1b]8;id=phase7-safe;https://phase7.safe.invalid/public\x1b\\");
    output += padded_line(QStringLiteral("public-safe-style-link "), options.columns);
    output += QByteArrayLiteral(
        "\x1b]8;;\x1b\\"
        "\x1b[0m"
        "\x1b[6;9H"
        "\x1b[2 q"
        "\x1b[?25h"
        "\x1b[?1l"
        "\x1b[?5l"
        "\x1b[?6l"
        "\x1b[?7h"
        "\x1b[?1002l"
        "\x1b[?1006l"
        "\x1b[?1007l"
        "\x1b[?2004l");
    return output;
}

QByteArray make_hidden_hold_output(const App_options& options)
{
    QByteArray output;
    output += QByteArrayLiteral(
        "\x1b[?25l"
        "\x1b[6 q"
        "\x1b[1;1H"
        "\x1b[?1h"
        "\x1b[?5h"
        "\x1b[?6h"
        "\x1b[?7l"
        "\x1b[?1002h"
        "\x1b[?1006h"
        "\x1b[?1007h"
        "\x1b[?2004h"
        "\x1b[1;4;38;2;240;32;80;48;2;8;16;24m"
        "\x1b]8;id=phase7-hidden;https://phase7.hidden.invalid/hold\x1b\\");
    const int line_count = options.rows * 4;
    for (int row = 0; row < line_count; ++row) {
        output += padded_line(
            QStringLiteral("HIDDEN-HOLD-ROW-%1 ").arg(row, 4, 10, QLatin1Char('0')),
            options.columns);
    }
    output += QByteArrayLiteral(
        "\x1b]8;;\x1b\\"
        "\x1b[0m");
    return output;
}

int expected_release_offset_from_tail(const App_options& options)
{
    const int hidden_hold_lines         = options.rows * 4;
    const int hidden_hold_scroll_growth =
        std::max(0, hidden_hold_lines - options.rows + 1);
    return options.wheel_events + hidden_hold_scroll_growth;
}

QString projection_row_text(const term::Terminal_public_projection_row& row)
{
    QString text;
    std::vector<const term::Terminal_render_cell*> cells(
        static_cast<std::size_t>(std::max(0, row.cells.empty() ? 0 : row.cells.back().position.column + 1)),
        nullptr);

    int max_column = 0;
    for (const term::Terminal_render_cell& cell : row.cells) {
        max_column = std::max(max_column, cell.position.column);
    }
    cells.assign(static_cast<std::size_t>(max_column + 1), nullptr);
    for (const term::Terminal_render_cell& cell : row.cells) {
        if (cell.position.column >= 0) {
            cells[static_cast<std::size_t>(cell.position.column)] = &cell;
        }
    }

    for (const term::Terminal_render_cell* cell : cells) {
        if (cell == nullptr || cell->wide_continuation) {
            continue;
        }
        text += cell->text;
    }
    return text;
}

bool projection_contains_text(
    const term::Terminal_public_projection& projection,
    const QString&                          needle)
{
    for (const term::Terminal_public_projection_row& row : projection.rows()) {
        if (projection_row_text(row).contains(needle)) {
            return true;
        }
    }
    return false;
}

bool color_states_equal(
    const term::Terminal_color_state& left,
    const term::Terminal_color_state& right)
{
    return
        left.default_foreground_rgba == right.default_foreground_rgba &&
        left.default_background_rgba == right.default_background_rgba &&
        left.cursor_rgba             == right.cursor_rgba             &&
        left.palette_rgba            == right.palette_rgba;
}

bool hyperlinks_equal(
    const std::vector<term::Terminal_render_hyperlink_metadata>& left,
    const std::vector<term::Terminal_render_hyperlink_metadata>& right)
{
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t i = 0U; i < left.size(); ++i) {
        if (left[i].hyperlink_id != right[i].hyperlink_id ||
            left[i].identity_key != right[i].identity_key ||
            left[i].uri          != right[i].uri)
        {
            return false;
        }
    }

    return true;
}

bool hyperlinks_contain_uri_marker(
    const std::vector<term::Terminal_render_hyperlink_metadata>& hyperlinks,
    const QByteArray&                                            marker)
{
    for (const term::Terminal_render_hyperlink_metadata& hyperlink : hyperlinks) {
        if (hyperlink.uri.contains(marker)) {
            return true;
        }
    }
    return false;
}

bool cursors_equal(
    const term::Terminal_render_cursor& left,
    const term::Terminal_render_cursor& right)
{
    return
        left.position      == right.position      &&
        left.shape         == right.shape         &&
        left.visible       == right.visible       &&
        left.blink_enabled == right.blink_enabled;
}

bool mode_states_equal(
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

std::size_t copied_cell_count(const term::Terminal_public_projection& projection)
{
    std::size_t count = 0U;
    for (const term::Terminal_public_projection_row& row : projection.rows()) {
        count += row.cells.size();
    }
    return count;
}

QString snapshot_row_text(const term::Terminal_render_snapshot& snapshot, int row)
{
    QString text;
    const std::vector<const term::Terminal_render_cell*> cells_by_position =
        term::render_snapshot_cells_by_position(snapshot);
    for (int column = 0; column < snapshot.grid_size.columns; ++column) {
        const term::Terminal_render_cell* cell =
            term::render_snapshot_cell_at(cells_by_position, snapshot.grid_size, row, column);
        if (cell == nullptr) {
            text += QLatin1Char(' ');
            continue;
        }
        if (!cell->wide_continuation) {
            text += cell->text;
        }
    }
    return text;
}

bool snapshot_contains_text(const term::Terminal_render_snapshot& snapshot, const QString& needle)
{
    for (int row = 0; row < snapshot.grid_size.rows; ++row) {
        if (snapshot_row_text(snapshot, row).contains(needle)) {
            return true;
        }
    }
    return false;
}

term::Terminal_render_cursor expected_cursor_for_public_scroll_snapshot(
    const term::Terminal_render_snapshot& safe_content,
    const term::Terminal_render_snapshot& snapshot)
{
    term::Terminal_render_cursor expected = safe_content.cursor;
    if (!safe_content.cursor.visible) {
        return expected;
    }

    const int safe_first_row =
        term::render_snapshot_first_visible_logical_row(safe_content);
    const int snapshot_first_row =
        term::render_snapshot_first_visible_logical_row(snapshot);
    expected.position.row =
        safe_first_row + safe_content.cursor.position.row - snapshot_first_row;
    expected.visible =
        expected.position.row >= 0 &&
        expected.position.row <  snapshot.grid_size.rows;
    return expected;
}

bool cursor_matches_public_scroll_snapshot(
    const term::Terminal_render_snapshot& safe_content,
    const term::Terminal_render_snapshot& snapshot)
{
    const term::Terminal_render_cursor expected =
        expected_cursor_for_public_scroll_snapshot(safe_content, snapshot);
    if (snapshot.cursor.shape         != expected.shape ||
        snapshot.cursor.blink_enabled != expected.blink_enabled)
    {
        return false;
    }

    if (!expected.visible) {
        return
            !snapshot.cursor.visible &&
            snapshot.cursor.position.column == safe_content.cursor.position.column;
    }

    return cursors_equal(snapshot.cursor, expected);
}

Public_state_oracle_result projection_oracle_result(
    const term::Terminal_public_projection& projection,
    const term::Terminal_render_snapshot&   safe_content)
{
    Public_state_oracle_result result;
    result.text_leak =
        projection_contains_text(projection, QString::fromLatin1(k_hidden_text_marker));
    result.style_leak =
        projection.styles() != safe_content.styles ||
        !color_states_equal(projection.color_state(), safe_content.color_state);
    result.hyperlink_leak =
        !hyperlinks_equal(projection.hyperlinks(), safe_content.hyperlinks) ||
        hyperlinks_contain_uri_marker(
            projection.hyperlinks(),
            QByteArray(k_hidden_hyperlink_uri_marker));
    result.cursor_leak = !cursors_equal(projection.cursor(), safe_content.cursor);
    result.mode_leak   = !mode_states_equal(projection.modes(), safe_content.modes);
    return result;
}

Public_state_oracle_result public_scroll_snapshot_oracle_result(
    const term::Terminal_render_snapshot& snapshot,
    const term::Terminal_render_snapshot& safe_content)
{
    Public_state_oracle_result result;
    result.text_leak =
        snapshot_contains_text(snapshot, QString::fromLatin1(k_hidden_text_marker));
    result.style_leak =
        snapshot.styles != safe_content.styles ||
        !color_states_equal(snapshot.color_state, safe_content.color_state);
    result.hyperlink_leak =
        !hyperlinks_equal(snapshot.hyperlinks, safe_content.hyperlinks) ||
        hyperlinks_contain_uri_marker(
            snapshot.hyperlinks,
            QByteArray(k_hidden_hyperlink_uri_marker));
    result.cursor_leak = !cursor_matches_public_scroll_snapshot(safe_content, snapshot);
    result.mode_leak   = !mode_states_equal(snapshot.modes, safe_content.modes);
    return result;
}

void merge_oracle_result(Attempt_result& attempt, const Public_state_oracle_result& oracle)
{
    attempt.hidden_text_leak_observed_during_hold      |= oracle.text_leak;
    attempt.hidden_style_leak_observed_during_hold     |= oracle.style_leak;
    attempt.hidden_hyperlink_leak_observed_during_hold |= oracle.hyperlink_leak;
    attempt.hidden_cursor_leak_observed_during_hold    |= oracle.cursor_leak;
    attempt.hidden_mode_leak_observed_during_hold      |= oracle.mode_leak;
    attempt.hidden_leak_observed_during_hold =
        attempt.hidden_text_leak_observed_during_hold      ||
        attempt.hidden_style_leak_observed_during_hold     ||
        attempt.hidden_hyperlink_leak_observed_during_hold ||
        attempt.hidden_cursor_leak_observed_during_hold    ||
        attempt.hidden_mode_leak_observed_during_hold;
}

QString hidden_leak_error_message(const Attempt_result& attempt, const QString& source)
{
    QStringList categories;
    if (attempt.hidden_text_leak_observed_during_hold) {
        categories.push_back(QStringLiteral("text"));
    }
    if (attempt.hidden_style_leak_observed_during_hold) {
        categories.push_back(QStringLiteral("style"));
    }
    if (attempt.hidden_hyperlink_leak_observed_during_hold) {
        categories.push_back(QStringLiteral("hyperlink"));
    }
    if (attempt.hidden_cursor_leak_observed_during_hold) {
        categories.push_back(QStringLiteral("cursor"));
    }
    if (attempt.hidden_mode_leak_observed_during_hold) {
        categories.push_back(QStringLiteral("mode"));
    }

    return QStringLiteral("%1 exposed hidden/public-state sentinel(s): %2")
        .arg(source)
        .arg(categories.join(QStringLiteral(", ")));
}

Attempt_result fail_attempt(QString error)
{
    Attempt_result result;
    result.error = std::move(error);
    return result;
}

Attempt_result run_attempt(const App_options& options)
{
    auto backend = std::make_unique<Benchmark_backend>();
    Benchmark_backend* backend_ptr = backend.get();
    term::Terminal_session session(std::move(backend), session_config_for_options(options));

    if (session.start(launch_config_for_options(options)).code !=
        term::Terminal_session_result_code::ACCEPTED)
    {
        return fail_attempt(QStringLiteral("session start failed"));
    }

    if (!backend_ptr->emit_output(make_public_seed_output(options))) {
        return fail_attempt(QStringLiteral("public seed output was rejected"));
    }

    const std::optional<term::Terminal_render_snapshot> safe_content =
        session.latest_content_render_snapshot_for_testing();
    if (!safe_content.has_value()) {
        return fail_attempt(QStringLiteral("safe content snapshot was not published"));
    }

    Attempt_result result;
    result.requested_wheel_events = options.wheel_events;
    result.safe_scrollback_rows   = safe_content->viewport.scrollback_rows;
    result.safe_visible_rows      = safe_content->viewport.visible_rows;
    result.expected_release_offset_from_tail = expected_release_offset_from_tail(options);
    result.rss_before_projection_bytes = process_resident_memory_bytes();

    const auto entry_start = steady_clock_t::now();
    if (!backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026h"))) {
        return fail_attempt(QStringLiteral("synchronized-output entry was rejected"));
    }
    const auto entry_end = steady_clock_t::now();
    result.entry_capture_ns = elapsed_nanoseconds(entry_start, entry_end);
    result.rss_after_projection_bytes = process_resident_memory_bytes();

    if (!backend_ptr->emit_output(QByteArrayLiteral("HIDDEN-ENTRY-SENTINEL\r\n"))) {
        return fail_attempt(QStringLiteral("hidden entry suffix was rejected"));
    }

    const std::optional<term::Terminal_public_projection> projection =
        session.public_projection_for_testing();
    if (!projection.has_value()) {
        return fail_attempt(QStringLiteral("immediate policy did not capture a public projection"));
    }

    result.projection_stored_rows           = projection->stored_row_count();
    result.projection_copied_row_bound      = projection->copied_row_bound();
    result.projection_row_capture_snapshots = projection->row_capture_snapshot_count();
    result.projection_copied_cells          = copied_cell_count(*projection);
    result.projection_full_row_source       = !projection->rows_are_safe_basis_viewport_only();
    merge_oracle_result(result, projection_oracle_result(*projection, *safe_content));
    if (result.hidden_leak_observed_during_hold) {
        result.error =
            hidden_leak_error_message(result, QStringLiteral("public projection"));
        return result;
    }

    if (!backend_ptr->emit_output(make_hidden_hold_output(options))) {
        return fail_attempt(QStringLiteral("hidden hold output was rejected"));
    }

    term::Terminal_viewport_state published_viewport = safe_content->viewport;
    const std::uint64_t generation_before_wheel = session.render_snapshot_generation();
    const auto wheel_start = steady_clock_t::now();
    for (int event_index = 0; event_index < options.wheel_events; ++event_index) {
        const term::Terminal_viewport_scroll_result scroll_result =
            session.scroll_viewport_lines_from_published_state(1, published_viewport);
        if (scroll_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED) {
            ++result.moved_wheel_events;
        }

        const std::optional<term::Terminal_render_snapshot> snapshot =
            session.latest_render_snapshot();
        if (!snapshot.has_value()) {
            continue;
        }

        if (snapshot->basis == term::Terminal_render_snapshot_basis::PUBLIC_PROJECTION &&
            snapshot->purpose == term::Terminal_render_snapshot_purpose::SCROLL)
        {
            ++result.public_scroll_snapshot_events;
            if (snapshot->cursor.visible) {
                ++result.public_scroll_cursor_visible_snapshots;
            }
            merge_oracle_result(
                result,
                public_scroll_snapshot_oracle_result(*snapshot, *safe_content));
        }
        published_viewport = snapshot->viewport;
    }
    const auto wheel_end = steady_clock_t::now();
    result.wheel_total_ns = elapsed_nanoseconds(wheel_start, wheel_end);
    result.snapshot_generation_delta_on_wheel =
        session.render_snapshot_generation() - generation_before_wheel;
    result.rss_after_wheel_bytes = process_resident_memory_bytes();

    if (result.hidden_leak_observed_during_hold) {
        result.error =
            hidden_leak_error_message(result, QStringLiteral("public scroll snapshot"));
        return result;
    }

    const auto release_start = steady_clock_t::now();
    if (!backend_ptr->emit_output(QByteArrayLiteral("\x1b[?2026l"))) {
        return fail_attempt(QStringLiteral("synchronized-output release was rejected"));
    }

    const std::optional<term::Terminal_render_snapshot> release_snapshot =
        session.latest_render_snapshot();
    const auto release_end = steady_clock_t::now();
    result.release_ns = elapsed_nanoseconds(release_start, release_end);
    result.rss_after_release_bytes = process_resident_memory_bytes();
    if (!release_snapshot.has_value()) {
        return fail_attempt(QStringLiteral("release produced no render snapshot"));
    }

    result.release_reconciliation_result =
        term::release_reconciliation_result_name(
            release_snapshot->public_scroll_diagnostics.release_reconciliation_result);
    result.release_offset_from_tail = release_snapshot->viewport.offset_from_tail;
    result.release_snapshot_basis   = term::render_snapshot_basis_name(release_snapshot->basis);
    result.release_snapshot_purpose = term::render_snapshot_purpose_name(release_snapshot->purpose);

    const std::size_t expected_copied_rows =
        static_cast<std::size_t>(result.safe_scrollback_rows + result.safe_visible_rows);
    const std::size_t max_copied_cells =
        expected_copied_rows * static_cast<std::size_t>(options.columns);
    const bool copied_rows_exact =
        result.projection_stored_rows      == expected_copied_rows &&
        result.projection_copied_row_bound == expected_copied_rows;
    const bool copied_cells_bounded =
        result.projection_copied_cells > 0U &&
        result.projection_copied_cells <= max_copied_cells;
    const bool row_captures_bounded =
        result.projection_row_capture_snapshots >  0U &&
        result.projection_row_capture_snapshots <= expected_copied_rows;
    const bool wheel_events_exact =
        result.moved_wheel_events == result.requested_wheel_events;
    const bool wheel_snapshots_exact =
        result.public_scroll_snapshot_events == result.moved_wheel_events &&
        result.snapshot_generation_delta_on_wheel ==
            static_cast<std::uint64_t>(result.moved_wheel_events);
    const bool release_result_expected =
        result.release_reconciliation_result ==
            QString::fromLatin1(k_expected_release_reconciliation_result);
    const bool release_snapshot_basis_expected =
        result.release_snapshot_basis   == QString::fromLatin1(k_expected_release_snapshot_basis) &&
        result.release_snapshot_purpose == QString::fromLatin1(k_expected_release_snapshot_purpose);
    const bool release_offset_expected =
        result.release_offset_from_tail == result.expected_release_offset_from_tail;

    result.ok =
        copied_rows_exact                           &&
        copied_cells_bounded                        &&
        row_captures_bounded                        &&
        wheel_events_exact                          &&
        wheel_snapshots_exact                       &&
        result.projection_full_row_source           &&
        result.public_scroll_cursor_visible_snapshots > 0 &&
        release_result_expected                     &&
        release_snapshot_basis_expected             &&
        release_offset_expected                     &&
        !result.hidden_leak_observed_during_hold;
    if (!result.ok && result.error.isEmpty()) {
        result.error = QStringLiteral("benchmark invariants failed");
    }
    return result;
}

void insert_i64(QJsonObject& object, const QString& name, qint64 value)
{
    object.insert(name, value);
}

void insert_u64(QJsonObject& object, const QString& name, std::uint64_t value)
{
    object.insert(name, static_cast<qint64>(value));
}

void insert_size(QJsonObject& object, const QString& name, std::size_t value)
{
    object.insert(name, static_cast<qint64>(value));
}

void insert_optional_i64(
    QJsonObject&                 object,
    const QString&               name,
    const std::optional<qint64>& value)
{
    if (value.has_value()) {
        object.insert(name, *value);
    }
    else {
        object.insert(name, QJsonValue::Null);
    }
}

QJsonObject attempt_json(const Attempt_result& result)
{
    QJsonObject object;
    object.insert(QStringLiteral("status"), result.ok ? QStringLiteral("ok") : QStringLiteral("failed"));
    if (!result.error.isEmpty()) {
        object.insert(QStringLiteral("error"), result.error);
    }
    insert_i64(object, QStringLiteral("entry_capture_ns"), result.entry_capture_ns);
    insert_i64(object, QStringLiteral("wheel_total_ns"), result.wheel_total_ns);
    insert_i64(
        object,
        QStringLiteral("wheel_mean_per_event_ns"),
        result.requested_wheel_events > 0
            ? result.wheel_total_ns / result.requested_wheel_events
            : 0);
    insert_i64(object, QStringLiteral("release_ns"), result.release_ns);
    object.insert(QStringLiteral("requested_wheel_events"), result.requested_wheel_events);
    object.insert(QStringLiteral("moved_wheel_events"), result.moved_wheel_events);
    object.insert(
        QStringLiteral("public_scroll_snapshot_events"),
        result.public_scroll_snapshot_events);
    insert_u64(
        object,
        QStringLiteral("snapshot_generation_delta_on_wheel"),
        result.snapshot_generation_delta_on_wheel);
    object.insert(QStringLiteral("safe_scrollback_rows"), result.safe_scrollback_rows);
    object.insert(QStringLiteral("safe_visible_rows"), result.safe_visible_rows);
    insert_size(
        object,
        QStringLiteral("projection_stored_rows"),
        result.projection_stored_rows);
    insert_size(
        object,
        QStringLiteral("projection_copied_row_bound"),
        result.projection_copied_row_bound);
    insert_size(
        object,
        QStringLiteral("projection_row_capture_snapshots"),
        result.projection_row_capture_snapshots);
    insert_size(
        object,
        QStringLiteral("projection_copied_cells"),
        result.projection_copied_cells);
    object.insert(
        QStringLiteral("projection_full_row_source"),
        result.projection_full_row_source);
    object.insert(
        QStringLiteral("hidden_text_leak_observed_during_hold"),
        result.hidden_text_leak_observed_during_hold);
    object.insert(
        QStringLiteral("hidden_style_leak_observed_during_hold"),
        result.hidden_style_leak_observed_during_hold);
    object.insert(
        QStringLiteral("hidden_hyperlink_leak_observed_during_hold"),
        result.hidden_hyperlink_leak_observed_during_hold);
    object.insert(
        QStringLiteral("hidden_cursor_leak_observed_during_hold"),
        result.hidden_cursor_leak_observed_during_hold);
    object.insert(
        QStringLiteral("hidden_mode_leak_observed_during_hold"),
        result.hidden_mode_leak_observed_during_hold);
    object.insert(
        QStringLiteral("hidden_leak_observed_during_hold"),
        result.hidden_leak_observed_during_hold);
    object.insert(
        QStringLiteral("public_scroll_cursor_visible_snapshots"),
        result.public_scroll_cursor_visible_snapshots);
    object.insert(
        QStringLiteral("release_reconciliation_result"),
        result.release_reconciliation_result);
    object.insert(QStringLiteral("release_snapshot_basis"), result.release_snapshot_basis);
    object.insert(QStringLiteral("release_snapshot_purpose"), result.release_snapshot_purpose);
    object.insert(QStringLiteral("release_offset_from_tail"), result.release_offset_from_tail);
    object.insert(
        QStringLiteral("expected_release_offset_from_tail"),
        result.expected_release_offset_from_tail);
    insert_optional_i64(
        object,
        QStringLiteral("rss_before_projection_bytes"),
        result.rss_before_projection_bytes);
    insert_optional_i64(
        object,
        QStringLiteral("rss_after_projection_bytes"),
        result.rss_after_projection_bytes);
    insert_optional_i64(
        object,
        QStringLiteral("rss_after_wheel_bytes"),
        result.rss_after_wheel_bytes);
    insert_optional_i64(
        object,
        QStringLiteral("rss_after_release_bytes"),
        result.rss_after_release_bytes);
    if (result.rss_before_projection_bytes.has_value() &&
        result.rss_after_projection_bytes.has_value())
    {
        insert_i64(
            object,
            QStringLiteral("rss_projection_delta_bytes"),
            *result.rss_after_projection_bytes - *result.rss_before_projection_bytes);
    }
    else {
        object.insert(QStringLiteral("rss_projection_delta_bytes"), QJsonValue::Null);
    }
    return object;
}

QJsonObject nanosecond_summary(std::vector<qint64> values)
{
    QJsonObject object;
    object.insert(QStringLiteral("sample_count"), static_cast<int>(values.size()));
    object.insert(QStringLiteral("unit"), QStringLiteral("ns"));
    if (values.empty()) {
        return object;
    }

    std::sort(values.begin(), values.end());
    const qint64 sum = std::accumulate(values.begin(), values.end(), qint64{0});
    const auto percentile = [&values](double fraction) {
        const std::size_t index = static_cast<std::size_t>(
            fraction * static_cast<double>(values.size() - 1U));
        return values[index];
    };

    insert_i64(object, QStringLiteral("min"), values.front());
    insert_i64(object, QStringLiteral("median"), percentile(0.50));
    insert_i64(object, QStringLiteral("p95"), percentile(0.95));
    insert_i64(object, QStringLiteral("max"), values.back());
    object.insert(
        QStringLiteral("mean"),
        static_cast<double>(sum) / static_cast<double>(values.size()));
    return object;
}

QJsonObject i64_summary(std::vector<qint64> values, const QString& unit)
{
    QJsonObject object = nanosecond_summary(std::move(values));
    object.insert(QStringLiteral("unit"), unit);
    return object;
}

QJsonObject make_summary_json(const std::vector<Attempt_result>& attempts)
{
    std::vector<qint64> entry_capture_ns;
    std::vector<qint64> wheel_total_ns;
    std::vector<qint64> wheel_per_event_ns;
    std::vector<qint64> release_ns;
    std::vector<qint64> rss_projection_delta_bytes;
    std::vector<qint64> rss_after_wheel_bytes;
    std::vector<qint64> rss_wheel_delta_from_projection_bytes;
    int max_public_scroll_snapshot_events = 0;
    int max_public_scroll_cursor_visible_snapshots = 0;
    std::uint64_t max_snapshot_generation_delta = 0U;
    std::size_t max_projection_stored_rows = 0U;
    std::size_t max_projection_copied_row_bound = 0U;
    std::size_t max_projection_copied_cells = 0U;

    for (const Attempt_result& attempt : attempts) {
        if (!attempt.ok) {
            continue;
        }

        entry_capture_ns.push_back(attempt.entry_capture_ns);
        wheel_total_ns.push_back(attempt.wheel_total_ns);
        wheel_per_event_ns.push_back(
            attempt.requested_wheel_events > 0
                ? attempt.wheel_total_ns / attempt.requested_wheel_events
                : 0);
        release_ns.push_back(attempt.release_ns);
        if (attempt.rss_before_projection_bytes.has_value() &&
            attempt.rss_after_projection_bytes.has_value())
        {
            rss_projection_delta_bytes.push_back(
                *attempt.rss_after_projection_bytes -
                *attempt.rss_before_projection_bytes);
        }
        if (attempt.rss_after_wheel_bytes.has_value()) {
            rss_after_wheel_bytes.push_back(*attempt.rss_after_wheel_bytes);
        }
        if (attempt.rss_after_projection_bytes.has_value() &&
            attempt.rss_after_wheel_bytes.has_value())
        {
            rss_wheel_delta_from_projection_bytes.push_back(
                *attempt.rss_after_wheel_bytes -
                *attempt.rss_after_projection_bytes);
        }
        max_public_scroll_snapshot_events =
            std::max(max_public_scroll_snapshot_events, attempt.public_scroll_snapshot_events);
        max_public_scroll_cursor_visible_snapshots =
            std::max(
                max_public_scroll_cursor_visible_snapshots,
                attempt.public_scroll_cursor_visible_snapshots);
        max_snapshot_generation_delta =
            std::max(max_snapshot_generation_delta, attempt.snapshot_generation_delta_on_wheel);
        max_projection_stored_rows =
            std::max(max_projection_stored_rows, attempt.projection_stored_rows);
        max_projection_copied_row_bound =
            std::max(max_projection_copied_row_bound, attempt.projection_copied_row_bound);
        max_projection_copied_cells =
            std::max(max_projection_copied_cells, attempt.projection_copied_cells);
    }

    QJsonObject object;
    object.insert(QStringLiteral("entry_capture"), nanosecond_summary(entry_capture_ns));
    object.insert(QStringLiteral("wheel_total"), nanosecond_summary(wheel_total_ns));
    object.insert(QStringLiteral("wheel_per_event"), nanosecond_summary(wheel_per_event_ns));
    object.insert(QStringLiteral("release"), nanosecond_summary(release_ns));
    object.insert(
        QStringLiteral("rss_projection_delta"),
        i64_summary(rss_projection_delta_bytes, QStringLiteral("bytes")));
    object.insert(
        QStringLiteral("rss_after_wheel"),
        i64_summary(rss_after_wheel_bytes, QStringLiteral("bytes")));
    object.insert(
        QStringLiteral("rss_wheel_delta_from_projection"),
        i64_summary(rss_wheel_delta_from_projection_bytes, QStringLiteral("bytes")));
    object.insert(
        QStringLiteral("max_public_scroll_snapshot_events_per_attempt"),
        max_public_scroll_snapshot_events);
    object.insert(
        QStringLiteral("max_public_scroll_cursor_visible_snapshots_per_attempt"),
        max_public_scroll_cursor_visible_snapshots);
    insert_u64(
        object,
        QStringLiteral("max_snapshot_generation_delta_per_attempt"),
        max_snapshot_generation_delta);
    insert_size(
        object,
        QStringLiteral("max_projection_stored_rows"),
        max_projection_stored_rows);
    insert_size(
        object,
        QStringLiteral("max_projection_copied_row_bound"),
        max_projection_copied_row_bound);
    insert_size(
        object,
        QStringLiteral("max_projection_copied_cells"),
        max_projection_copied_cells);
    return object;
}

QJsonObject make_options_json(const App_options& options)
{
    QJsonObject object;
    object.insert(QStringLiteral("iterations"), options.iterations);
    object.insert(QStringLiteral("warmup"), options.warmup);
    object.insert(QStringLiteral("rows"), options.rows);
    object.insert(QStringLiteral("columns"), options.columns);
    object.insert(QStringLiteral("scrollback_limit"), options.scrollback_limit);
    object.insert(QStringLiteral("wheel_events"), options.wheel_events);
    object.insert(QStringLiteral("command_line"), options.command_line);
    return object;
}

QJsonObject make_root_json(
    const App_options&                 options,
    const std::vector<Attempt_result>& attempts,
    bool                               warmup_ok,
    bool                               ok)
{
    QJsonArray attempt_array;
    for (const Attempt_result& attempt : attempts) {
        attempt_array.push_back(attempt_json(attempt));
    }

    QJsonObject root;
    root.insert(QStringLiteral("schema_version"), k_schema_version);
    root.insert(
        QStringLiteral("benchmark"),
        QStringLiteral("vnm_terminal_phase7_public_scroll_benchmark"));
    root.insert(
        QStringLiteral("status"),
        ok && warmup_ok ? QStringLiteral("ok") : QStringLiteral("failed"));
    root.insert(QStringLiteral("options"), make_options_json(options));
    root.insert(
        QStringLiteral("measurement_semantics"),
        QStringLiteral(
            "entry_capture excludes seed setup and covers DECSET 2026 processing plus public "
            "projection capture only; the hidden suffix is sent after entry timing; wheel timing "
            "covers synchronous public scroll requests during a DEC synchronized-output hold; "
            "release timing covers DECRST 2026 processing through release snapshot observation; "
            "memory is best-effort process RSS"));
    root.insert(
        QStringLiteral("memory_sample_metric"),
        QStringLiteral("resident_set_bytes"));
    root.insert(QStringLiteral("warmup_status"), warmup_ok ? QStringLiteral("ok") : QStringLiteral("failed"));
    root.insert(QStringLiteral("attempts"), attempt_array);
    root.insert(QStringLiteral("summary"), make_summary_json(attempts));
    return root;
}

bool validate_json_output(const QByteArray& json, const App_options& options, QString* out_error)
{
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        *out_error = QStringLiteral("benchmark output is not valid JSON: %1")
            .arg(parse_error.errorString());
        return false;
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schema_version")).toInt() != k_schema_version ||
        root.value(QStringLiteral("benchmark")).toString() !=
            QStringLiteral("vnm_terminal_phase7_public_scroll_benchmark") ||
        root.value(QStringLiteral("status")).toString() != QStringLiteral("ok"))
    {
        *out_error = QStringLiteral("benchmark root metadata changed or status failed");
        return false;
    }

    const QJsonArray attempts = root.value(QStringLiteral("attempts")).toArray();
    if (attempts.size() != options.iterations) {
        *out_error = QStringLiteral("benchmark attempt count does not match options");
        return false;
    }

    const int expected_rows = options.scrollback_limit + options.rows;
    const int expected_max_cells = expected_rows * options.columns;
    for (int attempt_index = 0; attempt_index < attempts.size(); ++attempt_index) {
        const QJsonValue& value = attempts[attempt_index];
        const QJsonObject attempt = value.toObject();
        if (attempt.value(QStringLiteral("status")).toString() != QStringLiteral("ok") ||
            attempt.value(QStringLiteral("hidden_leak_observed_during_hold")).toBool())
        {
            *out_error = QStringLiteral("benchmark attempt %1 failed or leaked hidden state")
                .arg(attempt_index);
            return false;
        }

        if (attempt.value(QStringLiteral("hidden_text_leak_observed_during_hold")).toBool() ||
            attempt.value(QStringLiteral("hidden_style_leak_observed_during_hold")).toBool() ||
            attempt.value(QStringLiteral("hidden_hyperlink_leak_observed_during_hold")).toBool() ||
            attempt.value(QStringLiteral("hidden_cursor_leak_observed_during_hold")).toBool() ||
            attempt.value(QStringLiteral("hidden_mode_leak_observed_during_hold")).toBool())
        {
            *out_error = QStringLiteral("benchmark attempt %1 leaked a hidden sentinel category")
                .arg(attempt_index);
            return false;
        }

        const int requested_wheel_events =
            attempt.value(QStringLiteral("requested_wheel_events")).toInt();
        const int moved_wheel_events =
            attempt.value(QStringLiteral("moved_wheel_events")).toInt();
        const int public_scroll_snapshot_events =
            attempt.value(QStringLiteral("public_scroll_snapshot_events")).toInt();
        const int snapshot_generation_delta =
            attempt.value(QStringLiteral("snapshot_generation_delta_on_wheel")).toInt();
        if (requested_wheel_events != options.wheel_events ||
            moved_wheel_events     != requested_wheel_events ||
            public_scroll_snapshot_events != moved_wheel_events ||
            snapshot_generation_delta     != public_scroll_snapshot_events)
        {
            *out_error = QStringLiteral(
                "benchmark attempt %1 wheel/snapshot/generation invariants changed")
                .arg(attempt_index);
            return false;
        }

        if (!attempt.value(QStringLiteral("projection_full_row_source")).toBool()) {
            *out_error = QStringLiteral("benchmark attempt %1 did not use full-row projection")
                .arg(attempt_index);
            return false;
        }

        const int safe_scrollback_rows =
            attempt.value(QStringLiteral("safe_scrollback_rows")).toInt();
        const int safe_visible_rows =
            attempt.value(QStringLiteral("safe_visible_rows")).toInt();
        const int projection_stored_rows =
            attempt.value(QStringLiteral("projection_stored_rows")).toInt();
        const int projection_copied_row_bound =
            attempt.value(QStringLiteral("projection_copied_row_bound")).toInt();
        const int projection_copied_cells =
            attempt.value(QStringLiteral("projection_copied_cells")).toInt();
        const int projection_row_capture_snapshots =
            attempt.value(QStringLiteral("projection_row_capture_snapshots")).toInt();
        const int copied_rows = safe_scrollback_rows + safe_visible_rows;
        if (safe_scrollback_rows          != options.scrollback_limit ||
            safe_visible_rows             != options.rows             ||
            copied_rows                   != expected_rows            ||
            projection_stored_rows        != copied_rows              ||
            projection_copied_row_bound   != copied_rows              ||
            projection_copied_cells       <= 0                        ||
            projection_copied_cells       >  expected_max_cells       ||
            projection_row_capture_snapshots <= 0                     ||
            projection_row_capture_snapshots > copied_rows)
        {
            *out_error = QStringLiteral("benchmark attempt %1 projection bounds changed")
                .arg(attempt_index);
            return false;
        }

        if (attempt.value(QStringLiteral("public_scroll_cursor_visible_snapshots")).toInt() <= 0) {
            *out_error = QStringLiteral("benchmark attempt %1 did not exercise visible cursor oracle")
                .arg(attempt_index);
            return false;
        }

        if (attempt.value(QStringLiteral("release_reconciliation_result")).toString() !=
            QString::fromLatin1(k_expected_release_reconciliation_result))
        {
            *out_error = QStringLiteral("benchmark attempt %1 release result changed")
                .arg(attempt_index);
            return false;
        }

        const int expected_release_offset = expected_release_offset_from_tail(options);
        if (attempt.value(QStringLiteral("release_snapshot_basis")).toString() !=
                QString::fromLatin1(k_expected_release_snapshot_basis) ||
            attempt.value(QStringLiteral("release_snapshot_purpose")).toString() !=
                QString::fromLatin1(k_expected_release_snapshot_purpose) ||
            attempt.value(QStringLiteral("expected_release_offset_from_tail")).toInt() !=
                expected_release_offset ||
            attempt.value(QStringLiteral("release_offset_from_tail")).toInt() !=
                expected_release_offset)
        {
            *out_error = QStringLiteral("benchmark attempt %1 release basis/offset changed")
                .arg(attempt_index);
            return false;
        }
    }

    const QJsonObject summary = root.value(QStringLiteral("summary")).toObject();
    if (summary.value(QStringLiteral("max_public_scroll_snapshot_events_per_attempt")).toInt() !=
            options.wheel_events ||
        summary.value(QStringLiteral("max_snapshot_generation_delta_per_attempt")).toInt() !=
            options.wheel_events ||
        summary.value(QStringLiteral("max_projection_stored_rows")).toInt() != expected_rows ||
        summary.value(QStringLiteral("max_projection_copied_row_bound")).toInt() != expected_rows ||
        summary.value(QStringLiteral("max_projection_copied_cells")).toInt() <= 0 ||
        summary.value(QStringLiteral("max_projection_copied_cells")).toInt() > expected_max_cells)
    {
        *out_error = QStringLiteral("benchmark summary invariants changed");
        return false;
    }

    return true;
}

bool write_output_file(const QString& path, const QByteArray& json, QString* out_error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        *out_error = QStringLiteral("could not open output file: %1").arg(path);
        return false;
    }

    if (file.write(json) != json.size()) {
        *out_error = QStringLiteral("could not write output file: %1").arg(path);
        return false;
    }

    if (!file.commit()) {
        *out_error = QStringLiteral("could not commit output file: %1").arg(path);
        return false;
    }

    return true;
}

int emit_json_and_status(const App_options& options, const QJsonObject& root, bool ok)
{
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (options.validate_json) {
        QString validation_error;
        if (!validate_json_output(json, options, &validation_error)) {
            std::cerr << "vnm_terminal_phase7_public_scroll_benchmark: "
                << validation_error.toUtf8().constData() << '\n';
            ok = false;
        }
    }

    if (!options.output_path.isEmpty()) {
        QString output_error;
        if (!write_output_file(options.output_path, json, &output_error)) {
            std::cerr << "vnm_terminal_phase7_public_scroll_benchmark: "
                << output_error.toUtf8().constData() << '\n';
            ok = false;
        }
    }

    if (!(options.quiet && !options.output_path.isEmpty())) {
        std::cout << json.constData();
    }
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    const QStringList arguments = raw_arguments(argc, argv);
    const Parse_result parse_result = parse_arguments(arguments);
    if (parse_result.options.help_requested) {
        print_usage();
        return 0;
    }

    if (!parse_result.error.isEmpty()) {
        std::cerr << "vnm_terminal_phase7_public_scroll_benchmark: "
            << parse_result.error.toUtf8().constData() << '\n';
        print_usage();
        return 2;
    }

    bool warmup_ok = true;
    for (int i = 0; i < parse_result.options.warmup; ++i) {
        const Attempt_result result = run_attempt(parse_result.options);
        warmup_ok = warmup_ok && result.ok;
    }

    std::vector<Attempt_result> attempts;
    attempts.reserve(static_cast<std::size_t>(parse_result.options.iterations));
    bool ok = warmup_ok;
    for (int i = 0; i < parse_result.options.iterations; ++i) {
        Attempt_result result = run_attempt(parse_result.options);
        ok = ok && result.ok;
        attempts.push_back(std::move(result));
    }

    const QJsonObject root = make_root_json(parse_result.options, attempts, warmup_ok, ok);
    return emit_json_and_status(parse_result.options, root, ok && warmup_ok);
}
