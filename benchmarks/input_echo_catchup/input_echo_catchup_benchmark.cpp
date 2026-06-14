#include "vnm_terminal/internal/backend_contract.h"
#include "vnm_terminal/internal/render_snapshot.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/vnm_terminal_surface.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMetaObject>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSaveFile>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QtGlobal>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

constexpr int k_schema_version       = 1;
constexpr int k_default_iterations   = 20;
constexpr int k_default_warmup       = 3;
constexpr int k_default_polish_delay = 2000;
constexpr int k_default_backlog      = 2;
constexpr int k_max_iterations       = 1000;
constexpr int k_max_warmup           = 100;
constexpr int k_max_delay_us         = 1000000;
constexpr int k_max_backlog          = 64;
constexpr int k_backlog_slice_bytes  = 4096;
constexpr int k_prompt_cursor_column = 2;
constexpr int k_expected_echo_column = k_prompt_cursor_column + 1;

const QString k_schema_name = QStringLiteral("vnm_terminal_input_echo_catchup_benchmark");
const QString k_queue_contract_schema_name =
    QStringLiteral("vnm_terminal_input_echo_queue_contract_benchmark");
const QString k_measurement_boundary = QStringLiteral(
    "surface_key_event_to_updatePolish_snapshot_before_scene_graph_capture");
const QString k_queue_contract_measurement_boundary = QStringLiteral(
    "backend_callback_enqueue_before_qsg_sync_capture");
const QString k_queue_contract_decision_criteria = QStringLiteral(
    "first captured post-input frame must not consume a snapshot whose "
    "backend_callback_epoch is older than a backend callback delivered before sync");
const QByteArray k_prompt_payload = QByteArrayLiteral("\x1b[2J\x1b[H> ");
const QByteArray k_echo_payload = QByteArrayLiteral("x");
const QString k_echo_visible_text = QStringLiteral("> x");

using steady_clock = std::chrono::steady_clock;

struct sample_summary_t
{
    int                    sample_count = 0;
    qint64                 total        = 0;
    qint64                 min          = 0;
    qint64                 median       = 0;
    qint64                 p95          = 0;
    qint64                 max          = 0;
};

struct Backend_drain_delta
{
    std::uint64_t          total_drain_calls                 = 0U;
    std::uint64_t          budgeted_drain_calls              = 0U;
    std::uint64_t          budget_exhausted_incomplete       = 0U;
    std::uint64_t          pending_callback_after_drain      = 0U;
    std::uint64_t          requeue_count                     = 0U;
    std::uint64_t          total_elapsed_ns                  = 0U;
    std::uint64_t          session_processing_elapsed_ns     = 0U;
    std::uint64_t          sync_from_session_elapsed_ns      = 0U;
    std::uint64_t          frame_work_pending_drain_calls    = 0U;
    std::uint64_t          render_update_pending_drain_calls = 0U;
};

struct Sample_result
{
    int                    sample_index                  = 0;
    int                    catchup_budget_us             = 0;
    int                    echo_delay_us                 = 0;
    bool                   input_accepted                = false;
    bool                   echo_enqueued_before_polish   = false;
    bool                   echo_visible_at_polish        = false;
    bool                   stale_frame_at_polish         = false;
    bool                   cursor_stale_at_polish        = false;
    bool                   callbacks_queued_after_polish = false;
    int                    cursor_column_at_polish       = -1;
    int                    expected_cursor_column        = k_expected_echo_column;
    int                    cursor_stale_cells            = 0;
    std::uint64_t          snapshot_sequence_before      = 0U;
    std::uint64_t          snapshot_sequence_after       = 0U;
    qint64                 actual_echo_delay_ns          = -1;
    qint64                 input_event_elapsed_ns        = 0;
    qint64                 input_to_polish_done_ns       = 0;
    qint64                 polish_elapsed_ns             = 0;
    Backend_drain_delta    drain_delta;
};

struct Case_result
{
    int                         catchup_budget_us = 0;
    int                         echo_delay_us     = 0;
    std::vector<Sample_result>  samples;
};

struct Benchmark_options
{
    int                    iterations       = k_default_iterations;
    int                    warmup           = k_default_warmup;
    int                    polish_delay_us  = k_default_polish_delay;
    int                    pre_echo_backlog = k_default_backlog;
    std::vector<int>       catchup_budgets_us{0, 250, 1000, 4000};
    std::vector<int>       echo_delay_us{0, 500, 1500, 3000, 6000};
    QString                output_path;
    bool                   quiet            = false;
    bool                   validate_json    = false;
    bool                   queue_contract   = false;
    bool                   help             = false;
};

struct Queue_contract_result
{
    bool                   input_accepted                         = false;
    bool                   echo_injected_before_sync              = false;
    bool                   stale_capture_observed                 = false;
    bool                   echo_snapshot_published                = false;
    bool                   passed                                 = false;
    std::uint64_t          callback_enqueue_epoch_before_echo     = 0U;
    std::uint64_t          callback_enqueue_epoch_after_echo      = 0U;
    std::uint64_t          callback_processed_epoch_after_capture = 0U;
    std::uint64_t          pre_echo_snapshot_sequence             = 0U;
    std::uint64_t          pre_echo_snapshot_callback_epoch       = 0U;
    std::uint64_t          polished_snapshot_sequence             = 0U;
    std::uint64_t          polished_snapshot_callback_epoch       = 0U;
    std::uint64_t          first_capture_count                    = 0U;
    std::uint64_t          first_captured_snapshot_sequence       = 0U;
    std::uint64_t          echo_snapshot_sequence                 = 0U;
    std::uint64_t          echo_snapshot_callback_epoch           = 0U;
    std::uint64_t          backend_callback_frame_deferrals_before = 0U;
    std::uint64_t          backend_callback_frame_deferrals_after  = 0U;
};

