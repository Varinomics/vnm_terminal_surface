#include "vnm_terminal/internal/terminal_session.h"

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSizeF>
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
#include <cstdint>
#include <iostream>
#include <memory>
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
constexpr int k_default_published_lines  = 200;
constexpr int k_default_match_stride     = 97;
constexpr int k_schema_version           = 1;
constexpr const char* k_benchmark_name = "vnm_terminal_search_incremental_benchmark";

struct App_options
{
    int     iterations       = k_default_iterations;
    int     warmup           = k_default_warmup;
    int     rows             = k_default_rows;
    int     columns          = k_default_columns;
    int     scrollback_limit = k_default_scrollback_limit;
    int     published_lines  = k_default_published_lines;
    int     match_stride     = k_default_match_stride;
    QString query            = QStringLiteral("needle");
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

// Per-line cost is a median plus an interquartile range so a noisy host cannot
// be promoted into a positive claim.
struct line_cost_t
{
    qint64 median_ns = 0;
    qint64 iqr_ns    = 0;
};

struct Attempt_result
{
    bool          ok                      = false;
    QString       error;
    qint64        seed_ns                 = 0;
    int           seed_scrollback_rows    = 0;
    std::uint64_t prefix_plain_ascii_rows = 0U;
    line_cost_t   baseline;
    line_cost_t   active;
    qint64        search_overhead_ns_per_line      = 0;
    qint64        query_activation_ns              = 0;
    int           match_count                      = 0;
    int           viewport_offset_after_activation = -1;
    qint64        reflow_widen_ns                  = 0;
    qint64        reflow_narrow_ns                 = 0;
    std::optional<qint64> rss_before_query_bytes;
    std::optional<qint64> rss_after_index_bytes;
    std::optional<qint64> rss_after_lines_bytes;
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

    term::Terminal_backend_result write(QByteArray) override
    {
        return m_running
            ? term::backend_accept()
            : term::backend_reject(
                term::Terminal_backend_error_code::WRITE_FAILED,
                QStringLiteral("benchmark backend is not running"));
    }

    term::Terminal_backend_result resize(
        term::Terminal_backend_resize_request request) override
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
    bool                             m_running       = false;
    bool                             m_output_paused = false;
};

using steady_clock_t = std::chrono::steady_clock;

qint64 elapsed_nanoseconds(
    steady_clock_t::time_point start,
    steady_clock_t::time_point end)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
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

    bool   ok             = false;
    qint64 resident_pages = fields[1].trimmed().toLongLong(&ok);
    if (!ok) {
        return std::nullopt;
    }

    const long page_size = sysconf(_SC_PAGESIZE);
    return page_size > 0
        ? std::optional<qint64>(resident_pages * static_cast<qint64>(page_size))
        : std::nullopt;
#else
    return std::nullopt;
#endif
}

line_cost_t line_cost_from_samples(std::vector<qint64> samples)
{
    if (samples.empty()) {
        return {};
    }

    std::sort(samples.begin(), samples.end());
    const std::size_t count = samples.size();
    const auto quantile = [&](double fraction) {
        const std::size_t index = std::min(
            count - 1U,
            static_cast<std::size_t>(fraction * static_cast<double>(count)));
        return samples[index];
    };
    return {quantile(0.5), quantile(0.75) - quantile(0.25)};
}

QByteArray seed_line(const App_options& options, int line_index)
{
    QString line = QStringLiteral("row-%1 ").arg(line_index, 9, 10, QLatin1Char('0'));
    if (line_index % options.match_stride == 0) {
        line += options.query;
        line += QLatin1Char(' ');
    }

    const int payload_columns = std::max(1, options.columns - 1);
    while (line.size() < payload_columns) {
        line += QLatin1Char('.');
    }
    line.truncate(payload_columns);
    line += QStringLiteral("\r\n");
    return line.toUtf8();
}

// Line index zero is emitted last so the newest visible row carries a match.
// The initial match selection then lands inside the visible grid and both
// measured legs run with the viewport at the tail.
QByteArray make_seed_output(const App_options& options)
{
    QByteArray output;
    for (int index = options.scrollback_limit + options.rows - 1; index >= 0; --index) {
        output += seed_line(options, index);
    }
    return output;
}

