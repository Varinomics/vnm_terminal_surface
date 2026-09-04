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
constexpr int k_schema_version           = 3;
constexpr const char* k_benchmark_name = "vnm_terminal_search_incremental_benchmark";
constexpr const char* k_dense_query = ".";
constexpr const char* k_dense_edited_query = "..";

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

struct dense_result_probe_t
{
    qint64 query_submission_ns          = 0;
    qint64 query_completion_tail_ns     = 0;
    qint64 query_completion_total_ns    = 0;
    int    match_count                  = 0;
    qint64 search_next_ns               = 0;
    qint64 search_previous_ns           = 0;
    qint64 query_edit_submission_ns     = 0;
    qint64 query_edit_completion_tail_ns = 0;
    qint64 query_edit_completion_total_ns = 0;
    int    edited_match_count           = 0;
    qint64 clear_submission_ns          = 0;
    qint64 clear_disposal_tail_ns       = 0;
    qint64 clear_completion_total_ns    = 0;
};

struct Attempt_result
{
    bool          ok                                      = false;
    QString       error;
    qint64        no_query_seed_submission_ns             = 0;
    qint64        no_query_preseed_to_postseed_ns         = 0;
    int           seed_scrollback_rows                    = 0;
    std::uint64_t prefix_plain_ascii_rows                 = 0U;
    line_cost_t   no_query_publication_submission;
    qint64        no_query_publication_maintenance_catchup_ns = 0;
    qint64        no_query_publication_batch_completion_ns = 0;
    line_cost_t   active_query_publication_submission;
    qint64        active_query_publication_submission_overhead_ns_per_line = 0;
    qint64        query_submission_ns                     = 0;
    qint64        query_completion_ns                     = 0;
    qint64        active_search_catchup_ns                = 0;
    int           match_count                             = 0;
    int           viewport_offset_after_activation        = -1;
    qint64        reflow_widen_ns                         = 0;
    qint64        reflow_narrow_ns                        = 0;
    std::optional<dense_result_probe_t> dense_result;
    std::optional<qint64> rss_pre_seed_bytes;
    std::optional<qint64> rss_post_seed_bytes;
    std::optional<qint64> rss_post_no_query_publications_bytes;
    std::optional<qint64> rss_post_query_completion_bytes;
    std::optional<qint64> rss_post_active_query_publications_bytes;
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

    result.rss_pre_seed_bytes = process_resident_memory_bytes();
    const steady_clock_t::time_point seed_start = steady_clock_t::now();
    if (!backend->emit_output(make_seed_output(options))) {
        result.error = QStringLiteral("seed output was rejected");
        return result;
    }
    result.no_query_seed_submission_ns =
        elapsed_nanoseconds(seed_start, steady_clock_t::now());
    if (!session.wait_for_search_idle_for_testing(std::chrono::seconds(30))) {
        result.error = QStringLiteral("seed search-source maintenance did not complete");
        return result;
    }
    result.no_query_preseed_to_postseed_ns =
        elapsed_nanoseconds(seed_start, steady_clock_t::now());
    result.rss_post_seed_bytes = process_resident_memory_bytes();

    const std::optional<term::Terminal_render_snapshot> seeded =
        session.latest_render_snapshot();
    if (!seeded.has_value()) {
        result.error = QStringLiteral("seed published no snapshot");
        return result;
    }
    result.seed_scrollback_rows    = seeded->viewport.scrollback_rows;
    result.prefix_plain_ascii_rows =
        session.retained_history_diagnostics().payload_kind_prefix_plain_ascii_rows;

    int line_index_base = options.scrollback_limit + options.rows;
    const int last_control_line = line_index_base + options.published_lines - 1;
    line_index_base +=
        (options.match_stride - last_control_line % options.match_stride) %
        options.match_stride;
    const steady_clock_t::time_point no_query_publication_start =
        steady_clock_t::now();
    std::vector<qint64> baseline_samples =
        emit_line_burst(*backend, options, line_index_base);
    if (baseline_samples.empty()) {
        result.error = QStringLiteral("control leg output was rejected");
        return result;
    }
    result.no_query_publication_submission =
        line_cost_from_samples(std::move(baseline_samples));
    const steady_clock_t::time_point no_query_submission_end =
        steady_clock_t::now();
    if (!session.wait_for_search_idle_for_testing(std::chrono::seconds(30))) {
        result.error = QStringLiteral(
            "no-query search-source maintenance did not complete");
        return result;
    }
    result.no_query_publication_maintenance_catchup_ns =
        elapsed_nanoseconds(no_query_submission_end, steady_clock_t::now());
    result.no_query_publication_batch_completion_ns =
        elapsed_nanoseconds(no_query_publication_start, steady_clock_t::now());
    result.rss_post_no_query_publications_bytes = process_resident_memory_bytes();

