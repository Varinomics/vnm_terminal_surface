#include "vnm_terminal/internal/backend_contract.h"
#include "vnm_terminal/internal/render_snapshot.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/vnm_terminal_surface.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QGuiApplication>
#include <QIODevice>
#include <QImage>
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
#include <QtGlobal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

constexpr int k_schema_version = 1;

const QString k_schema_name = QStringLiteral("vnm_terminal_input_echo_ordering_benchmark");
const QString k_measurement_boundary = QStringLiteral(
    "surface_input_to_qsg_sync_snapshot_capture_event_order");
const QByteArray k_prompt_payload = QByteArrayLiteral("\x1b[2J\x1b[H> ");
const QByteArray k_echo_payload   = QByteArrayLiteral("x");
const QString k_prompt_text       = QStringLiteral(">");
const QString k_echo_text         = QStringLiteral("> x");

enum class Injection_stage
{
    BEFORE_POLISH,
    AFTER_POLISH_BEFORE_SYNC,
    AFTER_CAPTURE,
};

struct Benchmark_options
{
    QString output_path;
    bool    quiet         = false;
    bool    validate_json = false;
    bool    help          = false;
};

struct Event_recorder
{
    int mark() { return ++epoch; }

    int epoch = 0;
};

struct Ordering_sample
{
    Injection_stage stage = Injection_stage::BEFORE_POLISH;
    QString         stage_name;
    bool            passed = false;

    int input_dispatch_epoch           = 0;
    int accepted_input_epoch           = 0;
    int backend_write_epoch            = 0;
    int callback_enqueue_epoch         = 0;
    int callback_deliver_epoch         = 0;
    int polish_begin_epoch             = 0;
    int polish_complete_epoch          = 0;
    int qsg_sync_epoch                 = 0;
    int first_capture_epoch            = 0;
    int second_capture_epoch           = 0;

    std::uint64_t pre_input_snapshot_sequence    = 0U;
    std::uint64_t pre_capture_snapshot_sequence  = 0U;
    std::uint64_t echo_snapshot_sequence         = 0U;
    std::uint64_t first_capture_count            = 0U;
    std::uint64_t second_capture_count           = 0U;
    std::uint64_t first_captured_snapshot        = 0U;
    std::uint64_t second_captured_snapshot       = 0U;

    bool input_accepted                   = false;
    bool echo_visible_at_polish           = false;
    bool first_capture_contains_echo      = false;
    bool first_capture_used_pre_echo      = false;
    bool second_capture_contains_echo     = false;
    bool second_capture_used_echo_snapshot = false;
};

class Scripted_backend final : public term::Terminal_backend
{
public:
    explicit Scripted_backend(Event_recorder& recorder)
    :
        m_recorder(recorder)
    {}

    term::Terminal_backend_result start(
        const term::Terminal_launch_config&   config,
        term::Terminal_backend_callbacks      callbacks) override
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
        m_last_write_epoch = m_recorder.mark();
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

        m_last_callback_enqueue_epoch = m_recorder.mark();
        m_callbacks.output_received(std::move(bytes));
        return true;
    }

    int last_write_epoch() const { return m_last_write_epoch; }
    int last_callback_enqueue_epoch() const { return m_last_callback_enqueue_epoch; }

private:
    Event_recorder&                                      m_recorder;
    bool                                                 m_running = false;
    int                                                  m_last_write_epoch = 0;
    int                                                  m_last_callback_enqueue_epoch = 0;
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

QString stage_name(Injection_stage stage)
{
    switch (stage) {
        case Injection_stage::BEFORE_POLISH:             return QStringLiteral("before_polish");
        case Injection_stage::AFTER_POLISH_BEFORE_SYNC:  return QStringLiteral("after_polish_before_sync");
        case Injection_stage::AFTER_CAPTURE:             return QStringLiteral("after_capture");
        default:                                         return QStringLiteral("unknown");
    }
}