term::Terminal_launch_config launch_config_for_options(const App_options& options)
{
    term::Terminal_launch_config config;
    config.argv              = {QStringLiteral("search-incremental-benchmark-fixture")};
    config.working_directory = QStringLiteral("C:/workspace");
    config.initial_grid_size = term::terminal_grid_size_t{options.rows, options.columns};
    return config;
}

term::Terminal_session_config session_config_for_options(const App_options& options)
{
    term::Terminal_session_config config;
    config.output_queue_limits.high_water_bytes = 8U * 1024U * 1024U;
    config.output_queue_limits.hard_limit_bytes = 256U * 1024U * 1024U;
    config.scrollback_limit                     = options.scrollback_limit;
    config.trace_notification_limit             = 64U;
    return config;
}

std::vector<qint64> emit_line_burst(
    Benchmark_backend& backend,
    const App_options& options,
    int                first_line_index)
{
    std::vector<qint64> samples;
    samples.reserve(static_cast<std::size_t>(options.published_lines));
    for (int index = 0; index < options.published_lines; ++index) {
        const QByteArray bytes = seed_line(options, first_line_index + index);
        const steady_clock_t::time_point start = steady_clock_t::now();
        const bool emitted = backend.emit_output(bytes);
        samples.push_back(elapsed_nanoseconds(start, steady_clock_t::now()));
        if (!emitted) {
            return {};
        }
    }
    return samples;
}

Attempt_result run_attempt(const App_options& options)
{
    Attempt_result result;
    auto backend_owner = std::make_unique<Benchmark_backend>();
    Benchmark_backend* backend = backend_owner.get();
    term::Terminal_session session(
        std::move(backend_owner),
        session_config_for_options(options));
    if (session.start(launch_config_for_options(options)).code !=
        term::Terminal_session_result_code::ACCEPTED)
    {
        result.error = QStringLiteral("session did not start");
        return result;
    }

    const steady_clock_t::time_point seed_start = steady_clock_t::now();
    if (!backend->emit_output(make_seed_output(options))) {
        result.error = QStringLiteral("seed output was rejected");
        return result;
    }
    result.seed_ns = elapsed_nanoseconds(seed_start, steady_clock_t::now());

    const std::optional<term::Terminal_render_snapshot> seeded =
        session.latest_render_snapshot();
    if (!seeded.has_value()) {
        result.error = QStringLiteral("seed published no snapshot");
        return result;
    }
    result.seed_scrollback_rows    = seeded->viewport.scrollback_rows;
    result.prefix_plain_ascii_rows =
        session.retained_history_diagnostics().payload_kind_prefix_plain_ascii_rows;
    result.rss_before_query_bytes  = process_resident_memory_bytes();

    const int line_index_base = options.scrollback_limit + options.rows;
    std::vector<qint64> baseline_samples =
        emit_line_burst(*backend, options, line_index_base);
    if (baseline_samples.empty()) {
        result.error = QStringLiteral("control leg output was rejected");
        return result;
    }
    result.baseline = line_cost_from_samples(std::move(baseline_samples));

    const steady_clock_t::time_point activation_start = steady_clock_t::now();
    session.set_search_query(options.query);
    result.query_activation_ns =
        elapsed_nanoseconds(activation_start, steady_clock_t::now());
    result.match_count           = session.search_result_state().match_count;
    result.rss_after_index_bytes = process_resident_memory_bytes();

    const std::optional<term::Terminal_render_snapshot> activated =
        session.latest_render_snapshot();
    if (activated.has_value()) {
        result.viewport_offset_after_activation = activated->viewport.offset_from_tail;
    }

    std::vector<qint64> active_samples = emit_line_burst(
        *backend,
        options,
        line_index_base + options.published_lines);
    if (active_samples.empty()) {
        result.error = QStringLiteral("search leg output was rejected");
        return result;
    }
    result.active = line_cost_from_samples(std::move(active_samples));
    result.search_overhead_ns_per_line =
        result.active.median_ns - result.baseline.median_ns;
    result.rss_after_lines_bytes = process_resident_memory_bytes();

    const term::terminal_grid_size_t widened{options.rows, options.columns * 2};
    const term::terminal_grid_size_t narrowed{options.rows, options.columns};
    const steady_clock_t::time_point widen_start = steady_clock_t::now();
    (void)session.resize(
        QSizeF(static_cast<qreal>(widened.columns) * 8.0, static_cast<qreal>(widened.rows) * 16.0),
        widened);
    result.reflow_widen_ns = elapsed_nanoseconds(widen_start, steady_clock_t::now());
    const steady_clock_t::time_point narrow_start = steady_clock_t::now();
    (void)session.resize(
        QSizeF(static_cast<qreal>(narrowed.columns) * 8.0, static_cast<qreal>(narrowed.rows) * 16.0),
        narrowed);
    result.reflow_narrow_ns = elapsed_nanoseconds(narrow_start, steady_clock_t::now());

    session.clear_search();
    result.ok = true;
    return result;
}