    const steady_clock_t::time_point activation_start = steady_clock_t::now();
    session.set_search_query(options.query);
    result.query_submission_ns =
        elapsed_nanoseconds(activation_start, steady_clock_t::now());
    if (session.search_result_state().status !=
            term::Terminal_search_result_status::SEARCHING)
    {
        result.error = QStringLiteral("query submission did not report SEARCHING");
        return result;
    }
    if (!session.wait_for_search_completion_for_testing(std::chrono::seconds(30))) {
        result.error = QStringLiteral("query evaluation did not complete");
        return result;
    }
    result.query_completion_ns =
        elapsed_nanoseconds(activation_start, steady_clock_t::now());
    result.match_count = session.search_result_state().match_count;
    result.rss_post_query_completion_bytes = process_resident_memory_bytes();

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
    const steady_clock_t::time_point catchup_start = steady_clock_t::now();
    if (!session.wait_for_search_completion_for_testing(std::chrono::seconds(30))) {
        result.error = QStringLiteral("search leg evaluation did not complete");
        return result;
    }
    result.active_search_catchup_ns =
        elapsed_nanoseconds(catchup_start, steady_clock_t::now());
    result.active_query_publication_submission =
        line_cost_from_samples(std::move(active_samples));
    result.active_query_publication_submission_overhead_ns_per_line =
        result.active_query_publication_submission.median_ns -
        result.no_query_publication_submission.median_ns;
    result.rss_post_active_query_publications_bytes =
        process_resident_memory_bytes();

    const term::terminal_grid_size_t widened{options.rows, options.columns * 2};
    const term::terminal_grid_size_t narrowed{options.rows, options.columns};
    const steady_clock_t::time_point widen_start = steady_clock_t::now();
    (void)session.resize(
        QSizeF(static_cast<qreal>(widened.columns) * 8.0, static_cast<qreal>(widened.rows) * 16.0),
        widened);
    if (!session.wait_for_search_completion_for_testing(std::chrono::seconds(30))) {
        result.error = QStringLiteral("widened reflow search did not complete");
        return result;
    }
    result.reflow_widen_ns = elapsed_nanoseconds(widen_start, steady_clock_t::now());
    const steady_clock_t::time_point narrow_start = steady_clock_t::now();
    (void)session.resize(
        QSizeF(static_cast<qreal>(narrowed.columns) * 8.0, static_cast<qreal>(narrowed.rows) * 16.0),
        narrowed);
    if (!session.wait_for_search_completion_for_testing(std::chrono::seconds(30))) {
        result.error = QStringLiteral("narrowed reflow search did not complete");
        return result;
    }
    result.reflow_narrow_ns = elapsed_nanoseconds(narrow_start, steady_clock_t::now());

    if (session.search_query() == QString::fromLatin1(k_dense_query)) {
        session.clear_search();
        if (!session.wait_for_search_idle_for_testing(std::chrono::seconds(30))) {
            result.error = QStringLiteral("dense-query setup clear did not complete");
            return result;
        }
    }
    result.dense_result.emplace();
    dense_result_probe_t& dense = *result.dense_result;
    const steady_clock_t::time_point dense_query_start = steady_clock_t::now();
    session.set_search_query(QString::fromLatin1(k_dense_query));
    const steady_clock_t::time_point dense_query_submission_end =
        steady_clock_t::now();
    dense.query_submission_ns =
        elapsed_nanoseconds(dense_query_start, dense_query_submission_end);
    if (session.search_result_state().status !=
            term::Terminal_search_result_status::SEARCHING)
    {
        result.error = QStringLiteral("dense query submission did not report SEARCHING");
        return result;
    }
    if (!session.wait_for_search_completion_for_testing(std::chrono::seconds(30))) {
        result.error = QStringLiteral("dense query evaluation did not complete");
        return result;
    }
    const steady_clock_t::time_point dense_query_completion = steady_clock_t::now();
    dense.query_completion_tail_ns =
        elapsed_nanoseconds(dense_query_submission_end, dense_query_completion);
    dense.query_completion_total_ns =
        elapsed_nanoseconds(dense_query_start, dense_query_completion);
    dense.match_count = session.search_result_state().match_count;
    if (dense.match_count <= 0) {
        result.error = QStringLiteral("dense query produced no matches");
        return result;
    }