class Scripted_backend final : public term::Terminal_backend
{
public:
    term::Terminal_backend_result start(
        const term::Terminal_launch_config&    config,
        term::Terminal_backend_callbacks       callbacks) override
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
        m_running = true;
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
        if (!term::is_valid_grid_size(request.grid_size)) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::RESIZE_FAILED,
                    QStringLiteral("scripted resize requires a positive grid"));
        }

        m_resize_requests.push_back(request);
        return term::backend_accept();
    }

    term::Terminal_backend_result set_output_paused(bool paused) override
    {
        m_output_pause_requests.push_back(paused);
        return term::backend_accept();
    }

    term::Terminal_backend_result interrupt() override
    {
        if (!m_running) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::INTERRUPT_FAILED,
                    QStringLiteral("scripted interrupt without process"));
        }

        m_running = false;
        m_callbacks.process_exited({term::Terminal_exit_reason::INTERRUPTED, 130});
        return term::backend_accept();
    }

    term::Terminal_backend_result terminate() override
    {
        if (!m_running) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::TERMINATE_FAILED,
                    QStringLiteral("scripted terminate without process"));
        }

        m_running = false;
        m_callbacks.process_exited({term::Terminal_exit_reason::TERMINATED, 0});
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

private:
    bool                                                 m_running = false;
    std::vector<QByteArray>                              m_writes;
    std::vector<term::Terminal_backend_resize_request>   m_resize_requests;
    std::vector<bool>                                    m_output_pause_requests;
    term::Terminal_backend_callbacks                     m_callbacks;
};

struct Surface_fixture
{
    QQuickWindow           window;
    VNM_TerminalSurface    surface;

    Surface_fixture()
    {
        window.resize(640, 320);
        surface.setParentItem(window.contentItem());
        surface.setSize(QSizeF(520.0, 240.0));
        surface.set_font_family(QStringLiteral("monospace"));
        surface.set_font_size(12.0);
        window.show();
        surface.forceActiveFocus();
    }
};

qint64 elapsed_ns_count(steady_clock::duration duration)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

qint64 bounded_json_i64(std::uint64_t value)
{
    constexpr std::uint64_t k_max =
        static_cast<std::uint64_t>(std::numeric_limits<qint64>::max());
    return value > k_max
        ? std::numeric_limits<qint64>::max()
        : static_cast<qint64>(value);
}

void insert_i64(QJsonObject& object, const QString& key, qint64 value)
{
    object.insert(key, QJsonValue(value));
}

void insert_u64(QJsonObject& object, const QString& key, std::uint64_t value)
{
    object.insert(key, QJsonValue(bounded_json_i64(value)));
}