QStringList raw_arguments(int argc, char** argv)
{
    QStringList arguments;
    for (int i = 0; i < argc; ++i) {
        arguments.push_back(QString::fromLocal8Bit(argv[i]));
    }
    return arguments;
}

void print_usage()
{
    std::cout
        << "usage: vnm_terminal_search_incremental_benchmark [options]\n"
        << "  --iterations <n>          measured attempts, default 7\n"
        << "  --warmup <n>              unreported warmup attempts, default 2\n"
        << "  --scrollback-limit <n>    retained rows to seed, default 10000\n"
        << "  --rows <n>                grid rows, default 24\n"
        << "  --columns <n>             grid columns, default 80\n"
        << "  --published-lines <n>     lines emitted per measured leg, default 200\n"
        << "  --match-stride <n>        seeded lines between matches, default 97\n"
        << "  --query <text>            literal search query, default needle\n"
        << "  --output <path>           write JSON report\n"
        << "  --validate-json           validate emitted JSON shape\n"
        << "  --quiet                   suppress stdout when --output is used\n";
}

bool matches_option(const QString& argument, const QString& option_name)
{
    return argument == option_name ||
        argument.startsWith(option_name + QLatin1Char('='));
}

bool option_value(
    const QStringList& arguments,
    int*               index,
    const QString&     option_name,
    QString*           out_value,
    QString*           out_error)
{
    if (arguments[*index] != option_name) {
        *out_value = arguments[*index].mid(option_name.size() + 1);
        return true;
    }

    if (*index + 1 >= arguments.size()) {
        *out_error = QStringLiteral("%1 requires a value").arg(option_name);
        return false;
    }

    ++(*index);
    *out_value = arguments[*index];
    return true;
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

    bool      parsed = false;
    const int number = value.toInt(&parsed);
    if (!parsed || number < minimum || number > maximum) {
        *out_error = QStringLiteral("%1 requires an integer in [%2, %3]")
            .arg(option_name).arg(minimum).arg(maximum);
        return false;
    }

    *target = number;
    return true;
}