void pump_events(QGuiApplication& app, int rounds = 4)
{
    for (int i = 0; i < rounds; ++i) {
        app.processEvents(QEventLoop::AllEvents);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
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

std::shared_ptr<const term::Terminal_render_snapshot> current_snapshot(
    const VNM_TerminalSurface& surface)
{
    return term::VNM_TerminalSurface_render_bridge::render_snapshot(surface);
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

bool start_surface(
    VNM_TerminalSurface&   surface,
    Event_recorder&        recorder,
    Scripted_backend**     out_backend,
    QString*               out_error)
{
    auto backend = std::make_unique<Scripted_backend>(recorder);
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

bool prepare_prompt_frame(
    QGuiApplication&       app,
    VNM_TerminalSurface&   surface,
    Scripted_backend&      backend,
    Ordering_sample*       sample,
    QString*               out_error)
{
    if (!backend.emit_output(k_prompt_payload)) {
        *out_error = QStringLiteral("scripted backend rejected prompt output");
        return false;
    }

    term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(surface);
    pump_events(app, 2);

    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        current_snapshot(surface);
    if (snapshot == nullptr || !snapshot_contains_text(*snapshot, k_prompt_text)) {
        *out_error = QStringLiteral("prompt setup did not publish a render snapshot");
        return false;
    }
    sample->pre_input_snapshot_sequence = snapshot->metadata.sequence;

    if (!mark_snapshot_rendered_for_next_attempt(surface, out_error)) {
        return false;
    }

    term::VNM_TerminalSurface_render_bridge::set_cursor_blink_visible(surface, true);
    const term::Terminal_surface_render_invalidation_stats_t pending_stats =
        term::VNM_TerminalSurface_render_bridge::invalidation_stats(surface);
    if (!pending_stats.pending_update) {
        *out_error = QStringLiteral("attempt setup did not leave a frame update pending");
        return false;
    }

    return true;
}

bool send_input_key(
    VNM_TerminalSurface&   surface,
    Event_recorder&        recorder,
    Ordering_sample*       sample)
{
    sample->input_dispatch_epoch = recorder.mark();
    QKeyEvent event(
        QEvent::KeyPress,
        Qt::Key_X,
        Qt::NoModifier,
        QStringLiteral("x"));
    QCoreApplication::sendEvent(&surface, &event);
    if (event.isAccepted()) {
        sample->accepted_input_epoch = recorder.mark();
    }
    return event.isAccepted();
}

std::optional<term::Qsg_atlas_frame_report> capture_next_surface_frame(
    QGuiApplication&       app,
    QQuickWindow&          window,
    VNM_TerminalSurface&   surface,
    std::uint64_t          previous_capture_count)
{
    for (int i = 0; i < 40; ++i) {
        surface.update();
        window.requestUpdate();
        pump_events(app, 1);
        const QImage image = window.grabWindow();
        const term::Qsg_atlas_frame_report report =
            term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(surface);
        if (!image.isNull() && report.capture_count > previous_capture_count) {
            return report;
        }
    }

    return std::nullopt;
}

bool snapshot_sequence_contains_echo(
    VNM_TerminalSurface&   surface,
    std::uint64_t*         out_sequence)
{
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        current_snapshot(surface);
    if (snapshot == nullptr || !snapshot_contains_text(*snapshot, k_echo_text)) {
        return false;
    }

    *out_sequence = snapshot->metadata.sequence;
    return true;
}

bool complete_after_callback_delivery(
    QGuiApplication&       app,
    VNM_TerminalSurface&   surface,
    Event_recorder&        recorder,
    Ordering_sample*       sample)
{
    for (int i = 0; i < 20; ++i) {
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(surface);
        pump_events(app, 1);
        if (snapshot_sequence_contains_echo(surface, &sample->echo_snapshot_sequence)) {
            sample->callback_deliver_epoch = recorder.mark();
            return true;
        }
    }

    return false;
}

bool capture_first_frame(
    QGuiApplication&       app,
    Surface_fixture&       fixture,
    Event_recorder&        recorder,
    Ordering_sample*       sample,
    QString*               out_error)
{
    const term::Qsg_atlas_frame_report before =
        term::VNM_TerminalSurface_render_bridge::qsg_atlas_frame(fixture.surface);

    const std::optional<term::Qsg_atlas_frame_report> report =
        capture_next_surface_frame(app, fixture.window, fixture.surface, before.capture_count);
    if (!report.has_value()) {
        *out_error = QStringLiteral("first frame was not captured");
        return false;
    }

    if (sample->echo_snapshot_sequence == 0U &&
        snapshot_sequence_contains_echo(fixture.surface, &sample->echo_snapshot_sequence))
    {
        sample->callback_deliver_epoch = recorder.mark();
    }

    sample->first_capture_epoch     = recorder.mark();
    sample->first_capture_count     = report->capture_count;
    sample->first_captured_snapshot = report->captured_snapshot_sequence;
    sample->first_capture_contains_echo =
        sample->echo_snapshot_sequence != 0U &&
        report->captured_snapshot_sequence >= sample->echo_snapshot_sequence;
    sample->first_capture_used_pre_echo =
        report->captured_snapshot_sequence == sample->pre_input_snapshot_sequence;
    return true;
}

bool capture_second_frame(
    QGuiApplication&       app,
    Surface_fixture&       fixture,
    Event_recorder&        recorder,
    Ordering_sample*       sample,
    QString*               out_error)
{
    const std::optional<term::Qsg_atlas_frame_report> report =
        capture_next_surface_frame(
            app,
            fixture.window,
            fixture.surface,
            sample->first_capture_count);
    if (!report.has_value()) {
        *out_error = QStringLiteral("second frame was not captured");
        return false;
    }

    if (sample->echo_snapshot_sequence == 0U &&
        snapshot_sequence_contains_echo(fixture.surface, &sample->echo_snapshot_sequence))
    {
        sample->callback_deliver_epoch = recorder.mark();
    }

    sample->second_capture_epoch      = recorder.mark();
    sample->second_capture_count      = report->capture_count;
    sample->second_captured_snapshot  = report->captured_snapshot_sequence;
    sample->second_capture_contains_echo =
        sample->echo_snapshot_sequence != 0U &&
        report->captured_snapshot_sequence >= sample->echo_snapshot_sequence;
    sample->second_capture_used_echo_snapshot =
        report->captured_snapshot_sequence == sample->echo_snapshot_sequence;
    return true;
}

void simulate_polish(
    VNM_TerminalSurface&   surface,
    Event_recorder&        recorder,
    Ordering_sample*       sample)
{
    sample->polish_begin_epoch = recorder.mark();
    term::VNM_TerminalSurface_render_bridge::simulate_update_polish(surface);
    sample->polish_complete_epoch = recorder.mark();

    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        current_snapshot(surface);
    if (snapshot != nullptr) {
        sample->pre_capture_snapshot_sequence = snapshot->metadata.sequence;
        sample->echo_visible_at_polish = snapshot_contains_text(*snapshot, k_echo_text);
        if (sample->echo_visible_at_polish) {
            sample->echo_snapshot_sequence = snapshot->metadata.sequence;
            sample->callback_deliver_epoch = recorder.mark();
        }
    }

}

bool run_case(
    QGuiApplication&       app,
    Injection_stage        stage,
    Ordering_sample*       sample,
    QString*               out_error)
{
    Event_recorder recorder;
    Surface_fixture fixture;
    pump_events(app);

    Scripted_backend* backend = nullptr;
    if (!start_surface(fixture.surface, recorder, &backend, out_error) || backend == nullptr) {
        return false;
    }

    sample->stage      = stage;
    sample->stage_name = stage_name(stage);
    if (!prepare_prompt_frame(app, fixture.surface, *backend, sample, out_error)) {
        return false;
    }

    sample->input_accepted = send_input_key(fixture.surface, recorder, sample);
    sample->backend_write_epoch = backend->last_write_epoch();
    if (!sample->input_accepted) {
        *out_error = QStringLiteral("input key was not accepted");
        return false;
    }

    if (stage == Injection_stage::BEFORE_POLISH) {
        if (!backend->emit_output(k_echo_payload)) {
            *out_error = QStringLiteral("scripted backend rejected echo output");
            return false;
        }
        sample->callback_enqueue_epoch = backend->last_callback_enqueue_epoch();
    }

    std::optional<QMetaObject::Connection> sync_connection;
    if (stage == Injection_stage::AFTER_POLISH_BEFORE_SYNC) {
        sync_connection = QObject::connect(
            &fixture.window,
            &QQuickWindow::beforeSynchronizing,
            &fixture.window,
            [&] {
                if (sample->callback_enqueue_epoch != 0) {
                    return;
                }

                sample->qsg_sync_epoch = recorder.mark();
                (void)backend->emit_output(k_echo_payload);
                sample->callback_enqueue_epoch = backend->last_callback_enqueue_epoch();
            },
            Qt::DirectConnection);
    }

    simulate_polish(fixture.surface, recorder, sample);

    if (stage == Injection_stage::AFTER_POLISH_BEFORE_SYNC) {
        if (!capture_first_frame(app, fixture, recorder, sample, out_error)) {
            if (sync_connection.has_value()) {
                QObject::disconnect(*sync_connection);
            }
            return false;
        }
        if (sync_connection.has_value()) {
            QObject::disconnect(*sync_connection);
        }
        if (sample->echo_snapshot_sequence == 0U &&
            !complete_after_callback_delivery(app, fixture.surface, recorder, sample))
        {
            *out_error = QStringLiteral("echo callback was not delivered after sync injection");
            return false;
        }
        if (!sample->first_capture_contains_echo &&
            !capture_second_frame(app, fixture, recorder, sample, out_error))
        {
            return false;
        }
    }
    else {
        if (!capture_first_frame(app, fixture, recorder, sample, out_error)) {
            return false;
        }
    }

    if (stage == Injection_stage::AFTER_CAPTURE) {
        if (!backend->emit_output(k_echo_payload)) {
            *out_error = QStringLiteral("scripted backend rejected echo output");
            return false;
        }
        sample->callback_enqueue_epoch = backend->last_callback_enqueue_epoch();
        term::VNM_TerminalSurface_render_bridge::drain_backend_callback_events(fixture.surface);
        if (!complete_after_callback_delivery(app, fixture.surface, recorder, sample)) {
            *out_error = QStringLiteral("echo callback was not delivered after first capture");
            return false;
        }
        if (!capture_second_frame(app, fixture, recorder, sample, out_error)) {
            return false;
        }
    }

    if (stage == Injection_stage::BEFORE_POLISH) {
        sample->passed =
            sample->callback_enqueue_epoch    <  sample->polish_begin_epoch &&
            sample->echo_visible_at_polish                             &&
            sample->first_capture_contains_echo                         &&
            !sample->first_capture_used_pre_echo;
    }
    else
    if (stage == Injection_stage::AFTER_POLISH_BEFORE_SYNC) {
        sample->passed =
            sample->polish_complete_epoch     <  sample->qsg_sync_epoch &&
            sample->qsg_sync_epoch            <  sample->callback_enqueue_epoch &&
            sample->callback_enqueue_epoch    <  sample->callback_deliver_epoch &&
            sample->echo_snapshot_sequence != 0U                         &&
            (sample->first_capture_contains_echo
                ? !sample->first_capture_used_pre_echo
                : sample->second_capture_contains_echo &&
                    sample->second_capture_used_echo_snapshot);
    }
    else
    if (stage == Injection_stage::AFTER_CAPTURE) {
        sample->passed =
            sample->first_capture_epoch       <  sample->callback_enqueue_epoch &&
            sample->first_capture_used_pre_echo                         &&
            sample->second_capture_contains_echo                        &&
            sample->second_capture_used_echo_snapshot;
    }

    return true;
}

bool parse_options(
    const QStringList&  args,
    Benchmark_options*  options,
    QString*            out_error)
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
        << "Usage: vnm_terminal_input_echo_ordering_benchmark [options]\n"
        << "  --output PATH       write JSON to PATH instead of stdout\n"
        << "  --quiet             suppress non-JSON status output\n"
        << "  --validate-json     validate output schema and ordering criteria\n";
}

void insert_u64(QJsonObject& object, const QString& key, std::uint64_t value)
{
    object.insert(key, QJsonValue(static_cast<qint64>(value)));
}

QJsonObject sample_json(const Ordering_sample& sample)
{
    QJsonObject object;
    object.insert(QStringLiteral("stage"), sample.stage_name);
    object.insert(QStringLiteral("passed"), sample.passed);
    object.insert(QStringLiteral("input_dispatch_epoch"), sample.input_dispatch_epoch);
    object.insert(QStringLiteral("accepted_input_epoch"), sample.accepted_input_epoch);
    object.insert(QStringLiteral("backend_write_epoch"), sample.backend_write_epoch);
    object.insert(QStringLiteral("callback_enqueue_epoch"), sample.callback_enqueue_epoch);
    object.insert(QStringLiteral("callback_deliver_epoch"), sample.callback_deliver_epoch);
    object.insert(QStringLiteral("polish_begin_epoch"), sample.polish_begin_epoch);
    object.insert(QStringLiteral("polish_complete_epoch"), sample.polish_complete_epoch);
    object.insert(QStringLiteral("qsg_sync_epoch"), sample.qsg_sync_epoch);
    object.insert(QStringLiteral("first_capture_epoch"), sample.first_capture_epoch);
    object.insert(QStringLiteral("second_capture_epoch"), sample.second_capture_epoch);
    insert_u64(
        object,
        QStringLiteral("pre_input_snapshot_sequence"),
        sample.pre_input_snapshot_sequence);
    insert_u64(
        object,
        QStringLiteral("pre_capture_snapshot_sequence"),
        sample.pre_capture_snapshot_sequence);
    insert_u64(object, QStringLiteral("echo_snapshot_sequence"), sample.echo_snapshot_sequence);
    insert_u64(object, QStringLiteral("first_capture_count"), sample.first_capture_count);
    insert_u64(object, QStringLiteral("second_capture_count"), sample.second_capture_count);
    insert_u64(
        object,
        QStringLiteral("first_captured_snapshot"),
        sample.first_captured_snapshot);
    insert_u64(
        object,
        QStringLiteral("second_captured_snapshot"),
        sample.second_captured_snapshot);
    object.insert(QStringLiteral("input_accepted"), sample.input_accepted);
    object.insert(QStringLiteral("echo_visible_at_polish"), sample.echo_visible_at_polish);
    object.insert(
        QStringLiteral("first_capture_contains_echo"),
        sample.first_capture_contains_echo);
    object.insert(
        QStringLiteral("first_capture_used_pre_echo"),
        sample.first_capture_used_pre_echo);
    object.insert(
        QStringLiteral("second_capture_contains_echo"),
        sample.second_capture_contains_echo);
    object.insert(
        QStringLiteral("second_capture_used_echo_snapshot"),
        sample.second_capture_used_echo_snapshot);
    return object;
}

QJsonObject build_root_json(const std::vector<Ordering_sample>& samples)
{
    QJsonArray case_array;
    for (const Ordering_sample& sample : samples) {
        case_array.append(sample_json(sample));
    }

    QJsonObject criteria;
    criteria.insert(
        QStringLiteral("before_polish"),
        QStringLiteral("callback_enqueue_epoch < polish_begin_epoch and first capture contains echo"));
    criteria.insert(
        QStringLiteral("after_polish_before_sync"),
        QStringLiteral("polish_complete_epoch < qsg_sync_epoch < callback_enqueue_epoch, "
                       "and the first or follow-up capture uses the echo snapshot"));
    criteria.insert(
        QStringLiteral("after_capture"),
        QStringLiteral("callback_enqueue_epoch follows first_capture_epoch; first capture may be "
                       "pre-echo but second capture must use the echo snapshot"));

    QJsonObject root;
    root.insert(QStringLiteral("schema"), k_schema_name);
    root.insert(QStringLiteral("schema_version"), k_schema_version);
    root.insert(QStringLiteral("measurement_boundary"), k_measurement_boundary);
    root.insert(QStringLiteral("clock_source"), QStringLiteral("deterministic_event_epoch"));
    root.insert(QStringLiteral("decision_criteria"), criteria);
    root.insert(QStringLiteral("cases"), case_array);
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

    const QJsonArray cases = root.value(QStringLiteral("cases")).toArray();
    if (cases.size() != 3) {
        *out_error = QStringLiteral("expected exactly three ordering cases");
        return false;
    }

    for (const QJsonValue& value : cases) {
        const QJsonObject object = value.toObject();
        if (!object.value(QStringLiteral("passed")).toBool()) {
            *out_error = QStringLiteral("ordering case failed: %1")
                .arg(object.value(QStringLiteral("stage")).toString());
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

    const std::vector<Injection_stage> stages = {
        Injection_stage::BEFORE_POLISH,
        Injection_stage::AFTER_POLISH_BEFORE_SYNC,
        Injection_stage::AFTER_CAPTURE,
    };

    std::vector<Ordering_sample> samples;
    samples.reserve(stages.size());
    for (Injection_stage stage : stages) {
        if (!options.quiet) {
            std::cerr << "running stage=" << stage_name(stage).toStdString() << "\n";
        }

        Ordering_sample sample;
        if (!run_case(app, stage, &sample, &error)) {
            std::cerr << error.toStdString() << "\n";
            return 1;
        }
        samples.push_back(std::move(sample));
    }

    const QJsonObject root = build_root_json(samples);
    if (options.validate_json && !validate_json_root(root, &error)) {
        std::cerr << error.toStdString() << "\n";
        return 1;
    }

    if (!write_json_output(root, options.output_path, &error)) {
        std::cerr << error.toStdString() << "\n";
        return 1;
    }

    return 0;
}