void pump_events(QGuiApplication& app, int rounds = 4)
{
    for (int i = 0; i < rounds; ++i) {
        app.processEvents(QEventLoop::AllEvents, 1);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

void wait_until_time(steady_clock::time_point target)
{
    for (;;) {
        const steady_clock::time_point now = steady_clock::now();
        if (now >= target) {
            return;
        }

        const steady_clock::duration remaining = target - now;
        if (remaining > std::chrono::microseconds{500}) {
            std::this_thread::sleep_for(std::chrono::microseconds{250});
        }
        else {
            QThread::yieldCurrentThread();
        }
    }
}

QString snapshot_row_text(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row)
{
    const term::Terminal_render_snapshot_row_content_view rows(snapshot);
    return rows.row_text(row, 0, snapshot.grid_size.columns, true);
}

bool snapshot_contains_text(
    const term::Terminal_render_snapshot&  snapshot,
    const QString&                         text)
{
    for (int row = 0; row < snapshot.grid_size.rows; ++row) {
        if (snapshot_row_text(snapshot, row).contains(text)) {
            return true;
        }
    }

    return false;
}

bool parse_int(
    const QString&  text,
    int             min_value,
    int             max_value,
    int*            out_value,
    QString*        out_error)
{
    bool ok = false;
    const int value = text.toInt(&ok);
    if (!ok || value < min_value || value > max_value) {
        *out_error = QStringLiteral("invalid integer '%1'").arg(text);
        return false;
    }

    *out_value = value;
    return true;
}

bool parse_int_list(
    const QString&      text,
    int                 min_value,
    int                 max_value,
    std::vector<int>*   out_values,
    QString*            out_error)
{
    std::vector<int> values;
    const QStringList parts = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (parts.empty()) {
        *out_error = QStringLiteral("expected a comma-separated integer list");
        return false;
    }

    values.reserve(static_cast<std::size_t>(parts.size()));
    for (const QString& part : parts) {
        int value = 0;
        if (!parse_int(part.trimmed(), min_value, max_value, &value, out_error)) {
            return false;
        }
        values.push_back(value);
    }

    *out_values = std::move(values);
    return true;
}

bool list_contains(const std::vector<int>& values, int needle)
{
    return std::find(values.begin(), values.end(), needle) != values.end();
}

bool parse_options(
    const QStringList&      args,
    Benchmark_options*      options,
    QString*                out_error)
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
        if (arg == QStringLiteral("--iterations")) {
            const std::optional<QString> value = require_value();
            if (!value.has_value() ||
                !parse_int(*value, 1, k_max_iterations, &options->iterations, out_error))
            {
                return false;
            }
        }
        else
        if (arg == QStringLiteral("--warmup")) {
            const std::optional<QString> value = require_value();
            if (!value.has_value() ||
                !parse_int(*value, 0, k_max_warmup, &options->warmup, out_error))
            {
                return false;
            }
        }
        else
        if (arg == QStringLiteral("--catchup-budget-us")) {
            const std::optional<QString> value = require_value();
            if (!value.has_value() ||
                !parse_int_list(
                    *value,
                    0,
                    k_max_delay_us,
                    &options->catchup_budgets_us,
                    out_error))
            {
                return false;
            }
        }
        else
        if (arg == QStringLiteral("--echo-delay-us")) {
            const std::optional<QString> value = require_value();
            if (!value.has_value() ||
                !parse_int_list(
                    *value,
                    0,
                    k_max_delay_us,
                    &options->echo_delay_us,
                    out_error))
            {
                return false;
            }
        }
        else
        if (arg == QStringLiteral("--polish-delay-us")) {
            const std::optional<QString> value = require_value();
            if (!value.has_value() ||
                !parse_int(
                    *value,
                    0,
                    k_max_delay_us,
                    &options->polish_delay_us,
                    out_error))
            {
                return false;
            }
        }
        else
        if (arg == QStringLiteral("--pre-echo-backlog")) {
            const std::optional<QString> value = require_value();
            if (!value.has_value() ||
                !parse_int(*value, 0, k_max_backlog, &options->pre_echo_backlog, out_error))
            {
                return false;
            }
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
        if (arg == QStringLiteral("--queue-contract")) {
            options->queue_contract = true;
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

    if (!options->queue_contract && !list_contains(options->catchup_budgets_us, 0)) {
        *out_error = QStringLiteral("--catchup-budget-us must include 0 for the A/B baseline");
        return false;
    }

    return true;
}

void print_usage()
{
    std::cout
        << "Usage: vnm_terminal_input_echo_catchup_benchmark [options]\n"
        << "  --iterations N             measured attempts per budget/delay case\n"
        << "  --warmup N                 unrecorded attempts per case\n"
        << "  --catchup-budget-us LIST   comma-separated budgets, must include 0\n"
        << "  --echo-delay-us LIST       comma-separated echo delay buckets\n"
        << "  --polish-delay-us N        simulated input-to-polish frame window\n"
        << "  --pre-echo-backlog N       4096-byte cursor-neutral backlog slices before echo\n"
        << "  --output PATH              write JSON to PATH instead of stdout\n"
        << "  --quiet                    suppress non-JSON status output\n"
        << "  --queue-contract           run deterministic queue-frontier capture contract\n"
        << "  --validate-json            validate output schema and case coverage\n";
}

std::uint64_t delta_u64(std::uint64_t before, std::uint64_t after)
{
    return after >= before ? after - before : 0U;
}

Backend_drain_delta drain_delta(
    const term::Terminal_surface_backend_drain_stats_t& before,
    const term::Terminal_surface_backend_drain_stats_t& after)
{
    Backend_drain_delta delta;
    delta.total_drain_calls =
        delta_u64(before.total_drain_calls, after.total_drain_calls);
    delta.budgeted_drain_calls =
        delta_u64(before.budgeted_drain_calls, after.budgeted_drain_calls);
    delta.budget_exhausted_incomplete =
        delta_u64(before.budget_exhausted_incomplete, after.budget_exhausted_incomplete);
    delta.pending_callback_after_drain =
        delta_u64(before.pending_callback_after_drain, after.pending_callback_after_drain);
    delta.requeue_count =
        delta_u64(before.requeue_count, after.requeue_count);
    delta.total_elapsed_ns =
        delta_u64(before.total_elapsed_ns, after.total_elapsed_ns);
    delta.session_processing_elapsed_ns =
        delta_u64(before.session_processing_elapsed_ns, after.session_processing_elapsed_ns);
    delta.sync_from_session_elapsed_ns =
        delta_u64(before.sync_from_session_elapsed_ns, after.sync_from_session_elapsed_ns);
    delta.frame_work_pending_drain_calls =
        delta_u64(before.frame_work_pending_drain_calls, after.frame_work_pending_drain_calls);
    delta.render_update_pending_drain_calls =
        delta_u64(
            before.render_update_pending_drain_calls,
            after.render_update_pending_drain_calls);
    return delta;
}

void add_drain_delta(Backend_drain_delta* total, const Backend_drain_delta& delta)
{
    total->total_drain_calls                 += delta.total_drain_calls;
    total->budgeted_drain_calls              += delta.budgeted_drain_calls;
    total->budget_exhausted_incomplete       += delta.budget_exhausted_incomplete;
    total->pending_callback_after_drain      += delta.pending_callback_after_drain;
    total->requeue_count                     += delta.requeue_count;
    total->total_elapsed_ns                  += delta.total_elapsed_ns;
    total->session_processing_elapsed_ns     += delta.session_processing_elapsed_ns;
    total->sync_from_session_elapsed_ns      += delta.sync_from_session_elapsed_ns;
    total->frame_work_pending_drain_calls    += delta.frame_work_pending_drain_calls;
    total->render_update_pending_drain_calls += delta.render_update_pending_drain_calls;
}

sample_summary_t summarize_samples(std::vector<qint64> values)
{
    sample_summary_t summary;
    summary.sample_count = static_cast<int>(values.size());
    if (values.empty()) {
        return summary;
    }

    std::sort(values.begin(), values.end());
    for (qint64 value : values) {
        summary.total += value;
    }

    const std::size_t median_index = values.size() / 2U;
    const std::size_t p95_index =
        std::min<std::size_t>(
            values.size() - 1U,
            (values.size() * 95U + 99U) / 100U - 1U);
    summary.min    = values.front();
    summary.median = values[median_index];
    summary.p95    = values[p95_index];
    summary.max    = values.back();
    return summary;
}

QJsonObject summary_json(const sample_summary_t& summary)
{
    QJsonObject object;
    object.insert(QStringLiteral("sample_count"), summary.sample_count);
    insert_i64(object, QStringLiteral("total"), summary.total);
    insert_i64(object, QStringLiteral("min"), summary.min);
    insert_i64(object, QStringLiteral("median"), summary.median);
    insert_i64(object, QStringLiteral("p95"), summary.p95);
    insert_i64(object, QStringLiteral("max"), summary.max);
    return object;
}

QJsonObject drain_delta_json(const Backend_drain_delta& delta)
{
    QJsonObject object;
    insert_u64(object, QStringLiteral("total_drain_calls"), delta.total_drain_calls);
    insert_u64(object, QStringLiteral("budgeted_drain_calls"), delta.budgeted_drain_calls);
    insert_u64(
        object,
        QStringLiteral("budget_exhausted_incomplete"),
        delta.budget_exhausted_incomplete);
    insert_u64(
        object,
        QStringLiteral("pending_callback_after_drain"),
        delta.pending_callback_after_drain);
    insert_u64(object, QStringLiteral("requeue_count"), delta.requeue_count);
    insert_u64(object, QStringLiteral("total_elapsed_ns"), delta.total_elapsed_ns);
    insert_u64(
        object,
        QStringLiteral("session_processing_elapsed_ns"),
        delta.session_processing_elapsed_ns);
    insert_u64(
        object,
        QStringLiteral("sync_from_session_elapsed_ns"),
        delta.sync_from_session_elapsed_ns);
    insert_u64(
        object,
        QStringLiteral("frame_work_pending_drain_calls"),
        delta.frame_work_pending_drain_calls);
    insert_u64(
        object,
        QStringLiteral("render_update_pending_drain_calls"),
        delta.render_update_pending_drain_calls);
    return object;
}

QJsonObject sample_json(const Sample_result& sample)
{
    QJsonObject object;
    object.insert(QStringLiteral("sample_index"), sample.sample_index);
    object.insert(QStringLiteral("catchup_budget_us"), sample.catchup_budget_us);
    object.insert(QStringLiteral("echo_delay_us"), sample.echo_delay_us);
    object.insert(QStringLiteral("input_accepted"), sample.input_accepted);
    object.insert(
        QStringLiteral("echo_enqueued_before_polish"),
        sample.echo_enqueued_before_polish);
    object.insert(QStringLiteral("echo_visible_at_polish"), sample.echo_visible_at_polish);
    object.insert(QStringLiteral("stale_frame_at_polish"), sample.stale_frame_at_polish);
    object.insert(QStringLiteral("cursor_stale_at_polish"), sample.cursor_stale_at_polish);
    object.insert(
        QStringLiteral("callbacks_queued_after_polish"),
        sample.callbacks_queued_after_polish);
    object.insert(QStringLiteral("cursor_column_at_polish"), sample.cursor_column_at_polish);
    object.insert(QStringLiteral("expected_cursor_column"), sample.expected_cursor_column);
    object.insert(QStringLiteral("cursor_stale_cells"), sample.cursor_stale_cells);
    insert_u64(object, QStringLiteral("snapshot_sequence_before"), sample.snapshot_sequence_before);
    insert_u64(object, QStringLiteral("snapshot_sequence_after"), sample.snapshot_sequence_after);
    insert_i64(object, QStringLiteral("actual_echo_delay_ns"), sample.actual_echo_delay_ns);
    insert_i64(object, QStringLiteral("input_event_elapsed_ns"), sample.input_event_elapsed_ns);
    insert_i64(object, QStringLiteral("input_to_polish_done_ns"), sample.input_to_polish_done_ns);
    insert_i64(object, QStringLiteral("polish_elapsed_ns"), sample.polish_elapsed_ns);
    object.insert(QStringLiteral("backend_drain_delta"), drain_delta_json(sample.drain_delta));
    return object;
}

QJsonArray int_array_json(const std::vector<int>& values)
{
    QJsonArray array;
    for (int value : values) {
        array.append(value);
    }
    return array;
}

bool start_surface(
    VNM_TerminalSurface&   surface,
    Scripted_backend**     out_backend,
    QString*               out_error)
{
    auto backend = std::make_unique<Scripted_backend>();
    Scripted_backend* backend_ptr = backend.get();
    const bool started = term::VNM_TerminalSurface_render_bridge::start_process_with_backend(
        surface,
        std::move(backend),
        {QStringLiteral("scripted-terminal")});
    if (!started) {
        *out_error = QStringLiteral("failed to start scripted terminal surface");
        return false;
    }

    *out_backend = backend_ptr;
    return true;
}

bool mark_snapshot_rendered_for_next_attempt(
    VNM_TerminalSurface&   surface,
    QString*               out_error)
{
    if (!term::VNM_TerminalSurface_render_bridge::
        mark_completed_atlas_completion_pending_for_testing(surface))
    {
        *out_error = QStringLiteral("cannot mark a missing render snapshot as consumed");
        return false;
    }

    (void)term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    return true;
}

bool prepare_attempt(
    QGuiApplication&       app,
    VNM_TerminalSurface&   surface,
    Scripted_backend&      backend,
    int                    attempt_index,
    QString*               out_error)
{
    if (!backend.emit_output(k_prompt_payload)) {
        *out_error = QStringLiteral("scripted backend rejected prompt output");
        return false;
    }

    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(surface);
    pump_events(app, 2);

    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(surface);
    if (snapshot == nullptr || !snapshot_contains_text(*snapshot, QStringLiteral(">"))) {
        *out_error = QStringLiteral("prompt setup did not publish a render snapshot");
        return false;
    }

    if (!mark_snapshot_rendered_for_next_attempt(surface, out_error)) {
        return false;
    }

    term::VNM_TerminalSurface_render_bridge::set_cursor_blink_visible(
        surface,
        (attempt_index % 2) == 0);
    const term::Terminal_surface_render_invalidation_stats_t pending_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    if (!pending_stats.pending_update) {
        *out_error = QStringLiteral("attempt setup did not leave a frame update pending");
        return false;
    }

    return true;
}

bool enqueue_backlog_and_echo(
    Scripted_backend&      backend,
    int                    backlog_count,
    QString*               out_error)
{
    QByteArray output;
    const int target_backlog_bytes = backlog_count * k_backlog_slice_bytes;
    output.reserve(target_backlog_bytes + k_echo_payload.size());
    output.append(QByteArray(target_backlog_bytes, '\0'));
    output += k_echo_payload;

    if (!backend.emit_output(std::move(output))) {
        *out_error = QStringLiteral("scripted backend rejected echo output");
        return false;
    }

    return true;
}

bool send_input_key(VNM_TerminalSurface& surface)
{
    QKeyEvent event(
        QEvent::KeyPress,
        Qt::Key_X,
        Qt::NoModifier,
        QStringLiteral("x"));
    QCoreApplication::sendEvent(&surface, &event);
    return event.isAccepted();
}

std::optional<term::Qsg_atlas_frame_report> capture_next_surface_frame(
    QGuiApplication&       app,
    QQuickWindow&          window,
    VNM_TerminalSurface&   surface,
    std::uint64_t          previous_capture_count)
{
    for (int attempt = 0; attempt < 30; ++attempt) {
        surface.update();
        window.requestUpdate();
        pump_events(app, 1);
        const QImage image = window.grabWindow();
        const term::Qsg_atlas_frame_report report =
            term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
        const term::terminal_renderer_stats_t renderer_stats =
            term::VNM_TerminalSurface_render_bridge::last_renderer_stats(surface);
        if (!image.isNull() &&
            report.capture_count > previous_capture_count &&
            renderer_stats.text_content_failures == 0)
        {
            return report;
        }
    }

    return std::nullopt;
}

std::shared_ptr<const term::Terminal_render_snapshot> wait_for_echo_snapshot(
    QGuiApplication&       app,
    VNM_TerminalSurface&   surface)
{
    for (int attempt = 0; attempt < 30; ++attempt) {
        pump_events(app, 1);
        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            term::VNM_TerminalSurface_render_bridge::render_snapshot(surface);
        if (snapshot != nullptr && snapshot_contains_text(*snapshot, k_echo_visible_text)) {
            return snapshot;
        }
    }

    return {};
}

std::optional<Queue_contract_result> run_queue_contract(
    QGuiApplication&       app,
    QString*               out_error)
{
    Surface_fixture fixture;
    pump_events(app);

    Scripted_backend* backend = nullptr;
    if (!start_surface(fixture.surface, &backend, out_error) || backend == nullptr) {
        return std::nullopt;
    }

    Queue_contract_result result;
    if (!prepare_attempt(app, fixture.surface, *backend, 0, out_error)) {
        return std::nullopt;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> pre_echo_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    if (pre_echo_snapshot == nullptr ||
        snapshot_contains_text(*pre_echo_snapshot, k_echo_visible_text))
    {
        *out_error = QStringLiteral("queue contract setup did not leave a pre-echo snapshot");
        return std::nullopt;
    }
    result.pre_echo_snapshot_sequence = pre_echo_snapshot->metadata.sequence;
    result.pre_echo_snapshot_callback_epoch =
        pre_echo_snapshot->metadata.backend_callback_epoch;

    const term::Terminal_surface_render_invalidation_stats_t invalidation_before =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    result.backend_callback_frame_deferrals_before =
        invalidation_before.backend_callback_frame_deferrals;
    if (!invalidation_before.pending_update) {
        *out_error = QStringLiteral("queue contract setup did not leave pending frame work");
        return std::nullopt;
    }

    result.input_accepted = send_input_key(fixture.surface);
    term::VNM_TerminalSurface_render_bridge::simulate_update_polish(fixture.surface);

    const std::shared_ptr<const term::Terminal_render_snapshot> polished_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(fixture.surface);
    if (polished_snapshot != nullptr) {
        result.polished_snapshot_sequence = polished_snapshot->metadata.sequence;
        result.polished_snapshot_callback_epoch =
            polished_snapshot->metadata.backend_callback_epoch;
    }

    const term::Qsg_atlas_frame_report report_before =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(fixture.surface);
    std::atomic<bool> echo_injected{false};
    std::atomic<std::uint64_t> enqueue_epoch_before{0U};
    std::atomic<std::uint64_t> enqueue_epoch_after{0U};
    const QMetaObject::Connection echo_connection = QObject::connect(
        &fixture.window,
        &QQuickWindow::beforeSynchronizing,
        &fixture.window,
        [&] {
            if (!echo_injected.exchange(true)) {
                enqueue_epoch_before.store(
                    term::VNM_TerminalSurface_render_bridge::backend_callback_enqueue_epoch(
                        fixture.surface),
                    std::memory_order_release);
                (void)backend->emit_output(k_echo_payload);
                enqueue_epoch_after.store(
                    term::VNM_TerminalSurface_render_bridge::backend_callback_enqueue_epoch(
                        fixture.surface),
                    std::memory_order_release);
            }
        },
        Qt::DirectConnection);

    const std::optional<term::Qsg_atlas_frame_report> first_frame_report =
        capture_next_surface_frame(
            app,
            fixture.window,
            fixture.surface,
            report_before.capture_count);
    QObject::disconnect(echo_connection);

    result.echo_injected_before_sync = echo_injected.load(std::memory_order_acquire);
    result.callback_enqueue_epoch_before_echo =
        enqueue_epoch_before.load(std::memory_order_acquire);
    result.callback_enqueue_epoch_after_echo =
        enqueue_epoch_after.load(std::memory_order_acquire);

    if (first_frame_report.has_value()) {
        result.first_capture_count = first_frame_report->capture_count;
        result.first_captured_snapshot_sequence =
            first_frame_report->captured_snapshot_sequence;
        result.stale_capture_observed =
            result.first_captured_snapshot_sequence == result.pre_echo_snapshot_sequence;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> echo_snapshot =
        wait_for_echo_snapshot(app, fixture.surface);
    if (echo_snapshot != nullptr) {
        result.echo_snapshot_published = true;
        result.echo_snapshot_sequence = echo_snapshot->metadata.sequence;
        result.echo_snapshot_callback_epoch =
            echo_snapshot->metadata.backend_callback_epoch;
    }

    result.callback_processed_epoch_after_capture =
        term::VNM_TerminalSurface_render_bridge::backend_callback_processed_epoch(
            fixture.surface);
    const term::Terminal_surface_render_invalidation_stats_t invalidation_after =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(fixture.surface);
    result.backend_callback_frame_deferrals_after =
        invalidation_after.backend_callback_frame_deferrals;

    result.passed =
        result.input_accepted &&
        result.echo_injected_before_sync &&
        first_frame_report.has_value() &&
        !result.stale_capture_observed &&
        result.echo_snapshot_published &&
        result.callback_enqueue_epoch_after_echo >
            result.callback_enqueue_epoch_before_echo &&
        result.callback_processed_epoch_after_capture >=
            result.callback_enqueue_epoch_after_echo &&
        result.echo_snapshot_callback_epoch >=
            result.callback_enqueue_epoch_after_echo &&
        result.first_captured_snapshot_sequence >= result.echo_snapshot_sequence &&
        result.backend_callback_frame_deferrals_after >
            result.backend_callback_frame_deferrals_before;

    return result;
}

Sample_result run_attempt(
    QGuiApplication&       app,
    VNM_TerminalSurface&   surface,
    Scripted_backend&      backend,
    const Benchmark_options& options,
    int                    catchup_budget_us,
    int                    echo_delay_us,
    int                    sample_index,
    QString*               out_error)
{
    Sample_result sample;
    sample.sample_index      = sample_index;
    sample.catchup_budget_us = catchup_budget_us;
    sample.echo_delay_us     = echo_delay_us;

    if (!prepare_attempt(app, surface, backend, sample_index, out_error)) {
        return sample;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> before_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(surface);
    if (before_snapshot != nullptr) {
        sample.snapshot_sequence_before = before_snapshot->metadata.sequence;
    }

    const steady_clock::time_point input_started = steady_clock::now();
    sample.input_accepted = send_input_key(surface);
    const steady_clock::time_point input_finished = steady_clock::now();
    sample.input_event_elapsed_ns = elapsed_ns_count(input_finished - input_started);

    const steady_clock::time_point echo_due =
        input_started + std::chrono::microseconds{echo_delay_us};
    const steady_clock::time_point polish_due =
        input_started + std::chrono::microseconds{options.polish_delay_us};

    bool echo_enqueued = false;
    if (echo_due <= polish_due) {
        wait_until_time(echo_due);
        const steady_clock::time_point echo_enqueued_at = steady_clock::now();
        sample.actual_echo_delay_ns = elapsed_ns_count(echo_enqueued_at - input_started);
        echo_enqueued = enqueue_backlog_and_echo(
            backend,
            options.pre_echo_backlog,
            out_error);
        if (!echo_enqueued) {
            return sample;
        }
        sample.echo_enqueued_before_polish = true;
        wait_until_time(polish_due);
    }
    else {
        wait_until_time(polish_due);
    }

    const term::Terminal_surface_backend_drain_stats_t stats_before =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(surface);
    const steady_clock::time_point polish_started = steady_clock::now();
    term::VNM_TerminalSurface_render_bridge::simulate_update_polish(surface);
    const steady_clock::time_point polish_finished = steady_clock::now();
    const term::Terminal_surface_backend_drain_stats_t stats_after =
        term::VNM_TerminalSurface_render_bridge::backend_drain_stats(surface);

    sample.polish_elapsed_ns = elapsed_ns_count(polish_finished - polish_started);
    sample.input_to_polish_done_ns = elapsed_ns_count(polish_finished - input_started);
    sample.drain_delta = drain_delta(stats_before, stats_after);
    sample.callbacks_queued_after_polish =
        term::VNM_TerminalSurface_render_bridge::backend_callback_drain_queued(surface);

    const std::shared_ptr<const term::Terminal_render_snapshot> after_snapshot =
        term::VNM_TerminalSurface_render_bridge::render_snapshot(surface);
    if (after_snapshot != nullptr) {
        sample.snapshot_sequence_after = after_snapshot->metadata.sequence;
        sample.echo_visible_at_polish = snapshot_contains_text(
            *after_snapshot,
            k_echo_visible_text);
        if (after_snapshot->cursor.visible) {
            sample.cursor_column_at_polish = after_snapshot->cursor.position.column;
        }
    }

    sample.stale_frame_at_polish =
        sample.echo_enqueued_before_polish && !sample.echo_visible_at_polish;
    sample.cursor_stale_at_polish =
        sample.echo_enqueued_before_polish &&
        sample.cursor_column_at_polish >= 0 &&
        sample.cursor_column_at_polish < sample.expected_cursor_column;
    sample.cursor_stale_cells = sample.cursor_stale_at_polish
        ? sample.expected_cursor_column - sample.cursor_column_at_polish
        : 0;

    if (!echo_enqueued) {
        wait_until_time(echo_due);
        const steady_clock::time_point echo_enqueued_at = steady_clock::now();
        sample.actual_echo_delay_ns = elapsed_ns_count(echo_enqueued_at - input_started);
        if (!enqueue_backlog_and_echo(backend, options.pre_echo_backlog, out_error)) {
            return sample;
        }
    }

    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(surface);
    pump_events(app, 2);
    (void)mark_snapshot_rendered_for_next_attempt(surface, out_error);
    pump_events(app, 1);

    return sample;
}

std::optional<Case_result> run_case(
    QGuiApplication&          app,
    const Benchmark_options&  options,
    int                       catchup_budget_us,
    int                       echo_delay_us,
    QString*                  out_error)
{
    Surface_fixture fixture;
    pump_events(app);

    Scripted_backend* backend = nullptr;
    if (!start_surface(fixture.surface, &backend, out_error) || backend == nullptr) {
        return std::nullopt;
    }

    term::VNM_TerminalSurface_render_bridge::
        set_backend_callback_frame_catchup_budget_for_benchmark(
            fixture.surface,
            std::chrono::microseconds{catchup_budget_us});

    Case_result result;
    result.catchup_budget_us = catchup_budget_us;
    result.echo_delay_us     = echo_delay_us;
    result.samples.reserve(static_cast<std::size_t>(options.iterations));

    const int total_attempts = options.warmup + options.iterations;
    for (int attempt = 0; attempt < total_attempts; ++attempt) {
        const Sample_result sample = run_attempt(
            app,
            fixture.surface,
            *backend,
            options,
            catchup_budget_us,
            echo_delay_us,
            attempt,
            out_error);
        if (!out_error->isEmpty()) {
            return std::nullopt;
        }

        if (attempt >= options.warmup) {
            result.samples.push_back(sample);
        }
    }

    return result;
}

QJsonObject case_json(const Case_result& result)
{
    int echo_eligible_count              = 0;
    int echo_visible_count               = 0;
    int stale_frame_count                = 0;
    int cursor_stale_count               = 0;
    int callbacks_queued_after_polish    = 0;
    int max_cursor_stale_cells           = 0;
    Backend_drain_delta total_drain_delta;
    std::vector<qint64> actual_echo_delay_ns;
    std::vector<qint64> catchup_elapsed_ns;
    std::vector<qint64> input_event_elapsed_ns;
    std::vector<qint64> input_to_polish_done_ns;
    std::vector<qint64> polish_elapsed_ns;

    QJsonArray samples;
    for (const Sample_result& sample : result.samples) {
        samples.append(sample_json(sample));
        if (sample.echo_enqueued_before_polish) {
            ++echo_eligible_count;
        }
        if (sample.echo_visible_at_polish) {
            ++echo_visible_count;
        }
        if (sample.stale_frame_at_polish) {
            ++stale_frame_count;
        }
        if (sample.cursor_stale_at_polish) {
            ++cursor_stale_count;
        }
        if (sample.callbacks_queued_after_polish) {
            ++callbacks_queued_after_polish;
        }
        max_cursor_stale_cells =
            std::max(max_cursor_stale_cells, sample.cursor_stale_cells);
        add_drain_delta(&total_drain_delta, sample.drain_delta);
        actual_echo_delay_ns.push_back(sample.actual_echo_delay_ns);
        catchup_elapsed_ns.push_back(bounded_json_i64(sample.drain_delta.total_elapsed_ns));
        input_event_elapsed_ns.push_back(sample.input_event_elapsed_ns);
        input_to_polish_done_ns.push_back(sample.input_to_polish_done_ns);
        polish_elapsed_ns.push_back(sample.polish_elapsed_ns);
    }

    QJsonObject object;
    object.insert(QStringLiteral("catchup_budget_us"), result.catchup_budget_us);
    object.insert(QStringLiteral("echo_delay_us"), result.echo_delay_us);
    object.insert(QStringLiteral("sample_count"), static_cast<int>(result.samples.size()));
    object.insert(QStringLiteral("echo_eligible_for_polish_count"), echo_eligible_count);
    object.insert(QStringLiteral("echo_visible_at_polish_count"), echo_visible_count);
    object.insert(QStringLiteral("stale_frame_at_polish_count"), stale_frame_count);
    object.insert(QStringLiteral("cursor_stale_at_polish_count"), cursor_stale_count);
    object.insert(
        QStringLiteral("callbacks_queued_after_polish_count"),
        callbacks_queued_after_polish);
    object.insert(QStringLiteral("max_cursor_stale_cells"), max_cursor_stale_cells);
    object.insert(QStringLiteral("backend_drain_delta_total"), drain_delta_json(total_drain_delta));
    object.insert(
        QStringLiteral("actual_echo_delay_ns"),
        summary_json(summarize_samples(std::move(actual_echo_delay_ns))));
    object.insert(
        QStringLiteral("catchup_elapsed_ns"),
        summary_json(summarize_samples(std::move(catchup_elapsed_ns))));
    object.insert(
        QStringLiteral("input_event_elapsed_ns"),
        summary_json(summarize_samples(std::move(input_event_elapsed_ns))));
    object.insert(
        QStringLiteral("input_to_polish_done_ns"),
        summary_json(summarize_samples(std::move(input_to_polish_done_ns))));
    object.insert(
        QStringLiteral("polish_elapsed_ns"),
        summary_json(summarize_samples(std::move(polish_elapsed_ns))));
    object.insert(QStringLiteral("samples"), samples);
    return object;
}

QJsonObject config_json(const Benchmark_options& options)
{
    QJsonObject object;
    object.insert(QStringLiteral("iterations"), options.iterations);
    object.insert(QStringLiteral("warmup"), options.warmup);
    object.insert(QStringLiteral("polish_delay_us"), options.polish_delay_us);
    object.insert(QStringLiteral("pre_echo_backlog_slices"), options.pre_echo_backlog);
    object.insert(QStringLiteral("pre_echo_backlog_slice_bytes"), k_backlog_slice_bytes);
    object.insert(QStringLiteral("catchup_budgets_us"), int_array_json(options.catchup_budgets_us));
    object.insert(QStringLiteral("echo_delay_buckets_us"), int_array_json(options.echo_delay_us));
    object.insert(QStringLiteral("backlog_payload_semantics"), QStringLiteral(
        "cursor-neutral NUL bytes placed before the echo in one coalesced output command"));
    object.insert(
        QStringLiteral("echo_payload_text"),
        QString::fromLatin1(k_echo_payload.constData(), k_echo_payload.size()));
    object.insert(QStringLiteral("latency_unit"), QStringLiteral("ns"));
    object.insert(QStringLiteral("time_source"), QStringLiteral("std::chrono::steady_clock"));
    return object;
}

QJsonObject build_root_json(
    const Benchmark_options&       options,
    const std::vector<Case_result>& results)
{
    QJsonArray cases;
    for (const Case_result& result : results) {
        cases.append(case_json(result));
    }

    QJsonObject root;
    root.insert(QStringLiteral("schema"), k_schema_name);
    root.insert(QStringLiteral("schema_version"), k_schema_version);
    root.insert(QStringLiteral("measurement_boundary"), k_measurement_boundary);
    root.insert(QStringLiteral("config"), config_json(options));
    root.insert(QStringLiteral("cases"), cases);
    return root;
}

QJsonObject queue_contract_json(const Queue_contract_result& result)
{
    QJsonObject object;
    object.insert(QStringLiteral("input_accepted"), result.input_accepted);
    object.insert(
        QStringLiteral("echo_injected_before_sync"),
        result.echo_injected_before_sync);
    object.insert(
        QStringLiteral("stale_capture_observed"),
        result.stale_capture_observed);
    object.insert(
        QStringLiteral("echo_snapshot_published"),
        result.echo_snapshot_published);
    object.insert(QStringLiteral("passed"), result.passed);
    insert_u64(
        object,
        QStringLiteral("callback_enqueue_epoch_before_echo"),
        result.callback_enqueue_epoch_before_echo);
    insert_u64(
        object,
        QStringLiteral("callback_enqueue_epoch_after_echo"),
        result.callback_enqueue_epoch_after_echo);
    insert_u64(
        object,
        QStringLiteral("callback_processed_epoch_after_capture"),
        result.callback_processed_epoch_after_capture);
    insert_u64(
        object,
        QStringLiteral("pre_echo_snapshot_sequence"),
        result.pre_echo_snapshot_sequence);
    insert_u64(
        object,
        QStringLiteral("pre_echo_snapshot_callback_epoch"),
        result.pre_echo_snapshot_callback_epoch);
    insert_u64(
        object,
        QStringLiteral("polished_snapshot_sequence"),
        result.polished_snapshot_sequence);
    insert_u64(
        object,
        QStringLiteral("polished_snapshot_callback_epoch"),
        result.polished_snapshot_callback_epoch);
    insert_u64(
        object,
        QStringLiteral("first_capture_count"),
        result.first_capture_count);
    insert_u64(
        object,
        QStringLiteral("first_captured_snapshot_sequence"),
        result.first_captured_snapshot_sequence);
    insert_u64(
        object,
        QStringLiteral("echo_snapshot_sequence"),
        result.echo_snapshot_sequence);
    insert_u64(
        object,
        QStringLiteral("echo_snapshot_callback_epoch"),
        result.echo_snapshot_callback_epoch);
    insert_u64(
        object,
        QStringLiteral("backend_callback_frame_deferrals_before"),
        result.backend_callback_frame_deferrals_before);
    insert_u64(
        object,
        QStringLiteral("backend_callback_frame_deferrals_after"),
        result.backend_callback_frame_deferrals_after);
    return object;
}

QJsonObject build_queue_contract_root_json(const Queue_contract_result& result)
{
    QJsonObject root;
    root.insert(QStringLiteral("schema"), k_queue_contract_schema_name);
    root.insert(QStringLiteral("schema_version"), k_schema_version);
    root.insert(
        QStringLiteral("measurement_boundary"),
        k_queue_contract_measurement_boundary);
    root.insert(
        QStringLiteral("decision_criteria"),
        k_queue_contract_decision_criteria);
    root.insert(QStringLiteral("queue_contract"), queue_contract_json(result));
    return root;
}

bool validate_json_root(
    const QJsonObject&      root,
    const Benchmark_options& options,
    QString*                out_error)
{
    if (options.queue_contract) {
        if (root.value(QStringLiteral("schema")).toString() !=
                k_queue_contract_schema_name ||
            root.value(QStringLiteral("schema_version")).toInt() != k_schema_version)
        {
            *out_error = QStringLiteral("queue contract root schema fields are invalid");
            return false;
        }

        const QJsonObject contract =
            root.value(QStringLiteral("queue_contract")).toObject();
        if (contract.isEmpty()) {
            *out_error = QStringLiteral("queue contract object is missing");
            return false;
        }
        if (!contract.value(QStringLiteral("passed")).toBool()) {
            *out_error = QStringLiteral("queue contract did not pass");
            return false;
        }
        if (contract.value(QStringLiteral("stale_capture_observed")).toBool()) {
            *out_error = QStringLiteral("queue contract observed a stale capture");
            return false;
        }
        if (contract.value(QStringLiteral("callback_enqueue_epoch_after_echo")).toInteger() <=
            contract.value(QStringLiteral("callback_enqueue_epoch_before_echo")).toInteger())
        {
            *out_error = QStringLiteral("queue contract did not advance enqueue epoch");
            return false;
        }
        if (contract.value(QStringLiteral("echo_snapshot_callback_epoch")).toInteger() <
            contract.value(QStringLiteral("callback_enqueue_epoch_after_echo")).toInteger())
        {
            *out_error = QStringLiteral("queue contract echo snapshot epoch is stale");
            return false;
        }
        return true;
    }

    if (root.value(QStringLiteral("schema")).toString() != k_schema_name ||
        root.value(QStringLiteral("schema_version")).toInt() != k_schema_version)
    {
        *out_error = QStringLiteral("root schema fields are invalid");
        return false;
    }

    const QJsonArray cases = root.value(QStringLiteral("cases")).toArray();
    const qsizetype expected_case_count =
        static_cast<qsizetype>(options.catchup_budgets_us.size() * options.echo_delay_us.size());
    if (cases.size() != expected_case_count) {
        *out_error = QStringLiteral("case count mismatch");
        return false;
    }

    for (const QJsonValue& value : cases) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("sample_count")).toInt() != options.iterations) {
            *out_error = QStringLiteral("case sample_count does not match iterations");
            return false;
        }
        if (object.value(QStringLiteral("samples")).toArray().size() != options.iterations) {
            *out_error = QStringLiteral("case sample array does not match iterations");
            return false;
        }
        if (!object.contains(QStringLiteral("catchup_elapsed_ns")) ||
            !object.contains(QStringLiteral("backend_drain_delta_total")))
        {
            *out_error = QStringLiteral("case missing required metric groups");
            return false;
        }
    }

    return true;
}

bool write_json_output(
    const QJsonObject&  root,
    const QString&      output_path,
    QString*            out_error)
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
    QGuiApplication app(argc, argv);

    Benchmark_options options;
    QString error;
    if (!parse_options(app.arguments(), &options, &error)) {
        std::cerr << error.toStdString() << "\n";
        print_usage();
        return 2;
    }
    if (options.help) {
        print_usage();
        return 0;
    }

    if (options.queue_contract) {
        if (!options.quiet) {
            std::cerr << "running deterministic input echo queue contract\n";
        }

        const std::optional<Queue_contract_result> result =
            run_queue_contract(app, &error);
        if (!result.has_value()) {
            std::cerr << error.toStdString() << "\n";
            return 1;
        }

        const QJsonObject root = build_queue_contract_root_json(*result);
        if (options.validate_json &&
            !validate_json_root(root, options, &error))
        {
            std::cerr << error.toStdString() << "\n";
            return 1;
        }

        if (!write_json_output(root, options.output_path, &error)) {
            std::cerr << error.toStdString() << "\n";
            return 1;
        }

        return 0;
    }

    std::vector<Case_result> results;
    results.reserve(options.catchup_budgets_us.size() * options.echo_delay_us.size());
    for (int echo_delay_us : options.echo_delay_us) {
        for (int catchup_budget_us : options.catchup_budgets_us) {
            if (!options.quiet) {
                std::cerr
                    << "running echo_delay_us=" << echo_delay_us
                    << " catchup_budget_us=" << catchup_budget_us << "\n";
            }

            std::optional<Case_result> result =
                run_case(app, options, catchup_budget_us, echo_delay_us, &error);
            if (!result.has_value()) {
                std::cerr << error.toStdString() << "\n";
                return 1;
            }
            results.push_back(std::move(*result));
        }
    }

    const QJsonObject root = build_root_json(options, results);
    if (options.validate_json &&
        !validate_json_root(root, options, &error))
    {
        std::cerr << error.toStdString() << "\n";
        return 1;
    }

    if (!write_json_output(root, options.output_path, &error)) {
        std::cerr << error.toStdString() << "\n";
        return 1;
    }

    return 0;
}