Parse_result parse_arguments(const QStringList& arguments)
{
    Parse_result result;
    result.options.command_line = arguments.join(QLatin1Char(' '));

    const struct
    {
        QString name;
        int     minimum;
        int     maximum;
        int*    target;
    } integer_options[] = {
        {QStringLiteral("--iterations"),       1, 1000,    &result.options.iterations},
        {QStringLiteral("--warmup"),           0, 1000,    &result.options.warmup},
        {QStringLiteral("--scrollback-limit"), 1, 1000000, &result.options.scrollback_limit},
        {QStringLiteral("--rows"),             1, 500,     &result.options.rows},
        {QStringLiteral("--columns"),          8, 1000,    &result.options.columns},
        {QStringLiteral("--published-lines"),  1, 100000,  &result.options.published_lines},
        {QStringLiteral("--match-stride"),     1, 1000000, &result.options.match_stride},
    };

    for (int i = 1; i < arguments.size(); ++i) {
        const QString argument = arguments[i];
        if (argument == QStringLiteral("--help")) {
            result.options.help_requested = true;
            continue;
        }
        if (argument == QStringLiteral("--validate-json")) {
            result.options.validate_json = true;
            continue;
        }
        if (argument == QStringLiteral("--quiet")) {
            result.options.quiet = true;
            continue;
        }

        bool handled = false;
        for (const auto& option : integer_options) {
            if (!matches_option(argument, option.name)) {
                continue;
            }
            if (!parse_int_option(
                    arguments, &i, option.name,
                    option.minimum, option.maximum, option.target, &result.error))
            {
                return result;
            }
            handled = true;
            break;
        }
        if (handled) {
            continue;
        }

        if (matches_option(argument, QStringLiteral("--query"))) {
            if (!option_value(
                    arguments, &i, QStringLiteral("--query"),
                    &result.options.query, &result.error))
            {
                return result;
            }
            if (result.options.query.isEmpty()) {
                result.error = QStringLiteral("--query requires a non-empty value");
                return result;
            }
        }
        else
        if (matches_option(argument, QStringLiteral("--output"))) {
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

void insert_optional_bytes(
    QJsonObject&                 object,
    const QString&               key,
    const std::optional<qint64>& value)
{
    object.insert(key, value.has_value() ? QJsonValue(*value) : QJsonValue());
}

QJsonObject attempt_json(const Attempt_result& attempt)
{
    QJsonObject object;
    object.insert(QStringLiteral("status"),
        attempt.ok ? QStringLiteral("ok") : QStringLiteral("failed"));
    if (!attempt.error.isEmpty()) {
        object.insert(QStringLiteral("error"), attempt.error);
    }
    object.insert(QStringLiteral("seed_ns"), attempt.seed_ns);
    object.insert(QStringLiteral("seed_scrollback_rows"), attempt.seed_scrollback_rows);
    object.insert(QStringLiteral("prefix_plain_ascii_rows"),
        static_cast<qint64>(attempt.prefix_plain_ascii_rows));
    object.insert(QStringLiteral("baseline_ns_per_line"), attempt.baseline.median_ns);
    object.insert(QStringLiteral("baseline_iqr_ns"), attempt.baseline.iqr_ns);
    object.insert(QStringLiteral("active_ns_per_line"), attempt.active.median_ns);
    object.insert(QStringLiteral("active_iqr_ns"), attempt.active.iqr_ns);
    object.insert(QStringLiteral("search_overhead_ns_per_line"),
        attempt.search_overhead_ns_per_line);
    object.insert(QStringLiteral("query_activation_ns"), attempt.query_activation_ns);
    object.insert(QStringLiteral("match_count"), attempt.match_count);
    object.insert(QStringLiteral("viewport_offset_after_activation"),
        attempt.viewport_offset_after_activation);
    object.insert(QStringLiteral("reflow_widen_ns"), attempt.reflow_widen_ns);
    object.insert(QStringLiteral("reflow_narrow_ns"), attempt.reflow_narrow_ns);
    insert_optional_bytes(object, QStringLiteral("rss_before_query_bytes"),
        attempt.rss_before_query_bytes);
    insert_optional_bytes(object, QStringLiteral("rss_after_index_bytes"),
        attempt.rss_after_index_bytes);
    insert_optional_bytes(object, QStringLiteral("rss_after_lines_bytes"),
        attempt.rss_after_lines_bytes);
    return object;
}

qint64 median_of(std::vector<qint64> values)
{
    if (values.empty()) {
        return 0;
    }

    std::sort(values.begin(), values.end());
    return values[values.size() / 2U];
}

QJsonObject make_summary_json(const std::vector<Attempt_result>& attempts)
{
    std::vector<qint64> baseline;
    std::vector<qint64> active;
    std::vector<qint64> overhead;
    std::vector<qint64> activation;
    for (const Attempt_result& attempt : attempts) {
        baseline.push_back(attempt.baseline.median_ns);
        active.push_back(attempt.active.median_ns);
        overhead.push_back(attempt.search_overhead_ns_per_line);
        activation.push_back(attempt.query_activation_ns);
    }

    QJsonObject summary;
    summary.insert(QStringLiteral("median_baseline_ns_per_line"), median_of(baseline));
    summary.insert(QStringLiteral("median_active_ns_per_line"), median_of(active));
    summary.insert(QStringLiteral("median_search_overhead_ns_per_line"),
        median_of(overhead));
    summary.insert(QStringLiteral("median_query_activation_ns"), median_of(activation));
    return summary;
}

QJsonObject make_options_json(const App_options& options)
{
    QJsonObject object;
    object.insert(QStringLiteral("iterations"), options.iterations);
    object.insert(QStringLiteral("warmup"), options.warmup);
    object.insert(QStringLiteral("rows"), options.rows);
    object.insert(QStringLiteral("columns"), options.columns);
    object.insert(QStringLiteral("scrollback_limit"), options.scrollback_limit);
    object.insert(QStringLiteral("published_lines"), options.published_lines);
    object.insert(QStringLiteral("match_stride"), options.match_stride);
    object.insert(QStringLiteral("query"), options.query);
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
    root.insert(QStringLiteral("benchmark"), QString::fromLatin1(k_benchmark_name));
    root.insert(QStringLiteral("status"),
        ok && warmup_ok ? QStringLiteral("ok") : QStringLiteral("failed"));
#if VNM_TERMINAL_PROFILING_ENABLED
    root.insert(QStringLiteral("profiling_compiled"), true);
#else
    root.insert(QStringLiteral("profiling_compiled"), false);
#endif
    root.insert(QStringLiteral("options"), make_options_json(options));
    root.insert(
        QStringLiteral("measurement_semantics"),
        QStringLiteral(
            "seed timing covers ingest of the whole retained history with no query set; "
            "the control leg emits single lines with no query and the search leg emits the "
            "same lines with the query active, both at the retention target and with the "
            "viewport at the tail; per-line cost is a median with an interquartile range "
            "over the leg; activation times one set_search_query on the seeded session; "
            "memory is best-effort process RSS"));
    root.insert(QStringLiteral("warmup_status"),
        warmup_ok ? QStringLiteral("ok") : QStringLiteral("failed"));
    root.insert(QStringLiteral("attempts"), attempt_array);
    root.insert(QStringLiteral("summary"), make_summary_json(attempts));
    return root;
}

bool validate_json_output(
    const QByteArray&  json,
    const App_options& options,
    QString*           out_error)
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
            QString::fromLatin1(k_benchmark_name) ||
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

    for (int index = 0; index < attempts.size(); ++index) {
        const QJsonObject attempt = attempts[index].toObject();
        if (attempt.value(QStringLiteral("status")).toString() != QStringLiteral("ok")) {
            *out_error = QStringLiteral("benchmark attempt %1 failed").arg(index);
            return false;
        }

        if (attempt.value(QStringLiteral("seed_scrollback_rows")).toInt() !=
            options.scrollback_limit)
        {
            *out_error = QStringLiteral(
                "benchmark attempt %1 did not reach the requested retention target")
                .arg(index);
            return false;
        }

        if (attempt.value(QStringLiteral("match_count")).toInt() <= 0 ||
            attempt.value(QStringLiteral("viewport_offset_after_activation")).toInt() != 0)
        {
            *out_error = QStringLiteral(
                "benchmark attempt %1 did not measure an active query at the tail")
                .arg(index);
            return false;
        }

        if (attempt.value(QStringLiteral("baseline_ns_per_line")).toInteger() <= 0 ||
            attempt.value(QStringLiteral("active_ns_per_line")).toInteger() <= 0)
        {
            *out_error = QStringLiteral("benchmark attempt %1 recorded no per-line cost")
                .arg(index);
            return false;
        }
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

    if (file.write(json) != json.size() || !file.commit()) {
        *out_error = QStringLiteral("could not write output file: %1").arg(path);
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
            std::cerr << k_benchmark_name << ": "
                << validation_error.toUtf8().constData() << '\n';
            ok = false;
        }
    }

    if (!options.output_path.isEmpty()) {
        QString output_error;
        if (!write_output_file(options.output_path, json, &output_error)) {
            std::cerr << k_benchmark_name << ": "
                << output_error.toUtf8().constData() << '\n';
            ok = false;
        }
    }

    if (!(options.quiet && !options.output_path.isEmpty())) {
        std::cout << json.constData();
    }
    return ok ? 0 : 1;
}

}

int main(int argc, char** argv)
{
    const QStringList arguments = raw_arguments(argc, argv);
    const Parse_result parse_result = parse_arguments(arguments);
    if (parse_result.options.help_requested) {
        print_usage();
        return 0;
    }

    if (!parse_result.error.isEmpty()) {
        std::cerr << k_benchmark_name << ": "
            << parse_result.error.toUtf8().constData() << '\n';
        print_usage();
        return 2;
    }

    bool warmup_ok = true;
    for (int i = 0; i < parse_result.options.warmup; ++i) {
        warmup_ok = run_attempt(parse_result.options).ok && warmup_ok;
    }

    std::vector<Attempt_result> attempts;
    attempts.reserve(static_cast<std::size_t>(parse_result.options.iterations));
    bool ok = warmup_ok;
    for (int i = 0; i < parse_result.options.iterations; ++i) {
        Attempt_result result = run_attempt(parse_result.options);
        ok = ok && result.ok;
        attempts.push_back(std::move(result));
    }

    const QJsonObject root =
        make_root_json(parse_result.options, attempts, warmup_ok, ok);
    return emit_json_and_status(parse_result.options, root, ok && warmup_ok);
}