    const steady_clock_t::time_point search_next_start = steady_clock_t::now();
    if (!session.search_next()) {
        result.error = QStringLiteral("dense search-next navigation failed");
        return result;
    }
    dense.search_next_ns =
        elapsed_nanoseconds(search_next_start, steady_clock_t::now());

    const steady_clock_t::time_point search_previous_start = steady_clock_t::now();
    if (!session.search_previous()) {
        result.error = QStringLiteral("dense search-previous navigation failed");
        return result;
    }
    dense.search_previous_ns =
        elapsed_nanoseconds(search_previous_start, steady_clock_t::now());

    const steady_clock_t::time_point query_edit_start = steady_clock_t::now();
    session.set_search_query(QString::fromLatin1(k_dense_edited_query));
    const steady_clock_t::time_point query_edit_submission_end =
        steady_clock_t::now();
    dense.query_edit_submission_ns =
        elapsed_nanoseconds(query_edit_start, query_edit_submission_end);
    if (session.search_result_state().status !=
            term::Terminal_search_result_status::SEARCHING)
    {
        result.error = QStringLiteral("dense query edit did not report SEARCHING");
        return result;
    }
    if (!session.wait_for_search_completion_for_testing(std::chrono::seconds(30))) {
        result.error = QStringLiteral("dense query edit did not complete");
        return result;
    }
    const steady_clock_t::time_point query_edit_completion = steady_clock_t::now();
    dense.query_edit_completion_tail_ns =
        elapsed_nanoseconds(query_edit_submission_end, query_edit_completion);
    dense.query_edit_completion_total_ns =
        elapsed_nanoseconds(query_edit_start, query_edit_completion);
    dense.edited_match_count = session.search_result_state().match_count;
    if (dense.edited_match_count <= 0) {
        result.error = QStringLiteral("dense edited query produced no matches");
        return result;
    }

    const steady_clock_t::time_point dense_clear_start = steady_clock_t::now();
    session.clear_search();
    const steady_clock_t::time_point dense_clear_submission_end =
        steady_clock_t::now();
    dense.clear_submission_ns =
        elapsed_nanoseconds(dense_clear_start, dense_clear_submission_end);
    if (session.search_result_state().status !=
            term::Terminal_search_result_status::INACTIVE)
    {
        result.error = QStringLiteral("dense clear submission did not report INACTIVE");
        return result;
    }
    if (!session.wait_for_search_idle_for_testing(std::chrono::seconds(30))) {
        result.error = QStringLiteral("dense result disposal did not complete");
        return result;
    }
    const steady_clock_t::time_point dense_clear_completion = steady_clock_t::now();
    dense.clear_disposal_tail_ns =
        elapsed_nanoseconds(dense_clear_submission_end, dense_clear_completion);
    dense.clear_completion_total_ns =
        elapsed_nanoseconds(dense_clear_start, dense_clear_completion);
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

std::optional<qint64> optional_growth(
    const std::optional<qint64>& before,
    const std::optional<qint64>& after)
{
    return before.has_value() && after.has_value()
        ? std::optional<qint64>(*after - *before)
        : std::nullopt;
}

QJsonObject dense_result_probe_json(const dense_result_probe_t& probe)
{
    QJsonObject object;
    object.insert(QStringLiteral("query_submission_ns"), probe.query_submission_ns);
    object.insert(QStringLiteral("query_completion_tail_ns"),
        probe.query_completion_tail_ns);
    object.insert(QStringLiteral("query_completion_total_ns"),
        probe.query_completion_total_ns);
    object.insert(QStringLiteral("match_count"), probe.match_count);
    object.insert(QStringLiteral("search_next_ns"), probe.search_next_ns);
    object.insert(QStringLiteral("search_previous_ns"), probe.search_previous_ns);
    object.insert(QStringLiteral("query_edit_submission_ns"),
        probe.query_edit_submission_ns);
    object.insert(QStringLiteral("query_edit_completion_tail_ns"),
        probe.query_edit_completion_tail_ns);
    object.insert(QStringLiteral("query_edit_completion_total_ns"),
        probe.query_edit_completion_total_ns);
    object.insert(QStringLiteral("edited_match_count"), probe.edited_match_count);
    object.insert(QStringLiteral("clear_submission_ns"), probe.clear_submission_ns);
    object.insert(QStringLiteral("clear_disposal_tail_ns"),
        probe.clear_disposal_tail_ns);
    object.insert(QStringLiteral("clear_completion_total_ns"),
        probe.clear_completion_total_ns);
    return object;
}

QJsonObject attempt_json(const Attempt_result& attempt)
{
    QJsonObject object;
    object.insert(QStringLiteral("status"),
        attempt.ok ? QStringLiteral("ok") : QStringLiteral("failed"));
    if (!attempt.error.isEmpty()) {
        object.insert(QStringLiteral("error"), attempt.error);
    }
    object.insert(QStringLiteral("no_query_seed_submission_ns"),
        attempt.no_query_seed_submission_ns);
    object.insert(QStringLiteral("no_query_preseed_to_postseed_ns"),
        attempt.no_query_preseed_to_postseed_ns);
    object.insert(QStringLiteral("seed_scrollback_rows"), attempt.seed_scrollback_rows);
    object.insert(QStringLiteral("prefix_plain_ascii_rows"),
        static_cast<qint64>(attempt.prefix_plain_ascii_rows));
    object.insert(QStringLiteral("no_query_publication_submission_ns_per_line"),
        attempt.no_query_publication_submission.median_ns);
    object.insert(QStringLiteral("no_query_publication_submission_iqr_ns"),
        attempt.no_query_publication_submission.iqr_ns);
    object.insert(QStringLiteral("no_query_publication_maintenance_catchup_ns"),
        attempt.no_query_publication_maintenance_catchup_ns);
    object.insert(QStringLiteral("no_query_publication_batch_completion_ns"),
        attempt.no_query_publication_batch_completion_ns);
    object.insert(QStringLiteral("active_query_publication_submission_ns_per_line"),
        attempt.active_query_publication_submission.median_ns);
    object.insert(QStringLiteral("active_query_publication_submission_iqr_ns"),
        attempt.active_query_publication_submission.iqr_ns);
    object.insert(
        QStringLiteral(
            "active_query_publication_submission_overhead_ns_per_line"),
        attempt.active_query_publication_submission_overhead_ns_per_line);
    object.insert(QStringLiteral("query_submission_ns"), attempt.query_submission_ns);
    object.insert(QStringLiteral("query_completion_ns"), attempt.query_completion_ns);
    object.insert(QStringLiteral("active_search_catchup_ns"),
        attempt.active_search_catchup_ns);
    object.insert(QStringLiteral("match_count"), attempt.match_count);
    object.insert(QStringLiteral("viewport_offset_after_activation"),
        attempt.viewport_offset_after_activation);
    object.insert(QStringLiteral("reflow_widen_ns"), attempt.reflow_widen_ns);
    object.insert(QStringLiteral("reflow_narrow_ns"), attempt.reflow_narrow_ns);
    if (attempt.dense_result.has_value()) {
        object.insert(
            QStringLiteral("dense_result"),
            dense_result_probe_json(*attempt.dense_result));
    }
    insert_optional_bytes(object, QStringLiteral("rss_pre_seed_bytes"),
        attempt.rss_pre_seed_bytes);
    insert_optional_bytes(object, QStringLiteral("rss_post_seed_bytes"),
        attempt.rss_post_seed_bytes);
    insert_optional_bytes(object, QStringLiteral("rss_post_no_query_publications_bytes"),
        attempt.rss_post_no_query_publications_bytes);
    insert_optional_bytes(object, QStringLiteral("rss_post_query_completion_bytes"),
        attempt.rss_post_query_completion_bytes);
    insert_optional_bytes(
        object,
        QStringLiteral("rss_post_active_query_publications_bytes"),
        attempt.rss_post_active_query_publications_bytes);
    insert_optional_bytes(
        object,
        QStringLiteral("rss_preseed_to_postseed_growth_bytes"),
        optional_growth(attempt.rss_pre_seed_bytes, attempt.rss_post_seed_bytes));
    insert_optional_bytes(
        object,
        QStringLiteral("rss_preseed_to_post_no_query_publications_growth_bytes"),
        optional_growth(
            attempt.rss_pre_seed_bytes,
            attempt.rss_post_no_query_publications_bytes));
    insert_optional_bytes(
        object,
        QStringLiteral("rss_prequery_to_postcompletion_growth_bytes"),
        optional_growth(
            attempt.rss_post_no_query_publications_bytes,
            attempt.rss_post_query_completion_bytes));
    insert_optional_bytes(
        object,
        QStringLiteral("rss_active_query_publications_growth_bytes"),
        optional_growth(
            attempt.rss_post_query_completion_bytes,
            attempt.rss_post_active_query_publications_bytes));
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
    std::vector<qint64> seed;
    std::vector<qint64> seed_submission;
    std::vector<qint64> no_query_publication_submission;
    std::vector<qint64> no_query_publication_maintenance_catchup;
    std::vector<qint64> no_query_publication_batch_completion;
    std::vector<qint64> active_query_publication_submission;
    std::vector<qint64> active_query_publication_submission_overhead;
    std::vector<qint64> submission;
    std::vector<qint64> completion;
    std::vector<qint64> catchup;
    std::vector<qint64> dense_query_submission;
    std::vector<qint64> dense_query_completion_tail;
    std::vector<qint64> dense_query_completion_total;
    std::vector<qint64> dense_search_next;
    std::vector<qint64> dense_search_previous;
    std::vector<qint64> dense_query_edit_submission;
    std::vector<qint64> dense_query_edit_completion_tail;
    std::vector<qint64> dense_query_edit_completion_total;
    std::vector<qint64> dense_clear_submission;
    std::vector<qint64> dense_clear_disposal_tail;
    std::vector<qint64> dense_clear_completion_total;
    for (const Attempt_result& attempt : attempts) {
        seed.push_back(attempt.no_query_preseed_to_postseed_ns);
        seed_submission.push_back(attempt.no_query_seed_submission_ns);
        no_query_publication_submission.push_back(
            attempt.no_query_publication_submission.median_ns);
        no_query_publication_maintenance_catchup.push_back(
            attempt.no_query_publication_maintenance_catchup_ns);
        no_query_publication_batch_completion.push_back(
            attempt.no_query_publication_batch_completion_ns);
        active_query_publication_submission.push_back(
            attempt.active_query_publication_submission.median_ns);
        active_query_publication_submission_overhead.push_back(
            attempt.active_query_publication_submission_overhead_ns_per_line);
        submission.push_back(attempt.query_submission_ns);
        completion.push_back(attempt.query_completion_ns);
        catchup.push_back(attempt.active_search_catchup_ns);
        if (attempt.dense_result.has_value()) {
            const dense_result_probe_t& dense = *attempt.dense_result;
            dense_query_submission.push_back(dense.query_submission_ns);
            dense_query_completion_tail.push_back(dense.query_completion_tail_ns);
            dense_query_completion_total.push_back(dense.query_completion_total_ns);
            dense_search_next.push_back(dense.search_next_ns);
            dense_search_previous.push_back(dense.search_previous_ns);
            dense_query_edit_submission.push_back(dense.query_edit_submission_ns);
            dense_query_edit_completion_tail.push_back(
                dense.query_edit_completion_tail_ns);
            dense_query_edit_completion_total.push_back(
                dense.query_edit_completion_total_ns);
            dense_clear_submission.push_back(dense.clear_submission_ns);
            dense_clear_disposal_tail.push_back(dense.clear_disposal_tail_ns);
            dense_clear_completion_total.push_back(dense.clear_completion_total_ns);
        }
    }

    QJsonObject summary;
    summary.insert(QStringLiteral("median_no_query_seed_submission_ns"),
        median_of(seed_submission));
    summary.insert(QStringLiteral("median_no_query_preseed_to_postseed_ns"),
        median_of(seed));
    summary.insert(
        QStringLiteral("median_no_query_publication_submission_ns_per_line"),
        median_of(no_query_publication_submission));
    summary.insert(
        QStringLiteral("median_no_query_publication_maintenance_catchup_ns"),
        median_of(no_query_publication_maintenance_catchup));
    summary.insert(
        QStringLiteral("median_no_query_publication_batch_completion_ns"),
        median_of(no_query_publication_batch_completion));
    summary.insert(
        QStringLiteral("median_active_query_publication_submission_ns_per_line"),
        median_of(active_query_publication_submission));
    summary.insert(
        QStringLiteral(
            "median_active_query_publication_submission_overhead_ns_per_line"),
        median_of(active_query_publication_submission_overhead));
    summary.insert(QStringLiteral("median_query_submission_ns"), median_of(submission));
    summary.insert(QStringLiteral("median_query_completion_ns"), median_of(completion));
    summary.insert(QStringLiteral("median_active_search_catchup_ns"), median_of(catchup));
    if (!dense_query_submission.empty()) {
        QJsonObject dense;
        dense.insert(QStringLiteral("median_query_submission_ns"),
            median_of(dense_query_submission));
        dense.insert(QStringLiteral("median_query_completion_tail_ns"),
            median_of(dense_query_completion_tail));
        dense.insert(QStringLiteral("median_query_completion_total_ns"),
            median_of(dense_query_completion_total));
        dense.insert(QStringLiteral("median_search_next_ns"),
            median_of(dense_search_next));
        dense.insert(QStringLiteral("median_search_previous_ns"),
            median_of(dense_search_previous));
        dense.insert(QStringLiteral("median_query_edit_submission_ns"),
            median_of(dense_query_edit_submission));
        dense.insert(QStringLiteral("median_query_edit_completion_tail_ns"),
            median_of(dense_query_edit_completion_tail));
        dense.insert(QStringLiteral("median_query_edit_completion_total_ns"),
            median_of(dense_query_edit_completion_total));
        dense.insert(QStringLiteral("median_clear_submission_ns"),
            median_of(dense_clear_submission));
        dense.insert(QStringLiteral("median_clear_disposal_tail_ns"),
            median_of(dense_clear_disposal_tail));
        dense.insert(QStringLiteral("median_clear_completion_total_ns"),
            median_of(dense_clear_completion_total));
        summary.insert(QStringLiteral("dense_result"), dense);
    }
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
    QJsonObject comparison_contract;
    comparison_contract.insert(
        QStringLiteral("pairing_option_fields"),
        QJsonArray{
            QStringLiteral("rows"),
            QStringLiteral("columns"),
            QStringLiteral("scrollback_limit"),
            QStringLiteral("published_lines"),
            QStringLiteral("match_stride"),
            QStringLiteral("query"),
        });
    comparison_contract.insert(
        QStringLiteral("revision_comparison_fields"),
        QJsonArray{
            QStringLiteral("no_query_seed_submission_ns"),
            QStringLiteral("no_query_preseed_to_postseed_ns"),
            QStringLiteral("no_query_publication_submission_ns_per_line"),
            QStringLiteral("no_query_publication_maintenance_catchup_ns"),
            QStringLiteral("no_query_publication_batch_completion_ns"),
            QStringLiteral("rss_post_seed_bytes"),
            QStringLiteral("rss_preseed_to_postseed_growth_bytes"),
            QStringLiteral(
                "rss_preseed_to_post_no_query_publications_growth_bytes"),
            QStringLiteral("query_submission_ns"),
            QStringLiteral("query_completion_ns"),
            QStringLiteral("rss_post_query_completion_bytes"),
            QStringLiteral("rss_prequery_to_postcompletion_growth_bytes"),
        });
    comparison_contract.insert(
        QStringLiteral("prior_schema_1_field_mapping"),
        QJsonObject{
            {QStringLiteral("no_query_seed_submission_ns"),
                QStringLiteral("seed_ns")},
            {QStringLiteral("no_query_publication_submission_ns_per_line"),
                QStringLiteral("baseline_ns_per_line")},
            {QStringLiteral("rss_post_seed_bytes"),
                QStringLiteral("rss_before_query_bytes")},
            {QStringLiteral("query_completion_ns"),
                QStringLiteral("query_activation_ns")},
            {QStringLiteral("rss_post_query_completion_bytes"),
                QStringLiteral("rss_after_index_bytes")},
        });
    root.insert(QStringLiteral("comparison_contract"), comparison_contract);
    root.insert(
        QStringLiteral("capability_measurements"),
        QJsonObject{
            {
                QStringLiteral("dense_result"),
                QJsonObject{
                    {QStringLiteral("attempt_field"), QStringLiteral("dense_result")},
                    {QStringLiteral("query"), QString::fromLatin1(k_dense_query)},
                    {QStringLiteral("edited_query"),
                        QString::fromLatin1(k_dense_edited_query)},
                },
            },
        });
    root.insert(
        QStringLiteral("measurement_semantics"),
        QStringLiteral(
            "no_query_seed_submission_ns covers synchronous ingest and publication of the "
            "whole retained history while no query is set; "
            "no_query_preseed_to_postseed_ns additionally waits until asynchronous "
            "search-source maintenance is idle; no_query_publication_submission_ns_per_line "
            "measures synchronous no-query publication one line at a time at the retention "
            "target, while maintenance_catchup and batch_completion report the asynchronous "
            "tail and total wall time for the whole leg; active "
            "query publication submission excludes asynchronous catchup, which is reported "
            "separately; query submission times only set_search_query while query completion "
            "also waits for the accepted asynchronous result; the optional dense_result "
            "capability record uses a fixed one-byte query that repeatedly matches seeded "
            "padding, measures next and previous navigation, then edits to a two-byte query; "
            "every dense-result completion_total_ns starts before the corresponding public "
            "call, while completion_tail_ns or disposal_tail_ns starts after that call "
            "returns; clear_submission_ns times only clear_search and its bounded overlay "
            "publication, while clear_completion_total_ns additionally waits until "
            "worker-side result disposal is idle; "
            "RSS checkpoints are absolute "
            "best-effort process working sets and every growth field names its exact before "
            "and after checkpoints, so prequery-to-postcompletion growth is not total search "
            "memory; pair reports from two revisions only when every pairing_option_field "
            "matches and compare the named revision_comparison_fields; the schema-1 map "
            "identifies equivalent historical checkpoints, not aliases for total memory"));
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

    const QJsonObject comparison_contract =
        root.value(QStringLiteral("comparison_contract")).toObject();
    if (comparison_contract.value(QStringLiteral("pairing_option_fields"))
            .toArray().isEmpty() ||
        comparison_contract.value(QStringLiteral("revision_comparison_fields"))
            .toArray().isEmpty() ||
        comparison_contract.value(QStringLiteral("prior_schema_1_field_mapping"))
            .toObject().isEmpty())
    {
        *out_error = QStringLiteral("benchmark comparison contract is missing");
        return false;
    }
    const QJsonObject dense_capability =
        root.value(QStringLiteral("capability_measurements"))
            .toObject()
            .value(QStringLiteral("dense_result"))
            .toObject();
    if (dense_capability.value(QStringLiteral("attempt_field")).toString() !=
            QStringLiteral("dense_result") ||
        dense_capability.value(QStringLiteral("query")).toString() !=
            QString::fromLatin1(k_dense_query) ||
        dense_capability.value(QStringLiteral("edited_query")).toString() !=
            QString::fromLatin1(k_dense_edited_query) ||
        root.value(QStringLiteral("summary"))
            .toObject()
            .value(QStringLiteral("dense_result"))
            .toObject()
            .isEmpty())
    {
        *out_error = QStringLiteral("dense-result capability metadata is missing");
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

        if (attempt.value(QStringLiteral("no_query_seed_submission_ns"))
                    .toInteger() <= 0 ||
            attempt.value(QStringLiteral("no_query_preseed_to_postseed_ns"))
                    .toInteger() <= 0 ||
            attempt.value(QStringLiteral(
                "no_query_publication_submission_ns_per_line"))
                    .toInteger() <= 0 ||
            attempt.value(QStringLiteral(
                "no_query_publication_maintenance_catchup_ns"))
                    .toInteger() < 0 ||
            attempt.value(QStringLiteral(
                "no_query_publication_batch_completion_ns"))
                    .toInteger() <= 0 ||
            attempt.value(QStringLiteral(
                "active_query_publication_submission_ns_per_line"))
                    .toInteger() <= 0 ||
            attempt.value(QStringLiteral("query_submission_ns")).toInteger() <= 0 ||
            attempt.value(QStringLiteral("query_completion_ns")).toInteger() <= 0)
        {
            *out_error = QStringLiteral("benchmark attempt %1 recorded an incomplete timing")
                .arg(index);
            return false;
        }
        if (attempt.value(QStringLiteral("no_query_preseed_to_postseed_ns"))
                    .toInteger() <
                attempt.value(QStringLiteral("no_query_seed_submission_ns"))
                    .toInteger() ||
            attempt.value(QStringLiteral(
                "no_query_publication_batch_completion_ns"))
                    .toInteger() <
                attempt.value(QStringLiteral(
                    "no_query_publication_maintenance_catchup_ns"))
                    .toInteger() ||
            attempt.value(QStringLiteral("query_completion_ns")).toInteger() <
                attempt.value(QStringLiteral("query_submission_ns")).toInteger())
        {
            *out_error = QStringLiteral(
                "benchmark attempt %1 recorded inconsistent async timing boundaries")
                .arg(index);
            return false;
        }

        const QJsonObject dense =
            attempt.value(QStringLiteral("dense_result")).toObject();
        if (dense.isEmpty() ||
            dense.value(QStringLiteral("match_count")).toInt() <= 0 ||
            dense.value(QStringLiteral("edited_match_count")).toInt() <= 0 ||
            dense.value(QStringLiteral("query_submission_ns")).toInteger() <= 0 ||
            dense.value(QStringLiteral("query_completion_tail_ns")).toInteger() < 0 ||
            dense.value(QStringLiteral("query_completion_total_ns")).toInteger() <= 0 ||
            dense.value(QStringLiteral("search_next_ns")).toInteger() <= 0 ||
            dense.value(QStringLiteral("search_previous_ns")).toInteger() <= 0 ||
            dense.value(QStringLiteral("query_edit_submission_ns")).toInteger() <= 0 ||
            dense.value(QStringLiteral("query_edit_completion_tail_ns")).toInteger() < 0 ||
            dense.value(QStringLiteral("query_edit_completion_total_ns")).toInteger() <= 0 ||
            dense.value(QStringLiteral("clear_submission_ns")).toInteger() <= 0 ||
            dense.value(QStringLiteral("clear_disposal_tail_ns")).toInteger() < 0 ||
            dense.value(QStringLiteral("clear_completion_total_ns")).toInteger() <= 0)
        {
            *out_error = QStringLiteral(
                "benchmark attempt %1 recorded an incomplete dense-result probe")
                .arg(index);
            return false;
        }
        if (dense.value(QStringLiteral("query_completion_total_ns")).toInteger() <
                dense.value(QStringLiteral("query_submission_ns")).toInteger() ||
            dense.value(QStringLiteral("query_completion_total_ns")).toInteger() <
                dense.value(QStringLiteral("query_completion_tail_ns")).toInteger() ||
            dense.value(QStringLiteral("query_edit_completion_total_ns")).toInteger() <
                dense.value(QStringLiteral("query_edit_submission_ns")).toInteger() ||
            dense.value(QStringLiteral("query_edit_completion_total_ns")).toInteger() <
                dense.value(QStringLiteral("query_edit_completion_tail_ns")).toInteger() ||
            dense.value(QStringLiteral("clear_completion_total_ns")).toInteger() <
                dense.value(QStringLiteral("clear_submission_ns")).toInteger() ||
            dense.value(QStringLiteral("clear_completion_total_ns")).toInteger() <
                dense.value(QStringLiteral("clear_disposal_tail_ns")).toInteger())
        {
            *out_error = QStringLiteral(
                "benchmark attempt %1 recorded inconsistent dense-result timing boundaries")
                .arg(index);
            return false;
        }

        const auto rss_growth_is_consistent = [&attempt](
            const QString& before_key,
            const QString& after_key,
            const QString& growth_key) {
            const QJsonValue before = attempt.value(before_key);
            const QJsonValue after  = attempt.value(after_key);
            const QJsonValue growth = attempt.value(growth_key);
            if (before.isNull() || after.isNull()) {
                return growth.isNull();
            }
            return before.isDouble() && after.isDouble() && growth.isDouble() &&
                growth.toInteger() == after.toInteger() - before.toInteger();
        };
        if (!rss_growth_is_consistent(
                QStringLiteral("rss_pre_seed_bytes"),
                QStringLiteral("rss_post_seed_bytes"),
                QStringLiteral("rss_preseed_to_postseed_growth_bytes")) ||
            !rss_growth_is_consistent(
                QStringLiteral("rss_pre_seed_bytes"),
                QStringLiteral("rss_post_no_query_publications_bytes"),
                QStringLiteral(
                    "rss_preseed_to_post_no_query_publications_growth_bytes")) ||
            !rss_growth_is_consistent(
                QStringLiteral("rss_post_no_query_publications_bytes"),
                QStringLiteral("rss_post_query_completion_bytes"),
                QStringLiteral("rss_prequery_to_postcompletion_growth_bytes")) ||
            !rss_growth_is_consistent(
                QStringLiteral("rss_post_query_completion_bytes"),
                QStringLiteral("rss_post_active_query_publications_bytes"),
                QStringLiteral("rss_active_query_publications_growth_bytes")))
        {
            *out_error = QStringLiteral(
                "benchmark attempt %1 recorded inconsistent RSS checkpoints")
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
