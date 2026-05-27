#include "helpers/decode_hex.h"
#include "helpers/test_check.h"
#include "vnm_terminal/internal/terminal_canvas_fixture_contract.h"
#include "vnm_terminal/internal/terminal_resize_controller.h"
#include "vnm_terminal/internal/terminal_session.h"

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QKeyEvent>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstddef>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;
using vnm_terminal::test_helpers::decode_hex;

term::Terminal_launch_config valid_launch_config()
{
    term::Terminal_launch_config config;
    config.argv              = {QStringLiteral("terminal-fixture"), QStringLiteral("--interactive")};
    config.working_directory = QStringLiteral("C:/workspace");
    config.initial_grid_size = term::terminal_grid_size_t{24, 80};
    return config;
}

term::Terminal_session_config tight_session_config()
{
    term::Terminal_session_config config;
    config.output_queue_limits.high_water_bytes    = 4U;
    config.output_queue_limits.hard_limit_bytes    = 8U;
    config.output_queue_limits.high_water_commands = 1U;
    config.output_queue_limits.hard_limit_commands = 3U;
    config.write_queue_limits.high_water_bytes     = 4U;
    config.write_queue_limits.hard_limit_bytes     = 8U;
    config.write_queue_limits.high_water_commands  = 1U;
    config.write_queue_limits.hard_limit_commands  = 3U;
    return config;
}

term::Terminal_session_config enable_test_traces(term::Terminal_session_config config = {})
{
    config.trace_command_limit              = 4096U;
    config.trace_notification_limit         = 4096U;
    config.trace_result_limit               = 4096U;
    config.trace_resize_limit               = 1024U;
    config.trace_output_chunk_limit         = 4096U;
    config.capture_last_model_ingest_result = true;
    return config;
}

class Scripted_backend final : public term::Terminal_backend
{
public:
    ~Scripted_backend() override
    {
        if (callback_worker.joinable()) {
            callback_worker.join();
        }

        if (emit_output_on_destroy) {
            m_callbacks.output_received(QByteArrayLiteral("destroy-output"));
        }
    }

    term::Terminal_backend_result start(
        const term::Terminal_launch_config&    config,
        term::Terminal_backend_callbacks       callbacks) override
    {
        const term::Terminal_backend_result callback_result =
            term::validate_backend_callbacks(callbacks);
        if (term::is_backend_rejection(callback_result)) {
            return callback_result;
        }

        m_callbacks = std::move(callbacks);
        start_configs.push_back(config);

        if (fail_start) {
            term::Terminal_backend_result result = term::backend_reject(
                term::Terminal_backend_error_code::START_FAILED,
                QStringLiteral("scripted start failure"));
            if (report_error_on_start_failure) {
                m_callbacks.error_reported(*result.error);
            }
            return result;
        }

        const term::Terminal_backend_result config_result =
            term::validate_launch_config(config);
        if (term::is_backend_rejection(config_result)) {
            m_callbacks.error_reported(*config_result.error);
            return config_result;
        }

        running = true;
        for (const QByteArray& output : outputs_during_start) {
            m_callbacks.output_received(output);
        }
        for (const term::Terminal_backend_error& error : errors_during_start) {
            m_callbacks.error_reported(error);
        }
        if (exit_during_start.has_value()) {
            running = false;
            m_callbacks.process_exited(*exit_during_start);
        }
        return term::backend_accept();
    }

    term::Terminal_backend_result write(QByteArray bytes) override
    {
        if (!running || fail_write) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::WRITE_FAILED,
                    QStringLiteral("scripted write failure"));
        }

        writes.push_back(std::move(bytes));
        for (const QByteArray& output : outputs_during_write) {
            m_callbacks.output_received(output);
        }
        if (after_outputs_during_write) {
            after_outputs_during_write();
        }
        return term::backend_accept();
    }

    term::Terminal_backend_result resize(term::Terminal_backend_resize_request request) override
    {
        resize_requests.push_back(request);
        if (!term::is_valid_grid_size(request.grid_size) || fail_resize) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::RESIZE_FAILED,
                    QStringLiteral("scripted resize failure"));
        }

        return term::backend_accept();
    }

    term::Terminal_backend_result set_output_paused(bool paused) override
    {
        output_pause_requests.push_back(paused);
        if (fail_output_pause_request_number               >  0 &&
            static_cast<int>(output_pause_requests.size()) == fail_output_pause_request_number)
        {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::OUTPUT_OVERFLOW,
                    QStringLiteral("scripted output pause failure"));
        }

        if (output_pause_failures_remaining > 0) {
            --output_pause_failures_remaining;
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::OUTPUT_OVERFLOW,
                    QStringLiteral("scripted output pause failure"));
        }

        output_paused = paused;
        return term::backend_accept();
    }

    term::Terminal_backend_result interrupt() override
    {
        if (!running) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::INTERRUPT_FAILED,
                    QStringLiteral("scripted interrupt without process"));
        }

        ++interrupt_count;
        if (exit_on_interrupt) {
            running = false;
            m_callbacks.process_exited({term::Terminal_exit_reason::INTERRUPTED, 130});
        }
        return term::backend_accept();
    }

    term::Terminal_backend_result terminate() override
    {
        if (!running) {
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::TERMINATE_FAILED,
                    QStringLiteral("scripted terminate without process"));
        }

        ++terminate_count;
        running = false;
        if (exit_on_terminate) {
            m_callbacks.process_exited({term::Terminal_exit_reason::TERMINATED, 0});
        }
        return term::backend_accept();
    }

    bool emit_output(QByteArray bytes)
    {
        if (!running || output_paused) {
            return false;
        }

        m_callbacks.output_received(std::move(bytes));
        return true;
    }

    bool emit_error(term::Terminal_backend_error error)
    {
        if (!running) {
            return false;
        }

        m_callbacks.error_reported(std::move(error));
        return true;
    }

    void emit_exit(term::Terminal_backend_exit exit)
    {
        running = false;
        m_callbacks.process_exited(exit);
    }

    void emit_output_from_worker_after_delay(
        QByteArray                         bytes,
        std::shared_ptr<std::atomic_bool>  callback_attempted)
    {
        if (callback_worker.joinable()) {
            callback_worker.join();
        }

        term::Terminal_backend_callbacks callbacks = m_callbacks;
        callback_worker = std::thread(
            [callbacks, bytes = std::move(bytes), callback_attempted]() mutable {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                callbacks.output_received(std::move(bytes));
                callback_attempted->store(true);
            });
    }

    bool                       fail_start                       = false;
    bool                       report_error_on_start_failure    = false;
    bool                       fail_write                       = false;
    bool                       fail_resize                      = false;
    bool                       running                          = false;
    bool                       exit_on_interrupt                = true;
    bool                       exit_on_terminate                = true;
    bool                       output_paused                    = false;
    int                        output_pause_failures_remaining  = 0;
    int                        fail_output_pause_request_number = 0;
    int                        interrupt_count                  = 0;
    int                        terminate_count                  = 0;
    std::vector<QByteArray>    outputs_during_start;
    std::vector<QByteArray>    outputs_during_write;
    std::function<void()>      after_outputs_during_write;
    std::vector<term::Terminal_backend_error>
                               errors_during_start;
    std::optional<term::Terminal_backend_exit>
                               exit_during_start;
    std::vector<QByteArray>    writes;
    std::vector<bool>          output_pause_requests;
    std::vector<term::Terminal_backend_resize_request>
                               resize_requests;
    std::vector<term::Terminal_launch_config>
                               start_configs;
    bool                       emit_output_on_destroy           = false;
    std::thread                callback_worker;

private:
    term::Terminal_backend_callbacks m_callbacks;
};

Scripted_backend* make_session(
    std::unique_ptr<term::Terminal_session>&   session,
    term::Terminal_session_config              config = {})
{
    auto              backend     = std::make_unique<Scripted_backend>();
    Scripted_backend* backend_ptr = backend.get();
    session = std::make_unique<term::Terminal_session>(
        std::move(backend),
        enable_test_traces(config));
    return backend_ptr;
}

std::size_t notification_count(
    const term::Terminal_session&              session,
    term::Terminal_session_notification_kind   kind)
{
    std::size_t count = 0U;
    for (const term::Terminal_session_notification& notification : session.notifications()) {
        if (notification.kind == kind) {
            ++count;
        }
    }
    return count;
}

std::optional<term::Terminal_session_notification> first_notification(
    const term::Terminal_session&              session,
    term::Terminal_session_notification_kind   kind)
{
    for (const term::Terminal_session_notification& notification : session.notifications()) {
        if (notification.kind == kind) {
            return notification;
        }
    }

    return std::nullopt;
}

std::vector<term::Terminal_session_notification> notifications_of_kind(
    const term::Terminal_session&              session,
    term::Terminal_session_notification_kind   kind)
{
    std::vector<term::Terminal_session_notification> notifications;
    for (const term::Terminal_session_notification& notification : session.notifications()) {
        if (notification.kind == kind) {
            notifications.push_back(notification);
        }
    }
    return notifications;
}

bool has_backend_error_code(
    const std::vector<term::Terminal_session_notification>&    notifications,
    term::Terminal_backend_error_code                          code)
{
    for (const term::Terminal_session_notification& notification : notifications) {
        if (notification.kind                == term::Terminal_session_notification_kind::BACKEND_ERROR &&
            notification.backend_error.has_value()                                                      &&
            notification.backend_error->code == code)
        {
            return true;
        }
    }

    return false;
}

std::optional<std::size_t> first_notification_index(
    const term::Terminal_session&              session,
    term::Terminal_session_notification_kind   kind)
{
    const std::vector<term::Terminal_session_notification> notifications =
        session.notifications();
    for (std::size_t i = 0U; i < notifications.size(); ++i) {
        if (notifications[i].kind == kind) {
            return i;
        }
    }

    return std::nullopt;
}

bool check_processed_sequences_are_monotonic(
    const term::Terminal_session&          session,
    const char*                            message)
{
    const std::vector<term::Terminal_session_command>& commands = session.processed_commands();
    for (std::size_t i = 1U; i < commands.size(); ++i) {
        if (commands[i - 1U].sequence >= commands[i].sequence) {
            return check(false, message);
        }
    }

    return check(true, message);
}

QString snapshot_row_text(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row)
{
    QString text;
    for (int column = 0; column < snapshot.grid_size.columns; ++column) {
        QString cell_text = QStringLiteral(" ");
        for (const term::Terminal_render_cell& cell : snapshot.cells) {
            if (cell.position.row == row && cell.position.column == column) {
                cell_text = cell.text;
                break;
            }
        }
        text += cell_text;
    }

    while (!text.isEmpty() && text.back() == QChar(u' ')) {
        text.chop(1);
    }
    return text;
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

bool snapshot_has_selection_span(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row,
    int                                    first_column,
    int                                    column_count)
{
    for (const term::Terminal_render_selection_span& span : snapshot.selection_spans) {
        if (span.row          == row          &&
            span.first_column == first_column &&
            span.column_count == column_count)
        {
            return true;
        }
    }

    return false;
}

std::vector<term::terminal_selection_line_lease_t> expected_line_leases_from_snapshot(
    const term::Terminal_render_snapshot&  snapshot,
    const term::Terminal_selection_range&  range)
{
    std::vector<term::terminal_selection_line_lease_t> lines;
    if (!term::render_snapshot_visible_line_provenance_is_valid(snapshot)) {
        return lines;
    }

    const term::terminal_grid_position_t start = term::normalized_selection_start(range);
    const term::terminal_grid_position_t end   = term::normalized_selection_end(range);
    const int first_visible_logical_row =
        term::render_snapshot_first_visible_logical_row(snapshot);
    const int last_visible_logical_row =
        first_visible_logical_row + snapshot.grid_size.rows - 1;
    if (start.row < first_visible_logical_row || end.row > last_visible_logical_row) {
        return lines;
    }

    lines.reserve(static_cast<std::size_t>(end.row - start.row + 1));
    for (int logical_row = start.row; logical_row <= end.row; ++logical_row) {
        const int viewport_row = logical_row - first_visible_logical_row;
        const term::Terminal_render_line_provenance& provenance =
            snapshot.visible_line_provenance[static_cast<std::size_t>(viewport_row)];
        lines.push_back({
            logical_row - start.row,
            provenance.retained_line_id,
            provenance.content_generation,
        });
    }
    return lines;
}

bool snapshot_cells_fit_grid(const term::Terminal_render_snapshot& snapshot)
{
    for (const term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row    <  0                          ||
            cell.position.row    >= snapshot.grid_size.rows     ||
            cell.position.column <  0                          ||
            cell.position.column >= snapshot.grid_size.columns)
        {
            return false;
        }
    }

    return true;
}

QByteArray numbered_scroll_lines(int count)
{
    QByteArray bytes;
    for (int i = 0; i < count; ++i) {
        bytes += QStringLiteral("scroll-line-%1\r\n")
            .arg(i, 3, 10, QLatin1Char('0'))
            .toUtf8();
    }
    return bytes;
}

QByteArray concatenate_writes(
    const std::vector<QByteArray>& writes,
    std::size_t                    first_index)
{
    QByteArray bytes;
    for (std::size_t i = first_index; i < writes.size(); ++i) {
        bytes += writes[i];
    }

    return bytes;
}

QByteArray repeat_bytes(const QByteArray& bytes, int repeat_count)
{
    QByteArray repeated;
    repeated.reserve(bytes.size() * repeat_count);
    for (int i = 0; i < repeat_count; ++i) {
        repeated += bytes;
    }

    return repeated;
}

QByteArray framed_paste(QByteArray body)
{
    QByteArray bytes = QByteArrayLiteral("\x1b[200~");
    bytes += body;
    bytes += QByteArrayLiteral("\x1b[201~");
    return bytes;
}

bool fixture_required_labels_were_seen(const std::vector<std::string_view>& seen_labels)
{
    bool ok = true;
    for (std::string_view label : term::terminal_canvas_fixture_required_labels()) {
        if (std::find(seen_labels.begin(), seen_labels.end(), label) != seen_labels.end()) {
            continue;
        }

        std::cerr << "FAIL: fixture required label was not replayed: " << label << '\n';
        ok = false;
    }

    return ok;
}

std::optional<std::size_t> fixture_record_index(
    term::Terminal_canvas_fixture_record_kind  kind,
    std::string_view                           label)
{
    const std::vector<term::terminal_canvas_fixture_record_t>& records =
        term::terminal_canvas_fixture_contract_script();
    for (std::size_t i = 0U; i < records.size(); ++i) {
        if (records[i].kind == kind && records[i].label == label) {
            return i;
        }
    }

    return std::nullopt;
}

bool fixture_record_precedes(
    term::Terminal_canvas_fixture_record_kind  before_kind,
    std::string_view                           before_label,
    term::Terminal_canvas_fixture_record_kind  after_kind,
    std::string_view                           after_label)
{
    const std::optional<std::size_t> before =
        fixture_record_index(before_kind, before_label);
    const std::optional<std::size_t> after =
        fixture_record_index(after_kind, after_label);
    return before.has_value() && after.has_value() && *before < *after;
}

bool fixture_acknowledgement_order_is_valid()
{
    using kind_t = term::Terminal_canvas_fixture_record_kind;

    return
        fixture_record_precedes(
            kind_t::EXPECT_INPUT,
            "prompt-editing-keys",
            kind_t::OUTPUT,
            "prompt-editing-keys-ack") &&
        fixture_record_precedes(
            kind_t::RESIZE,
            "resize",
            kind_t::OUTPUT,
            "resize-report") &&
        fixture_record_precedes(
            kind_t::EXPECT_INPUT,
            "bracketed-paste",
            kind_t::OUTPUT,
            "bracketed-paste-ack") &&
        fixture_record_precedes(
            kind_t::EXPECT_INPUT,
            "focus-reporting",
            kind_t::OUTPUT,
            "focus-reporting-ack") &&
        fixture_record_precedes(
            kind_t::EXPECT_INPUT,
            "mouse-sgr-1006",
            kind_t::OUTPUT,
            "mouse-sgr-1006-ack");
}

bool processed_command_kinds_match(
    const std::vector<term::Terminal_session_command>&         commands,
    const std::vector<term::Terminal_session_command_kind>&    expected_kinds,
    const char*                                                message)
{
    bool ok = true;
    if (commands.size() != expected_kinds.size()) {
        std::cerr << "FAIL: " << message << " size: got " << commands.size()
            << " expected " << expected_kinds.size() << '\n';
        ok = false;
    }

    const std::size_t count = std::min(commands.size(), expected_kinds.size());
    for (std::size_t i = 0U; i < count; ++i) {
        if (commands[i].kind == expected_kinds[i]) {
            continue;
        }

        std::cerr << "FAIL: " << message << " mismatch at index " << i << '\n';
        ok = false;
    }

    return ok;
}

int action_kind_count(
    const term::Terminal_screen_model_result&  result,
    term::Parser_action_kind                   kind)
{
    int count = 0;
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) == kind) {
            ++count;
        }
    }
    return count;
}

bool test_start_callback_ordering_and_output()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend     = make_session(session);
    backend->outputs_during_start = {
        QByteArrayLiteral("ready"),
        QByteArrayLiteral("prompt> "),
    };

    term::Terminal_launch_config launch_config = valid_launch_config();
    launch_config.identity.term      = QStringLiteral("vnm-test-term");
    launch_config.identity.colorterm = QStringLiteral("vnm-truecolor");

    const term::Terminal_session_result start_result = session->start(launch_config);

    ok &= check(start_result.code == term::Terminal_session_result_code::ACCEPTED,
        "session start accepted");
    ok &= check(backend->start_configs.size() == 1U,
        "session forwards exactly one launch config");
    ok &= check(backend->start_configs.front().argv.size() == 2 &&
        backend->start_configs.front().argv[0] == QStringLiteral("terminal-fixture") &&
        backend->start_configs.front().argv[1] == QStringLiteral("--interactive"),
        "session forwards launch argv vector");
    ok &= check(backend->start_configs.front().working_directory ==
        QStringLiteral("C:/workspace"),
        "session forwards launch working directory");
    ok &= check(backend->start_configs.front().identity.term == QStringLiteral("vnm-test-term") &&
        backend->start_configs.front().identity.colorterm ==
            QStringLiteral("vnm-truecolor"),
        "session forwards terminal identity");
    ok &= check(backend->start_configs.front().initial_grid_size.has_value() &&
        backend->start_configs.front().initial_grid_size->rows == 24 &&
        backend->start_configs.front().initial_grid_size->columns == 80,
        "session forwards initial grid size");
    ok &= check(session->process_state() == term::Terminal_process_state::RUNNING,
        "session enters running state");
    ok &= check(session->backend_ready(), "session marks backend ready");
    ok &= check(session->grid_size().rows == 24 && session->grid_size().columns == 80,
        "session records initial grid size");
    ok &= check(session->processed_commands().size() == 3U,
        "session processes start and synchronous backend output");
    ok &= check(session->processed_commands()[0].kind == term::Terminal_session_command_kind::START,
        "start command stays first");
    ok &= check(session->processed_commands()[1].kind ==
        term::Terminal_session_command_kind::BACKEND_OUTPUT,
        "first backend output follows start");
    ok &= check(session->processed_commands()[2].sequence >
        session->processed_commands()[1].sequence,
        "backend output receives monotonic sequence numbers");
    ok &= check_processed_sequences_are_monotonic(
        *session,
        "start/output processed command stream stays monotonic");
    ok &= check(session->output_chunks().size() == 2U &&
        session->output_chunks()[0] == QByteArrayLiteral("ready") &&
        session->output_chunks()[1] == QByteArrayLiteral("prompt> "),
        "session preserves backend output chunks");
    ok &= check(notification_count(
        *session, term::Terminal_session_notification_kind::PROCESS_STARTED) == 1U,
        "session publishes process-start notification");

    const std::optional<term::Terminal_render_snapshot> snapshot =
        session->latest_render_snapshot();
    ok &= check(snapshot.has_value(),
        "backend output during start publishes a render snapshot");
    if (snapshot.has_value()) {
        ok &= check(snapshot->grid_size.rows == 24 && snapshot->grid_size.columns == 80,
            "startup output snapshot keeps launch grid");
        ok &= check(snapshot_row_text(*snapshot, 0) == QStringLiteral("readyprompt>"),
            "startup output mutates session screen model");
        ok &= check(snapshot->metadata.backend_geometry_in_sync,
            "startup output snapshot records backend geometry sync");
        ok &= check(term::validate_render_snapshot(*snapshot).status ==
            term::Terminal_render_snapshot_status::OK,
            "startup output snapshot validates");
    }
    const std::optional<term::Terminal_screen_model_result> ingest_result =
        session->last_model_ingest_result();
    ok &= check(ingest_result.has_value() &&
        action_kind_count(*ingest_result, term::Parser_action_kind::SCREEN_MUTATION) > 0,
        "startup output records last model ingest result");

    return ok;
}

bool test_backend_output_capture_file()
{
    bool ok = true;

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(), "backend output capture temp dir is valid");
    const QString capture_path = temp_dir.filePath(QStringLiteral("backend-output.raw"));

    term::Terminal_session_config config;
    config.backend_output_capture_path = capture_path;
    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend     = make_session(session, config);
    backend->outputs_during_start = {
        QByteArray("ready\0", 6),
        QByteArrayLiteral("prompt> "),
    };

    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "backend output capture session starts");
    ok &= check(backend->emit_output(QByteArrayLiteral("tail")),
        "backend output capture accepts later output");

    QFile capture_file(capture_path);
    ok &= check(capture_file.open(QIODevice::ReadOnly),
        "backend output capture file opens for verification");
    const QByteArray captured_bytes = capture_file.readAll();
    ok &= check(captured_bytes == QByteArray("ready\0", 6) + QByteArrayLiteral("prompt> tail"),
        "backend output capture preserves raw backend byte stream");

    return ok;
}

bool test_text_area_resize_request_updates_session_grid_in_sequence()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);

    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "text-area resize session starts");
    ok &= check(backend->emit_output(
        QByteArrayLiteral("aa\x1b[8;3;5t\x1b[3;5HZ")),
        "text-area resize output is accepted");

    ok &= check(session->grid_size().rows == 3 && session->grid_size().columns == 5,
        "text-area resize request updates session grid");
    ok &= check(backend->resize_requests.size() == 1U &&
        backend->resize_requests.back().grid_size.rows == 3 &&
        backend->resize_requests.back().grid_size.columns == 5,
        "text-area resize request reaches backend resize path");

    const std::optional<term::Terminal_render_snapshot> snapshot =
        session->latest_render_snapshot();
    ok &= check(snapshot.has_value(),
        "text-area resize request publishes a snapshot");
    if (snapshot.has_value()) {
        ok &= check(snapshot->grid_size.rows == 3 && snapshot->grid_size.columns == 5,
            "text-area resize snapshot uses requested grid");
        ok &= check(snapshot_row_text(*snapshot, 2) == QStringLiteral("    Z"),
            "text-area resize snapshot applies following output after resize");
        ok &= check(term::validate_render_snapshot(*snapshot).status ==
            term::Terminal_render_snapshot_status::OK,
            "text-area resize snapshot validates");
    }

    const std::vector<term::Terminal_session_notification> resize_requests = notifications_of_kind(
        *session,
        term::Terminal_session_notification_kind::TEXT_AREA_RESIZE_REQUESTED);
    ok &= check(resize_requests.size() == 1U &&
        resize_requests.front().text_area_resize_request.has_value() &&
        resize_requests.front().text_area_resize_request->rows == 3 &&
        resize_requests.front().text_area_resize_request->columns == 5,
        "text-area resize request remains observable as a public notification");

    return ok;
}

bool test_text_area_resize_retry_publishes_geometry_metadata()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);

    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "text-area resize retry session starts");

    backend->fail_resize  = true;
    ok                   &= check(backend->emit_output(QByteArrayLiteral("\x1b[8;3;5t")),
        "failed text-area resize output is accepted");
    ok                   &= check(!session->backend_geometry_in_sync(),
        "failed text-area resize marks backend geometry out of sync");
    const std::optional<term::Terminal_render_snapshot> failed_snapshot =
        session->latest_render_snapshot();
    ok &= check(failed_snapshot.has_value() &&
        !failed_snapshot->metadata.backend_geometry_in_sync,
        "failed text-area resize snapshot records out-of-sync geometry");
    const std::uint64_t failed_generation = session->render_snapshot_generation();
    session->mark_render_snapshot_synced(failed_generation);

    backend->fail_resize = false;
    const std::size_t failed_resize_count = backend->resize_requests.size();
    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[8;3;5t")),
        "same-grid text-area retry output is accepted");
    ok &= check(backend->resize_requests.size() == failed_resize_count + 1U,
        "same-grid text-area retry reaches backend resize path");
    ok &= check(session->backend_geometry_in_sync(),
        "same-grid text-area retry restores backend geometry sync");

    const std::optional<term::Terminal_render_snapshot> retry_snapshot =
        session->latest_render_snapshot();
    ok &= check(session->render_snapshot_generation() > failed_generation,
        "same-grid text-area retry publishes a new render snapshot");
    ok &= check(retry_snapshot.has_value() &&
        retry_snapshot->metadata.backend_geometry_in_sync,
        "same-grid text-area retry snapshot records restored geometry sync");
    ok &= check(retry_snapshot.has_value() && retry_snapshot->dirty_row_ranges.empty(),
        "same-grid text-area retry snapshot has no text dirty rows");

    return ok;
}

bool test_backend_output_capture_records_callback_overflow_bytes()
{
    bool ok = true;

    QTemporaryDir temp_dir;
    ok &= check(temp_dir.isValid(), "overflow capture temp dir is valid");
    const QString capture_path = temp_dir.filePath(QStringLiteral("overflow-output.raw"));

    term::Terminal_session_config config;
    config.backend_output_capture_path             = capture_path;
    config.backend_event_notifier                  = [] {};
    config.output_queue_limits.high_water_bytes    = 100U;
    config.output_queue_limits.hard_limit_bytes    = 8U;
    config.output_queue_limits.high_water_commands = 100U;
    config.output_queue_limits.hard_limit_commands = 100U;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session, config);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "overflow capture session starts");
    ok &= check(backend->emit_output(QByteArrayLiteral("abcd")),
        "overflow capture accepts first output");
    ok &= check(backend->emit_output(QByteArrayLiteral("efgh")),
        "overflow capture accepts second output");
    ok &= check(backend->emit_output(QByteArrayLiteral("ijkl")),
        "overflow capture accepts dropped output callback");

    QFile capture_file(capture_path);
    ok &= check(capture_file.open(QIODevice::ReadOnly),
        "overflow capture file opens for verification");
    ok &= check(capture_file.readAll() == QByteArrayLiteral("abcdefghijkl"),
        "overflow capture records bytes before callback queue drop");

    return ok;
}

bool test_backend_output_updates_latest_render_snapshot()
{
    bool ok = true;

    auto              backend     = std::make_unique<Scripted_backend>();
    Scripted_backend* backend_ptr = backend.get();
    term::Terminal_session session(std::move(backend));

    ok &= check(session.start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "snapshot session starts without trace buffers");
    ok &= check(backend_ptr->emit_output(QByteArrayLiteral("hello")),
        "snapshot fake backend emits output");

    const std::optional<term::Terminal_render_snapshot> snapshot =
        session.latest_render_snapshot();
    ok &= check(snapshot.has_value(),
        "latest render snapshot is runtime state outside trace buffers");
    if (snapshot.has_value()) {
        ok &= check(snapshot->metadata.sequence == session.last_processed_sequence(),
            "latest render snapshot uses backend-output command sequence");
        ok &= check(snapshot->metadata.backend_geometry_in_sync,
            "latest render snapshot records geometry sync state");
        ok &= check(snapshot_row_text(*snapshot, 0) == QStringLiteral("hello"),
            "latest render snapshot contains backend output text");
        ok &= check(term::validate_render_snapshot(*snapshot).status ==
            term::Terminal_render_snapshot_status::OK,
            "latest render snapshot validates");
    }
    ok &= check(session.notifications().empty() &&
        session.output_chunks().empty() &&
        session.processed_commands().empty(),
        "trace buffers remain disabled while runtime snapshot is available");
    ok &= check(session.render_snapshot_generation() == 1U,
        "live render snapshot generation is independent of trace buffers");
    ok &= check(!session.last_model_ingest_result().has_value(),
        "last model ingest result is not captured unless diagnostic capture is enabled");

    return ok;
}

bool test_selection_snapshot_and_visible_text()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "selection session starts");
    ok &= check(backend->emit_output(QByteArrayLiteral("alpha\r\nbeta")),
        "selection backend emits visible rows");

    const std::uint64_t output_generation = session->render_snapshot_generation();
    session->mark_render_snapshot_synced(output_generation);
    const term::Terminal_selection_result empty_text = session->selected_text();
    ok &= check(empty_text.code == term::Terminal_selection_result_code::NO_SELECTION,
        "session selected_text reports no selection before selection starts");

    session->set_selection_range({
        { 0, 1 },
        { 1, 2 },
        term::Terminal_selection_mode::NORMAL,
    });
    const std::optional<term::Terminal_render_snapshot> snapshot =
        session->latest_render_snapshot();
    const term::Terminal_selection_result selected_text = session->selected_text();

    ok &= check(session->has_selection(),
        "session records active selection");
    ok &= check(session->render_snapshot_generation() >= output_generation + 1U,
        "selection publishes a render snapshot");
    ok &= check(snapshot.has_value() &&
        snapshot->selection_spans.size() == 2U &&
        snapshot_has_selection_span(*snapshot, 0, 1, 79) &&
        snapshot_has_selection_span(*snapshot, 1, 0, 2),
        "selection snapshot exposes visible selection spans");
    ok &= check(snapshot.has_value() && snapshot->dirty_row_ranges.empty(),
        "selection-only snapshot does not mark text rows dirty");
    ok &= check(selected_text.code == term::Terminal_selection_result_code::OK &&
        selected_text.text == QStringLiteral("lpha\nbe"),
        "session extracts selected visible text in row order");

    std::unique_ptr<term::Terminal_session> partial_space_session;
    Scripted_backend* partial_space_backend = make_session(partial_space_session);
    ok &= check(partial_space_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "Phase 4 bounded-space selection session starts");
    ok &= check(partial_space_backend->emit_output(QByteArrayLiteral("A B")),
        "Phase 4 bounded-space backend emits visible text");
    partial_space_session->set_selection_range({
        { 0, 0 },
        { 0, 2 },
        term::Terminal_selection_mode::NORMAL,
    });
    const term::Terminal_selection_result partial_space_payload =
        partial_space_session->selected_text();
    ok &= check(partial_space_payload.code == term::Terminal_selection_result_code::OK &&
        partial_space_payload.text == QStringLiteral("A "),
        "Phase 4 snapshot payload preserves bounded trailing space");

    const std::uint64_t selected_generation = session->render_snapshot_generation();
    session->set_selection_range({
        { 0, 1 },
        { 1, 2 },
        term::Terminal_selection_mode::NORMAL,
    });
    ok &= check(session->render_snapshot_generation() == selected_generation,
        "setting the same selection range does not publish a redundant snapshot");

    std::unique_ptr<term::Terminal_session> payload_session;
    Scripted_backend* payload_backend = make_session(payload_session);
    ok &= check(payload_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "payload selection session starts");
    ok &= check(payload_backend->emit_output(QByteArrayLiteral("original")),
        "payload backend emits original row text");
    payload_session->set_selection_range({
        { 0, 0 },
        { 0, 8 },
        term::Terminal_selection_mode::NORMAL,
    });
    const term::Terminal_selection_result original_payload =
        payload_session->selected_text();
    ok &= check(original_payload.code == term::Terminal_selection_result_code::OK &&
        original_payload.text == QStringLiteral("original"),
        "finalized selection captures the original payload");
    ok &= check(payload_backend->emit_output(QByteArrayLiteral("\rmutated!")),
        "payload backend rewrites selected row text");
    const term::Terminal_selection_result retained_payload =
        payload_session->selected_text();
    ok &= check(retained_payload.code == term::Terminal_selection_result_code::OK &&
        retained_payload.text == QStringLiteral("original"),
        "selected_text keeps finalized payload after backing row mutates");
    payload_session->clear_selection();
    ok &= check(payload_session->selected_text().code ==
        term::Terminal_selection_result_code::NO_SELECTION,
        "clear_selection clears the finalized payload");
    payload_session->set_selection_range({
        { 0, 0 },
        { 0, 8 },
        term::Terminal_selection_mode::NORMAL,
    });
    const term::Terminal_selection_result refreshed_payload =
        payload_session->selected_text();
    ok &= check(refreshed_payload.code == term::Terminal_selection_result_code::OK &&
        refreshed_payload.text == QStringLiteral("mutated!"),
        "new selection captures replacement payload");

    std::unique_ptr<term::Terminal_session> sync_payload_session;
    Scripted_backend* sync_payload_backend = make_session(sync_payload_session);
    ok &= check(sync_payload_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "synchronized payload selection session starts");
    ok &= check(sync_payload_backend->emit_output(QByteArrayLiteral("original")),
        "synchronized payload backend emits original row text");
    sync_payload_session->set_selection_range({
        { 0, 0 },
        { 0, 8 },
        term::Terminal_selection_mode::NORMAL,
    });
    ok &= check(sync_payload_backend->emit_output(QByteArrayLiteral("\x1b[?2026h\rmutated!")),
        "synchronized payload backend mutates selected row while held");
    const term::Terminal_selection_result held_payload =
        sync_payload_session->selected_text();
    ok &= check(held_payload.code == term::Terminal_selection_result_code::OK &&
        held_payload.text == QStringLiteral("original"),
        "selected_text keeps session payload while synchronized output is held");

    session->clear_selection();
    const std::optional<term::Terminal_render_snapshot> cleared_snapshot =
        session->latest_render_snapshot();
    ok &= check(!session->has_selection(),
        "session clear_selection removes active selection");
    ok &= check(cleared_snapshot.has_value() &&
        cleared_snapshot->selection_spans.empty() &&
        cleared_snapshot->dirty_row_ranges.empty(),
        "cleared selection publishes snapshot without selection spans");

    session->set_selection_range({
        { 0, 0 },
        { 0, 2 },
        term::Terminal_selection_mode::NORMAL,
    });
    ok &= check(session->has_selection(),
        "session accepts a fresh selection before invalid input");
    session->set_selection_range({
        { 0, -1 },
        { 0, 2 },
        term::Terminal_selection_mode::NORMAL,
    });
    const std::optional<term::Terminal_render_snapshot> invalid_snapshot =
        session->latest_render_snapshot();
    ok &= check(!session->has_selection(),
        "invalid selection columns clear the active selection");
    ok &= check(invalid_snapshot.has_value() && invalid_snapshot->selection_spans.empty(),
        "invalid selection columns publish a cleared selection snapshot");

    session->set_selection_range({
        { 999999, 0 },
        { 999999, 2 },
        term::Terminal_selection_mode::NORMAL,
    });
    ok &= check(!session->has_selection(),
        "invalid selection rows are rejected without activating selection");

    std::unique_ptr<term::Terminal_session> wide_session;
    Scripted_backend* wide_backend = make_session(wide_session);
    ok &= check(wide_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "wide selection session starts");
    ok &= check(wide_backend->emit_output(
        QByteArrayLiteral("A") + QStringLiteral("\u754c").toUtf8() +
        QByteArrayLiteral("B")),
        "wide selection backend emits wide glyph text");
    wide_session->set_selection_range({
        { 0, 0 },
        { 0, 4 },
        term::Terminal_selection_mode::NORMAL,
    });
    const term::Terminal_selection_result wide_text = wide_session->selected_text();
    ok &= check(wide_text.code == term::Terminal_selection_result_code::OK &&
        wide_text.text == QStringLiteral("A\u754c" "B"),
        "wide selection text includes the base glyph once");

    std::unique_ptr<term::Terminal_session> detached_session;
    Scripted_backend* detached_backend = make_session(detached_session);
    ok &= check(detached_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "detached selection session starts");
    ok &= check(detached_backend->emit_output(numbered_scroll_lines(80)),
        "detached selection backend creates scrollback");
    ok &= check(detached_session->scroll_viewport_lines(3).action ==
        term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "detached selection viewport scrolls into scrollback");
    const std::optional<term::Terminal_render_snapshot> detached_snapshot =
        detached_session->latest_render_snapshot();
    if (detached_snapshot.has_value()) {
        const int first_visible_logical_row =
            detached_snapshot->viewport.scrollback_rows -
            detached_snapshot->viewport.offset_from_tail;
        const int selection_row = first_visible_logical_row + 3;
        const QString expected_text =
            QStringLiteral("scroll-line-%1")
                .arg(selection_row, 3, 10, QLatin1Char('0'));
        detached_session->set_selection_range({
            { selection_row, 0 },
            { selection_row, 15 },
            term::Terminal_selection_mode::NORMAL,
        });
        const term::Terminal_selection_result detached_text =
            detached_session->selected_text();
        const std::optional<term::Terminal_render_snapshot> selected_detached_snapshot =
            detached_session->latest_render_snapshot();
        ok &= check(detached_text.code == term::Terminal_selection_result_code::OK &&
            detached_text.text == expected_text,
            "detached viewport selection extracts text from visible scrollback");
        ok &= check(selected_detached_snapshot.has_value() &&
            snapshot_has_selection_span(*selected_detached_snapshot, 3, 0, 15),
            "detached viewport selection publishes span at the visible row");
        ok &= check(detached_session->scroll_viewport_lines(-3).action ==
            term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
            "detached selection viewport can return to tail after selection");
        const term::Terminal_selection_result offscreen_detached_text =
            detached_session->selected_text();
        ok &= check(offscreen_detached_text.code == term::Terminal_selection_result_code::OK &&
            offscreen_detached_text.text == expected_text,
            "selected_text extracts retained logical text after viewport moves away");
    }
    else {
        ok &= check(false, "detached selection snapshot is available");
    }

    std::unique_ptr<term::Terminal_session> boundary_session;
    Scripted_backend* boundary_backend = make_session(boundary_session);
    ok &= check(boundary_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "scrollback-boundary selection session starts");
    ok &= check(boundary_backend->emit_output(numbered_scroll_lines(80)),
        "scrollback-boundary backend creates scrollback");
    ok &= check(boundary_session->scroll_viewport_lines(1).action ==
        term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "scrollback-boundary viewport exposes the last scrollback row");
    const std::optional<term::Terminal_render_snapshot> boundary_snapshot =
        boundary_session->latest_render_snapshot();
    if (boundary_snapshot.has_value()) {
        const int scrollback_rows = boundary_snapshot->viewport.scrollback_rows;
        const int tail_columns    = boundary_snapshot->grid_size.columns;
        boundary_session->set_selection_range({
            { scrollback_rows - 1, 0 },
            { scrollback_rows, 15 },
            term::Terminal_selection_mode::NORMAL,
        });
        const term::Terminal_selection_result boundary_text =
            boundary_session->selected_text();
        const std::optional<term::Terminal_render_snapshot> selected_boundary_snapshot =
            boundary_session->latest_render_snapshot();
        const QString expected_text =
            QStringLiteral("scroll-line-%1\nscroll-line-%2")
                .arg(scrollback_rows - 1, 3, 10, QLatin1Char('0'))
                .arg(scrollback_rows, 3, 10, QLatin1Char('0'));
        ok &= check(boundary_text.code == term::Terminal_selection_result_code::OK &&
            boundary_text.text == expected_text,
            "selection can extract text across the scrollback/live-screen boundary");
        ok &= check(selected_boundary_snapshot.has_value() &&
            selected_boundary_snapshot->selection_spans.size() == 2U &&
            snapshot_has_selection_span(*selected_boundary_snapshot, 0, 0, tail_columns) &&
            snapshot_has_selection_span(*selected_boundary_snapshot, 1, 0, 15),
            "boundary selection publishes spans on both sides of the boundary");
    }
    else {
        ok &= check(false, "scrollback-boundary snapshot is available");
    }

    std::unique_ptr<term::Terminal_session> alternate_session;
    Scripted_backend* alternate_backend = make_session(alternate_session);
    ok &= check(alternate_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "alternate selection session starts");
    ok &= check(alternate_backend->emit_output(QByteArrayLiteral("primary")),
        "alternate selection backend emits primary text");
    alternate_session->set_selection_range({
        { 0, 0 },
        { 0, 4 },
        term::Terminal_selection_mode::NORMAL,
    });
    ok &= check(alternate_session->has_selection(),
        "alternate selection session records primary selection");
    ok &= check(alternate_backend->emit_output(QByteArrayLiteral("\x1b[?1049halternate")),
        "alternate selection backend enters alternate buffer");
    const std::optional<term::Terminal_render_snapshot> alternate_snapshot =
        alternate_session->latest_render_snapshot();
    const term::Terminal_selection_result alternate_retained_text =
        alternate_session->selected_text();
    ok &= check(alternate_session->has_selection(),
        "active buffer transition retains primary finalized selection payload");
    ok &= check(alternate_snapshot.has_value() &&
        alternate_snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE &&
        alternate_snapshot->selection_spans.empty(),
        "alternate buffer snapshot does not project primary selection spans");
    ok &= check(alternate_retained_text.code == term::Terminal_selection_result_code::OK &&
        alternate_retained_text.text == QStringLiteral("prim"),
        "selected_text reports retained payload after active buffer transition");

    std::unique_ptr<term::Terminal_session> alternate_eviction_session;
    Scripted_backend* alternate_eviction_backend = make_session(alternate_eviction_session);
    ok &= check(alternate_eviction_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "alternate eviction selection session starts");
    ok &= check(alternate_eviction_backend->emit_output(numbered_scroll_lines(80)),
        "alternate eviction backend creates primary scrollback");
    ok &= check(alternate_eviction_backend->emit_output(
        QByteArrayLiteral("\x1b[?1049halternate")),
        "alternate eviction backend enters alternate buffer");
    alternate_eviction_session->set_selection_range({
        { 0, 0 },
        { 0, 9 },
        term::Terminal_selection_mode::NORMAL,
    });
    ok &= check(alternate_eviction_session->has_selection(),
        "alternate eviction session records alternate-buffer selection");
    alternate_eviction_session->set_scrollback_limit(2);
    const term::Terminal_selection_result alternate_eviction_text =
        alternate_eviction_session->selected_text();
    ok &= check(alternate_eviction_session->has_selection() &&
        alternate_eviction_text.code == term::Terminal_selection_result_code::OK &&
        alternate_eviction_text.text == QStringLiteral("alternate"),
        "primary scrollback eviction does not affect alternate-buffer selection");
    ok &= check(alternate_eviction_backend->emit_output(QByteArrayLiteral("\x1b[?1049l")),
        "alternate eviction backend leaves alternate buffer");
    const std::optional<term::Terminal_render_snapshot> leave_alternate_snapshot =
        alternate_eviction_session->latest_render_snapshot();
    const term::Terminal_selection_result leave_alternate_text =
        alternate_eviction_session->selected_text();
    ok &= check(alternate_eviction_session->has_selection(),
        "alternate-to-primary transition retains alternate-buffer payload");
    ok &= check(leave_alternate_snapshot.has_value() &&
        leave_alternate_snapshot->viewport.active_buffer ==
            term::Terminal_buffer_id::PRIMARY &&
        leave_alternate_snapshot->selection_spans.empty(),
        "primary snapshot does not project alternate-buffer selection spans");
    ok &= check(leave_alternate_text.code == term::Terminal_selection_result_code::OK &&
        leave_alternate_text.text == QStringLiteral("alternate"),
        "selected_text reports retained alternate-buffer payload");

    std::unique_ptr<term::Terminal_session> eviction_session;
    Scripted_backend* eviction_backend = make_session(eviction_session);
    ok &= check(eviction_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "eviction selection session starts");
    ok &= check(eviction_backend->emit_output(numbered_scroll_lines(80)),
        "eviction selection backend creates scrollback");
    const std::optional<term::terminal_selection_source_identity_t> eviction_source =
        eviction_session->published_selection_source_identity();
    ok &= check(eviction_source.has_value(),
        "Phase 4 offscreen scrollback selection has a published source identity");
    eviction_session->set_selection_range_from_published_source({
        { 0, 0 },
        { 2, 15 },
        term::Terminal_selection_mode::NORMAL,
    }, eviction_source.value_or(term::terminal_selection_source_identity_t{}));
    const term::Terminal_selection_result offscreen_scrollback_payload =
        eviction_session->selected_text();
    ok &= check(offscreen_scrollback_payload.code == term::Terminal_selection_result_code::OK &&
        offscreen_scrollback_payload.text ==
            QStringLiteral("scroll-line-000\nscroll-line-001\nscroll-line-002"),
        "Phase 4 offscreen scrollback payload is complete instead of visible-snapshot-truncated");
    eviction_session->clear_selection();
    eviction_session->set_selection_range({
        { 0, 0 },
        { 0, 5 },
        term::Terminal_selection_mode::NORMAL,
    });
    ok &= check(eviction_session->has_selection(),
        "eviction selection session records scrollback selection");
    const term::Terminal_selection_result eviction_original_payload =
        eviction_session->selected_text();
    ok &= check(eviction_original_payload.code == term::Terminal_selection_result_code::OK &&
        eviction_original_payload.text == QStringLiteral("scrol"),
        "eviction selection captures exact finalized offscreen payload before eviction");
    eviction_session->set_scrollback_limit(2);
    const std::optional<term::Terminal_render_snapshot> eviction_snapshot =
        eviction_session->latest_render_snapshot();
    const term::Terminal_selection_result eviction_payload =
        eviction_session->selected_text();
    ok &= check(eviction_session->has_selection(),
        "scrollback eviction retains finalized selection payload");
    ok &= check(eviction_payload.code == term::Terminal_selection_result_code::OK &&
        eviction_payload.text == eviction_original_payload.text,
        "scrollback eviction preserves finalized selected text");
    ok &= check(eviction_snapshot.has_value() && eviction_snapshot->selection_spans.empty(),
        "scrollback eviction publishes snapshot without stale selection spans");
    ok &= check(!eviction_session->selection_visual_lease().has_value(),
        "scrollback eviction clears retained-line visual proof");

    term::Terminal_session_config zero_scrollback_config;
    zero_scrollback_config.scrollback_limit = 0;
    std::unique_ptr<term::Terminal_session> zero_scrollback_session;
    Scripted_backend* zero_scrollback_backend =
        make_session(zero_scrollback_session, zero_scrollback_config);
    ok &= check(zero_scrollback_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "zero-scrollback selection session starts");
    ok &= check(zero_scrollback_backend->emit_output(numbered_scroll_lines(24)),
        "zero-scrollback selection backend fills the screen");
    zero_scrollback_session->set_selection_range({
        { 0, 0 },
        { 0, 5 },
        term::Terminal_selection_mode::NORMAL,
    });
    ok &= check(zero_scrollback_session->has_selection(),
        "zero-scrollback session records a visible selection");
    const term::Terminal_selection_result zero_scrollback_original_payload =
        zero_scrollback_session->selected_text();
    ok &= check(zero_scrollback_original_payload.code ==
        term::Terminal_selection_result_code::OK,
        "zero-scrollback session captures finalized payload before live scroll");
    ok &= check(zero_scrollback_backend->emit_output(QByteArrayLiteral("extra\r\n")),
        "zero-scrollback backend scrolls without retaining scrollback");
    const std::optional<term::Terminal_render_snapshot> zero_scrollback_snapshot =
        zero_scrollback_session->latest_render_snapshot();
    const term::Terminal_selection_result zero_scrollback_payload =
        zero_scrollback_session->selected_text();
    ok &= check(zero_scrollback_session->has_selection(),
        "zero-scrollback live scroll retains finalized selection payload");
    ok &= check(zero_scrollback_payload.code == term::Terminal_selection_result_code::OK &&
        zero_scrollback_payload.text == zero_scrollback_original_payload.text,
        "zero-scrollback live scroll preserves finalized selected text");
    ok &= check(zero_scrollback_snapshot.has_value() &&
        zero_scrollback_snapshot->selection_spans.empty(),
        "zero-scrollback live scroll publishes no stale selection spans");
    ok &= check(!zero_scrollback_session->selection_visual_lease().has_value(),
        "zero-scrollback live scroll clears retained-line visual proof");

    std::unique_ptr<term::Terminal_session> sync_session;
    Scripted_backend* sync_backend = make_session(sync_session);
    ok &= check(sync_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "synchronized selection session starts");
    ok &= check(sync_backend->emit_output(QByteArrayLiteral("alpha")),
        "synchronized selection backend emits visible text");
    sync_session->set_selection_range({
        { 0, 0 },
        { 0, 5 },
        term::Terminal_selection_mode::NORMAL,
    });
    const std::uint64_t sync_selected_generation =
        sync_session->render_snapshot_generation();
    ok &= check(sync_backend->emit_output(QByteArrayLiteral("\x1b[?2026hheld")),
        "synchronized selection backend enters synchronized output");
    sync_session->clear_selection();
    ok &= check(sync_session->render_snapshot_generation() == sync_selected_generation,
        "selection clear during synchronized output is deferred");
    ok &= check(sync_backend->emit_output(QByteArrayLiteral("\x1b[?2026l")),
        "synchronized selection backend releases synchronized output");
    const std::optional<term::Terminal_render_snapshot> sync_snapshot =
        sync_session->latest_render_snapshot();
    ok &= check(!sync_session->has_selection(),
        "deferred synchronized selection clear updates session state");
    ok &= check(sync_snapshot.has_value() &&
        sync_snapshot->selection_spans.empty(),
        "synchronized selection clear releases without stale selection spans");

    std::unique_ptr<term::Terminal_session> sync_set_session;
    Scripted_backend* sync_set_backend = make_session(sync_set_session);
    ok &= check(sync_set_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "synchronized selection set session starts");
    ok &= check(sync_set_backend->emit_output(QByteArrayLiteral("alpha")),
        "synchronized selection set backend emits visible text");
    const std::uint64_t sync_set_base_generation =
        sync_set_session->render_snapshot_generation();
    ok &= check(sync_set_backend->emit_output(QByteArrayLiteral("\x1b[?2026h")),
        "synchronized selection set backend enters synchronized output");
    sync_set_session->set_selection_range({
        { 0, 1 },
        { 0, 4 },
        term::Terminal_selection_mode::NORMAL,
    });
    ok &= check(sync_set_session->render_snapshot_generation() == sync_set_base_generation,
        "selection set during synchronized output is deferred");
    ok &= check(sync_set_backend->emit_output(QByteArrayLiteral("\x1b[?2026l")),
        "synchronized selection set backend releases synchronized output");
    const std::optional<term::Terminal_render_snapshot> sync_set_snapshot =
        sync_set_session->latest_render_snapshot();
    ok &= check(sync_set_snapshot.has_value() &&
        snapshot_has_selection_span(*sync_set_snapshot, 0, 1, 3),
        "synchronized selection set releases with the deferred selection span");

    std::unique_ptr<term::Terminal_session> sync_source_session;
    Scripted_backend* sync_source_backend = make_session(sync_source_session);
    ok &= check(sync_source_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "Phase 4 synchronized payload-source session starts");
    ok &= check(sync_source_backend->emit_output(QByteArrayLiteral("alpha")),
        "Phase 4 synchronized payload-source backend emits visible text");
    const std::optional<term::terminal_selection_source_identity_t> visible_source =
        sync_source_session->published_selection_source_identity();
    ok &= check(visible_source.has_value(),
        "Phase 4 visible published source identity is available");
    ok &= check(sync_source_backend->emit_output(QByteArrayLiteral("\x1b[?2026h\rbravo")),
        "Phase 4 synchronized payload-source backend mutates hidden text");
    sync_source_session->set_selection_range_from_published_source({
        { 0, 0 },
        { 0, 5 },
        term::Terminal_selection_mode::NORMAL,
    }, visible_source.value_or(term::terminal_selection_source_identity_t{}));
    const term::Terminal_selection_result hidden_source_payload =
        sync_source_session->selected_text();
    ok &= check(hidden_source_payload.code == term::Terminal_selection_result_code::OK &&
        hidden_source_payload.text == QStringLiteral("alpha"),
        "Phase 4 selection payload is extracted from the published source, not hidden synchronized state");
    ok &= check(sync_source_backend->emit_output(QByteArrayLiteral("\x1b[?2026l")),
        "Phase 4 synchronized payload-source backend releases hidden text");
    const std::optional<term::Terminal_render_snapshot> released_source_snapshot =
        sync_source_session->latest_render_snapshot();
    const term::Terminal_selection_result released_source_payload =
        sync_source_session->selected_text();
    ok &= check(released_source_snapshot.has_value() &&
        snapshot_row_text(*released_source_snapshot, 0) == QStringLiteral("bravo"),
        "Phase 4 synchronized payload-source release publishes hidden replacement text");
    ok &= check(released_source_payload.code == term::Terminal_selection_result_code::OK &&
        released_source_payload.text == QStringLiteral("alpha"),
        "Phase 4 synchronized payload-source release retains the published-source payload");

    std::unique_ptr<term::Terminal_session> hidden_offscreen_session;
    Scripted_backend* hidden_offscreen_backend = make_session(hidden_offscreen_session);
    ok &= check(hidden_offscreen_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "Phase 4 hidden offscreen selection session starts");
    ok &= check(hidden_offscreen_backend->emit_output(numbered_scroll_lines(80)),
        "Phase 4 hidden offscreen backend creates scrollback");
    ok &= check(hidden_offscreen_backend->emit_output(QByteArrayLiteral("\x1b[?2026hhidden")),
        "Phase 4 hidden offscreen backend enters synchronized output with hidden text");
    hidden_offscreen_session->set_selection_range({
        { 0, 0 },
        { 0, 5 },
        term::Terminal_selection_mode::NORMAL,
    });
    const term::Terminal_selection_result hidden_offscreen_payload =
        hidden_offscreen_session->selected_text();
    ok &= check(hidden_offscreen_payload.code == term::Terminal_selection_result_code::NO_SELECTION,
        "Phase 4 hidden synchronized output blocks offscreen model fallback");

    return ok;
}

bool test_selection_snapshot_during_blocked_publication()
{
    bool ok = true;

    {
        std::unique_ptr<term::Terminal_session> session;
        Scripted_backend* backend = make_session(session);
        ok &= check(session->start(valid_launch_config()).code ==
            term::Terminal_session_result_code::ACCEPTED,
            "blocked detach session starts");
        ok &= check(backend->emit_output(QByteArrayLiteral("visible")),
            "blocked detach backend emits visible text");
        session->set_selection_range({
            { 0, 0 },
            { 0, 4 },
            term::Terminal_selection_mode::NORMAL,
        });
        const std::uint64_t selected_generation =
            session->render_snapshot_generation();
        ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026hheld")),
            "blocked detach backend holds synchronized output");
        ok &= check(session->render_snapshot_generation() == selected_generation,
            "blocked detach held output does not publish content");

        session->detach_selection_visual_attachment();
        const std::optional<term::Terminal_render_snapshot> detached_snapshot =
            session->latest_render_snapshot();
        const term::Terminal_selection_result detached_payload =
            session->selected_text();
        ok &= check(session->render_snapshot_generation() == selected_generation + 1U,
            "blocked detach publishes a selection-only snapshot");
        ok &= check(detached_snapshot.has_value() &&
            snapshot_contains_text(*detached_snapshot, QStringLiteral("visible")) &&
            !snapshot_contains_text(*detached_snapshot, QStringLiteral("held")) &&
            detached_snapshot->selection_spans.empty(),
            "blocked detach clears public spans without publishing held content");
        ok &= check(detached_payload.code == term::Terminal_selection_result_code::OK &&
            detached_payload.text == QStringLiteral("visi"),
            "blocked detach preserves selected payload");
    }

    {
        std::unique_ptr<term::Terminal_session> session;
        Scripted_backend* backend = make_session(session);
        ok &= check(session->start(valid_launch_config()).code ==
            term::Terminal_session_result_code::ACCEPTED,
            "blocked clear session starts");
        ok &= check(backend->emit_output(QByteArrayLiteral("alpha")),
            "blocked clear backend emits visible text");
        session->set_selection_range({
            { 0, 0 },
            { 0, 5 },
            term::Terminal_selection_mode::NORMAL,
        });
        const std::uint64_t selected_generation =
            session->render_snapshot_generation();
        ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026hheld")),
            "blocked clear backend holds synchronized output");
        session->clear_selection();
        ok &= check(session->render_snapshot_generation() == selected_generation,
            "blocked clear defers snapshot publication");
        ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026l")),
            "blocked clear backend releases synchronized output");
        const std::optional<term::Terminal_render_snapshot> released_snapshot =
            session->latest_render_snapshot();
        ok &= check(!session->has_selection(),
            "blocked clear updates internal selection state");
        ok &= check(released_snapshot.has_value() &&
            snapshot_contains_text(*released_snapshot, QStringLiteral("held")) &&
            released_snapshot->selection_spans.empty(),
            "blocked clear publishes deferred clear on release");
    }

    {
        std::unique_ptr<term::Terminal_session> session;
        Scripted_backend* backend = make_session(session);
        ok &= check(session->start(valid_launch_config()).code ==
            term::Terminal_session_result_code::ACCEPTED,
            "blocked set session starts");
        ok &= check(backend->emit_output(QByteArrayLiteral("alpha")),
            "blocked set backend emits visible text");
        const std::uint64_t visible_generation =
            session->render_snapshot_generation();
        ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026h")),
            "blocked set backend holds synchronized output");
        session->set_selection_range({
            { 0, 1 },
            { 0, 4 },
            term::Terminal_selection_mode::NORMAL,
        });
        ok &= check(session->render_snapshot_generation() == visible_generation,
            "blocked set defers snapshot publication");
        ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026l")),
            "blocked set backend releases synchronized output");
        const std::optional<term::Terminal_render_snapshot> released_snapshot =
            session->latest_render_snapshot();
        ok &= check(released_snapshot.has_value() &&
            snapshot_has_selection_span(*released_snapshot, 0, 1, 3),
            "blocked set publishes deferred selection on release");
    }

    return ok;
}

bool test_blocked_detach_resize_snapshot_metadata_coherence()
{
    bool ok = true;

    term::Terminal_session_config config;
    config.scrollback_limit = 0;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session, config);
    term::Terminal_launch_config launch_config = valid_launch_config();
    launch_config.initial_grid_size = {3, 10};
    ok &= check(session->start(launch_config).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "blocked detach resize metadata session starts");
    ok &= check(backend->emit_output(QByteArrayLiteral("visible")),
        "blocked detach resize backend emits visible text");
    session->set_selection_range({
        { 0, 0 },
        { 0, 4 },
        term::Terminal_selection_mode::NORMAL,
    });
    const std::optional<term::Terminal_render_snapshot> selected_snapshot =
        session->latest_render_snapshot();
    const std::uint64_t selected_generation =
        session->render_snapshot_generation();

    QByteArray held_output = QByteArrayLiteral("\x1b[?2026h");
    held_output += numbered_scroll_lines(8);
    ok &= check(backend->emit_output(std::move(held_output)),
        "blocked detach resize backend emits held scrollback churn");
    ok &= check(session->render_snapshot_generation() == selected_generation,
        "held scrollback churn stays unpublished before resize");

    const term::Terminal_session_result resize_result =
        session->resize(QSizeF(120.0, 80.0), {4, 12});
    const std::optional<term::Terminal_render_snapshot> resize_snapshot =
        session->latest_render_snapshot();
    ok &= check(resize_result.code == term::Terminal_session_result_code::ACCEPTED,
        "blocked detach resize is accepted");
    ok &= check(resize_snapshot.has_value()             &&
        resize_snapshot->grid_size.rows    == 4         &&
        resize_snapshot->grid_size.columns == 12        &&
        snapshot_cells_fit_grid(*resize_snapshot),
        "synchronized resize snapshot publishes current geometry");
    ok &= check(selected_snapshot.has_value() && resize_snapshot.has_value() &&
        resize_snapshot->metadata.row_origin_generation >
            selected_snapshot->metadata.row_origin_generation,
        "synchronized resize snapshot includes deferred row-origin churn");

    session->detach_selection_visual_attachment();
    const std::optional<term::Terminal_render_snapshot> detached_snapshot =
        session->latest_render_snapshot();
    ok &= check(detached_snapshot.has_value()             &&
        detached_snapshot->grid_size.rows    == 4         &&
        detached_snapshot->grid_size.columns == 12        &&
        snapshot_cells_fit_grid(*detached_snapshot)       &&
        detached_snapshot->selection_spans.empty()        &&
        !snapshot_contains_text(*detached_snapshot, QStringLiteral("scroll-line")),
        "blocked detach after synchronized resize preserves current public geometry only");
    ok &= check(resize_snapshot.has_value() && detached_snapshot.has_value() &&
        detached_snapshot->metadata.row_origin_generation ==
            resize_snapshot->metadata.row_origin_generation,
        "blocked detach after synchronized resize stamps current row-origin metadata");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026l")),
        "blocked detach resize backend releases synchronized output");
    const std::optional<term::Terminal_render_snapshot> released_snapshot =
        session->latest_render_snapshot();
    ok &= check(detached_snapshot.has_value() && released_snapshot.has_value() &&
        released_snapshot->metadata.row_origin_generation ==
            detached_snapshot->metadata.row_origin_generation,
        "synchronized release does not reapply row-origin churn consumed by resize");

    return ok;
}

term::terminal_selection_visual_lease_t make_phase2_selection_lease(
    term::Terminal_selection_range range)
{
    term::terminal_selection_visual_lease_t lease;
    lease.source_content_basis = {7U, 3U};
    lease.session_epoch        = 11U;
    lease.buffer_id            = term::Terminal_buffer_id::PRIMARY;
    lease.grid_reflow_basis    = 3U;
    lease.grid_size            = {24, 80};
    lease.viewport_mapping     = {
        term::Terminal_buffer_id::PRIMARY,
        6,
        24,
        2,
        false,
        term::Terminal_alternate_screen_scroll_policy::KEEP_AT_TAIL,
    };
    lease.selected_range = range;
    lease.anchor         = range.start;
    lease.extent         = range.end;
    lease.selected_lines = {{
        0,
        101U,
        13U,
    }};
    return lease;
}

bool test_selection_phase2_internal_state_and_lease()
{
    bool ok = true;

    term::Selection_contract_controller selection;
    ok &= check(selection.internal_state() ==
            term::Terminal_selection_internal_state::NONE &&
        !selection.has_selection(),
        "Phase 2 selection starts with internal NONE state");

    selection.begin({1, 2});
    ok &= check(selection.internal_state() ==
            term::Terminal_selection_internal_state::DRAG_ARMED &&
        !selection.has_selection() &&
        selection.has_internal_selection() &&
        selection.provisional_payload_identity() != 0U &&
        selection.durable_payload_identity() == 0U,
        "Phase 2 drag begin arms a non-copyable provisional selection and clears durable payload");

    selection.extend({1, 6});
    const std::vector<QString> rows = {
        QStringLiteral("zero"),
        QStringLiteral("abcdefgh"),
    };
    const term::Terminal_selection_result preview_text =
        selection.selected_text(std::span<const QString>(rows));
    ok &= check(selection.internal_state() ==
            term::Terminal_selection_internal_state::DRAG_PREVIEW &&
        preview_text.code == term::Terminal_selection_result_code::OK &&
        preview_text.text == QStringLiteral("cdef"),
        "Phase 2 drag extend records DRAG_PREVIEW without durable payload storage");

    const term::Terminal_selection_range range = {
        {1, 2},
        {1, 6},
        term::Terminal_selection_mode::NORMAL,
    };
    selection.set_range(range, QStringLiteral("cdef"), make_phase2_selection_lease(range));
    const std::uint64_t durable_identity = selection.durable_payload_identity();
    const term::Terminal_selection_result durable_text = selection.selected_text();
    ok &= check(selection.internal_state() ==
            term::Terminal_selection_internal_state::ATTACHED_VISIBLE &&
        durable_identity != 0U &&
        selection.provisional_payload_identity() == 0U &&
        durable_text.code == term::Terminal_selection_result_code::OK &&
        durable_text.text == QStringLiteral("cdef"),
        "Phase 2 finalize stores durable payload separately from provisional drag state");

    const std::optional<term::terminal_selection_visual_lease_t>& lease =
        selection.visual_lease();
    ok &= check(lease.has_value() &&
        lease->source_content_basis == term::terminal_selection_content_basis_t{7U, 3U} &&
        lease->session_epoch == 11U &&
        lease->buffer_id == term::Terminal_buffer_id::PRIMARY &&
        lease->grid_reflow_basis == 3U &&
        lease->grid_size.rows == 24 &&
        lease->grid_size.columns == 80 &&
        lease->viewport_mapping.offset_from_tail == 2 &&
        lease->selected_range == range &&
        lease->anchor == range.start &&
        lease->extent == range.end &&
        lease->selected_lines == make_phase2_selection_lease(range).selected_lines &&
        lease->durable_payload_identity == durable_identity,
        "Phase 2 visual lease records content basis, grid, viewport, endpoints, and payload identity");

    selection.hide_visual_attachment();
    ok &= check(selection.internal_state() ==
            term::Terminal_selection_internal_state::ATTACHED_HIDDEN &&
        selection.visual_lease().has_value() &&
        selection.selected_text().text == QStringLiteral("cdef"),
        "Phase 2 hidden attachment keeps the lease and durable payload");

    selection.detach_visual_attachment();
    ok &= check(selection.internal_state() ==
            term::Terminal_selection_internal_state::PAYLOAD_ONLY &&
        !selection.visual_lease().has_value() &&
        selection.has_selection() &&
        selection.selected_text().text == QStringLiteral("cdef"),
        "Phase 2 visual detach keeps copyability as PAYLOAD_ONLY");

    selection.clear();
    ok &= check(selection.internal_state() ==
            term::Terminal_selection_internal_state::NONE &&
        !selection.has_selection() &&
        selection.selected_text().code == term::Terminal_selection_result_code::NO_SELECTION,
        "Phase 2 clear removes payload and visual lease data");

    return ok;
}

bool test_selection_phase2_replacement_empty_cancel_and_eviction()
{
    bool ok = true;

    term::Selection_contract_controller selection;
    const term::Terminal_selection_range original_range = {
        {2, 0},
        {2, 5},
        term::Terminal_selection_mode::NORMAL,
    };
    selection.set_range(
        original_range,
        QStringLiteral("alpha"),
        make_phase2_selection_lease(original_range));
    const std::uint64_t original_identity = selection.durable_payload_identity();

    selection.begin({3, 1});
    ok &= check(selection.internal_state() ==
            term::Terminal_selection_internal_state::DRAG_ARMED &&
        !selection.has_selection() &&
        selection.has_internal_selection() &&
        selection.durable_payload_identity() == 0U &&
        selection.selected_text().code == term::Terminal_selection_result_code::INVALID_RANGE,
        "Phase 2 replacement mouse-down clears the old durable payload immediately");

    selection.clear();
    ok &= check(selection.internal_state() ==
            term::Terminal_selection_internal_state::NONE &&
        selection.selected_text().code == term::Terminal_selection_result_code::NO_SELECTION,
        "Phase 2 cancelled replacement returns to NONE without restoring old payload");

    const term::Terminal_selection_range empty_range = {
        {4, 6},
        {4, 6},
        term::Terminal_selection_mode::NORMAL,
    };
    selection.set_range(empty_range, QString(), make_phase2_selection_lease(empty_range));
    const std::uint64_t empty_identity = selection.durable_payload_identity();
    const term::Terminal_selection_result empty_text = selection.selected_text();
    ok &= check(selection.internal_state() ==
            term::Terminal_selection_internal_state::ATTACHED_VISIBLE &&
        selection.has_selection() &&
        empty_text.code == term::Terminal_selection_result_code::OK &&
        empty_text.text.isEmpty(),
        "Phase 2 committed empty selection remains an active durable payload");

    const term::Terminal_selection_range replacement_range = {
        {5, 0},
        {5, 4},
        term::Terminal_selection_mode::NORMAL,
    };
    selection.set_range(
        replacement_range,
        QStringLiteral("beta"),
        make_phase2_selection_lease(replacement_range));
    ok &= check(selection.durable_payload_identity() != original_identity &&
        selection.durable_payload_identity() != empty_identity &&
        selection.selected_text().text == QStringLiteral("beta"),
        "Phase 2 non-empty replacement creates a new durable payload identity");

    ok &= check(selection.apply_scrollback_eviction(6),
        "Phase 2 scrollback eviction applies to the active selection");
    ok &= check(selection.internal_state() ==
            term::Terminal_selection_internal_state::PAYLOAD_ONLY &&
        !selection.visual_lease().has_value() &&
        selection.selected_text().code == term::Terminal_selection_result_code::OK &&
        selection.selected_text().text == QStringLiteral("beta"),
        "Phase 2 scrollback eviction detaches visuals while preserving payload");

    return ok;
}

bool test_selection_phase2_model_semantic_flags()
{
    bool ok = true;

    term::Terminal_screen_model_config config;
    config.grid_size = {3, 10};
    term::Terminal_screen_model model(config);

    const term::Terminal_screen_model_result text_result =
        model.ingest(QByteArrayLiteral("abc"));
    ok &= check(text_result.terminal_content_changed &&
        !text_result.active_buffer_changed &&
        !text_result.grid_reflow_changed,
        "Phase 2 model marks printable output as semantic content change");

    const term::Terminal_screen_model_result cursor_result =
        model.ingest(QByteArrayLiteral("\r"));
    ok &= check(!cursor_result.dirty_rows.empty() &&
        !cursor_result.terminal_content_changed &&
        !cursor_result.active_buffer_changed &&
        !cursor_result.grid_reflow_changed,
        "Phase 2 model does not treat cursor-only dirty rows as semantic content identity");

    const term::Terminal_screen_model_result resize_result =
        model.resize({4, 10});
    ok &= check(!resize_result.terminal_content_changed &&
        !resize_result.active_buffer_changed &&
        resize_result.grid_reflow_changed,
        "Phase 2 model reports resize/reflow through an explicit grid flag");

    const term::Terminal_screen_model_result alternate_result =
        model.ingest(QByteArrayLiteral("\x1b[?1049h"));
    ok &= check(alternate_result.active_buffer_changed,
        "Phase 2 model reports alternate-buffer transitions through an explicit buffer flag");

    return ok;
}

bool test_selection_phase2_session_lease_basis_advances()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "Phase 2 lease-basis session starts");
    ok &= check(backend->emit_output(QByteArrayLiteral("alpha")),
        "Phase 2 lease-basis backend publishes initial content");

    session->set_selection_range({
        { 0, 0 },
        { 0, 5 },
        term::Terminal_selection_mode::NORMAL,
    });
    const std::optional<term::terminal_selection_visual_lease_t> initial_lease =
        session->selection_visual_lease();
    ok &= check(initial_lease.has_value() &&
        initial_lease->source_content_basis.content_generation > 0U,
        "Phase 2 initial lease captures advanced content basis after published text");

    session->clear_selection();
    ok &= check(backend->emit_output(QByteArrayLiteral("\ralpha!")),
        "Phase 2 lease-basis backend publishes content mutation");
    session->set_selection_range({
        { 0, 0 },
        { 0, 6 },
        term::Terminal_selection_mode::NORMAL,
    });
    const std::optional<term::terminal_selection_visual_lease_t> mutated_lease =
        session->selection_visual_lease();
    ok &= check(initial_lease.has_value() &&
        mutated_lease.has_value() &&
        mutated_lease->source_content_basis.content_generation >
            initial_lease->source_content_basis.content_generation,
        "Phase 2 lease captures later content-generation advancement");

    session->clear_selection();
    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?1049h")),
        "Phase 2 lease-basis backend enters alternate buffer");
    session->set_selection_range({
        { 0, 0 },
        { 0, 0 },
        term::Terminal_selection_mode::NORMAL,
    });
    const std::optional<term::terminal_selection_visual_lease_t> alternate_lease =
        session->selection_visual_lease();
    ok &= check(mutated_lease.has_value() &&
        alternate_lease.has_value() &&
        alternate_lease->buffer_id == term::Terminal_buffer_id::ALTERNATE &&
        alternate_lease->source_content_basis.content_generation >
            mutated_lease->source_content_basis.content_generation,
        "Phase 2 lease captures active-buffer content-basis advancement");

    session->clear_selection();
    const term::Terminal_session_result resize_result =
        session->resize(QSizeF(400.0, 120.0), {12, 40});
    session->set_selection_range({
        { 0, 0 },
        { 0, 0 },
        term::Terminal_selection_mode::NORMAL,
    });
    const std::optional<term::terminal_selection_visual_lease_t> resized_lease =
        session->selection_visual_lease();
    ok &= check(resize_result.code == term::Terminal_session_result_code::ACCEPTED,
        "Phase 2 lease-basis resize is accepted");
    ok &= check(alternate_lease.has_value() &&
        resized_lease.has_value() &&
        resized_lease->source_content_basis.grid_reflow_generation >
            alternate_lease->source_content_basis.grid_reflow_generation,
        "Phase 2 lease captures grid-reflow basis advancement");

    std::unique_ptr<term::Terminal_session> synchronized_resize_session;
    Scripted_backend* synchronized_resize_backend =
        make_session(synchronized_resize_session);
    ok &= check(synchronized_resize_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "Phase 2 synchronized resize lease-basis session starts");
    ok &= check(synchronized_resize_backend->emit_output(QByteArrayLiteral("\x1b[?2026h")),
        "Phase 2 synchronized resize enters synchronized output");
    const term::Terminal_session_result synchronized_resize_result =
        synchronized_resize_session->resize(QSizeF(640.0, 300.0), {15, 64});
    const std::optional<term::Terminal_render_snapshot> synchronized_resize_snapshot =
        synchronized_resize_session->latest_render_snapshot();
    ok &= check(synchronized_resize_result.code == term::Terminal_session_result_code::ACCEPTED,
        "Phase 2 synchronized resize is accepted");
    ok &= check(synchronized_resize_snapshot.has_value() &&
        synchronized_resize_snapshot->grid_size.rows    == 15 &&
        synchronized_resize_snapshot->grid_size.columns == 64,
        "Phase 2 synchronized resize publishes a geometry snapshot");
    synchronized_resize_session->set_selection_range({
        { 0, 0 },
        { 0, 0 },
        term::Terminal_selection_mode::NORMAL,
    });
    const std::optional<term::terminal_selection_visual_lease_t>
        synchronized_resize_lease =
            synchronized_resize_session->selection_visual_lease();
    ok &= check(synchronized_resize_lease.has_value() &&
        synchronized_resize_lease->source_content_basis.grid_reflow_generation > 0U,
        "Phase 2 lease captures synchronized resize grid-reflow basis advancement");

    return ok;
}

bool test_selection_phase3_line_lease_provenance_capture()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    term::Terminal_launch_config launch_config = valid_launch_config();
    launch_config.initial_grid_size = {4, 20};
    ok &= check(session->start(launch_config).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "Phase 3 line lease session starts");
    ok &= check(backend->emit_output(QByteArrayLiteral("alpha\r\nbeta\r\ngamma")),
        "Phase 3 line lease backend emits selectable rows");
    const std::optional<term::Terminal_render_snapshot> source_snapshot =
        session->latest_render_snapshot();
    const std::optional<term::terminal_selection_source_identity_t> source =
        session->published_selection_source_identity();

    const term::Terminal_selection_range range = {
        {0, 1},
        {2, 3},
        term::Terminal_selection_mode::NORMAL,
    };
    session->set_selection_range_from_published_source(
        range,
        source.value_or(term::terminal_selection_source_identity_t{}));
    const term::Terminal_selection_result payload = session->selected_text();
    const std::optional<term::terminal_selection_visual_lease_t> lease =
        session->selection_visual_lease();
    ok &= check(payload.code == term::Terminal_selection_result_code::OK &&
        payload.text == QStringLiteral("lpha\nbeta\ngam"),
        "Phase 3 line lease payload is extracted from the selected source rows");
    ok &= check(source_snapshot.has_value() && source.has_value() && lease.has_value() &&
        lease->selected_lines ==
            expected_line_leases_from_snapshot(*source_snapshot, range),
        "Phase 3 line lease descriptors match the source snapshot provenance");
    ok &= check(lease.has_value() && lease->selected_lines.size() == 3U,
        "Phase 3 multi-row selection stores one descriptor per selected logical row");
    if (lease.has_value() && lease->selected_lines.size() == 3U) {
        ok &= check(lease->selected_lines[0].row_offset == 0 &&
            lease->selected_lines[1].row_offset == 1 &&
            lease->selected_lines[2].row_offset == 2,
            "Phase 3 multi-row line lease row offsets are normalized from the selected start row");
    }
    if (source_snapshot.has_value() &&
        lease.has_value()          &&
        lease->selected_lines.size() == 3U &&
        source_snapshot->visible_line_provenance.size() >= 3U)
    {
        const std::vector<term::Terminal_render_line_provenance>& provenance =
            source_snapshot->visible_line_provenance;
        ok &= check(provenance[0].logical_row == 0 &&
            provenance[1].logical_row == 1 &&
            provenance[2].logical_row == 2,
            "Phase 3 source snapshot independently exposes the selected logical rows");
        ok &= check(lease->selected_lines[0].retained_line_id ==
                provenance[0].retained_line_id &&
            lease->selected_lines[0].content_generation ==
                provenance[0].content_generation &&
            lease->selected_lines[1].retained_line_id ==
                provenance[1].retained_line_id &&
            lease->selected_lines[1].content_generation ==
                provenance[1].content_generation &&
            lease->selected_lines[2].retained_line_id ==
                provenance[2].retained_line_id &&
            lease->selected_lines[2].content_generation ==
                provenance[2].content_generation,
            "Phase 3 line lease descriptor values match snapshot provenance fields directly");
    }

    term::Terminal_session_config drained_config;
    drained_config.backend_event_notifier = [] {};
    std::unique_ptr<term::Terminal_session> drained_session;
    Scripted_backend* drained_backend = make_session(drained_session, drained_config);
    term::Terminal_launch_config drained_launch_config = valid_launch_config();
    drained_launch_config.initial_grid_size = {3, 20};
    ok &= check(drained_session->start(drained_launch_config).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "Phase 3 drained line lease session starts");
    ok &= check(drained_backend->emit_output(QByteArrayLiteral("drained\r\nsource")),
        "Phase 3 drained line lease backend queues selectable rows");
    ok &= check(drained_session->output_chunks().empty(),
        "Phase 3 drained line lease fixture holds queued output before owner drain");
    drained_session->process_backend_callback_events();
    const std::optional<term::Terminal_render_snapshot> drained_snapshot =
        drained_session->latest_render_snapshot();
    const std::optional<term::terminal_selection_source_identity_t> drained_source =
        drained_session->published_selection_source_identity();
    const term::Terminal_selection_range drained_range = {
        {1, 0},
        {1, 6},
        term::Terminal_selection_mode::NORMAL,
    };
    ok &= check(drained_snapshot.has_value() &&
        drained_source.has_value() &&
        snapshot_row_text(*drained_snapshot, 0) == QStringLiteral("drained") &&
        snapshot_row_text(*drained_snapshot, 1) == QStringLiteral("source"),
        "Phase 3 owner drain publishes queued source rows before selection");
    if (drained_snapshot.has_value() && drained_source.has_value()) {
        drained_session->set_selection_range_from_drained_published_source(
            drained_range,
            *drained_source);
        const term::Terminal_selection_result drained_payload =
            drained_session->selected_text();
        const std::optional<term::terminal_selection_visual_lease_t> drained_lease =
            drained_session->selection_visual_lease();
        ok &= check(drained_payload.code == term::Terminal_selection_result_code::OK &&
            drained_payload.text == QStringLiteral("source"),
            "Phase 3 drained published source payload comes from queued output");
        ok &= check(drained_lease.has_value() &&
            drained_lease->selected_lines.size() == 1U,
            "Phase 3 drained published source captures one retained-line descriptor");
        if (drained_lease.has_value() &&
            drained_lease->selected_lines.size() == 1U &&
            drained_snapshot->visible_line_provenance.size() >= 2U)
        {
            const term::Terminal_render_line_provenance& provenance =
                drained_snapshot->visible_line_provenance[1];
            const term::terminal_selection_line_lease_t& selected_line =
                drained_lease->selected_lines[0];
            ok &= check(provenance.logical_row == 1 &&
                selected_line.row_offset == 0 &&
                selected_line.retained_line_id == provenance.retained_line_id &&
                selected_line.content_generation == provenance.content_generation,
                "Phase 3 drained descriptor fields match drained snapshot provenance directly");
        }
        else {
            ok &= check(false,
                "Phase 3 drained published source fixture exposes selected row provenance");
        }
    }

    return ok;
}

bool test_selection_phase3_line_lease_negative_source_paths()
{
    bool ok = true;

    {
        std::unique_ptr<term::Terminal_session> session;
        Scripted_backend* backend = make_session(session);
        term::Terminal_launch_config launch_config = valid_launch_config();
        launch_config.initial_grid_size = {4, 20};
        ok &= check(session->start(launch_config).code ==
            term::Terminal_session_result_code::ACCEPTED,
            "Phase 3 reversed line lease session starts");
        ok &= check(backend->emit_output(QByteArrayLiteral("alpha\r\nbeta\r\ngamma")),
            "Phase 3 reversed line lease backend emits selectable rows");
        const std::optional<term::Terminal_render_snapshot> source_snapshot =
            session->latest_render_snapshot();
        const std::optional<term::terminal_selection_source_identity_t> source =
            session->published_selection_source_identity();

        const term::Terminal_selection_range reversed_range = {
            {2, 3},
            {0, 1},
            term::Terminal_selection_mode::NORMAL,
        };
        session->set_selection_range_from_published_source(
            reversed_range,
            source.value_or(term::terminal_selection_source_identity_t{}));
        const term::Terminal_selection_result payload = session->selected_text();
        const std::optional<term::terminal_selection_visual_lease_t> lease =
            session->selection_visual_lease();
        ok &= check(payload.code == term::Terminal_selection_result_code::OK &&
            payload.text == QStringLiteral("lpha\nbeta\ngam"),
            "Phase 3 reversed multi-row selection normalizes payload row order");
        ok &= check(source_snapshot.has_value() && source.has_value() && lease.has_value() &&
            lease->selected_range == reversed_range &&
            lease->anchor == reversed_range.start &&
            lease->extent == reversed_range.end,
            "Phase 3 reversed visual lease preserves caller endpoints");
        if (source_snapshot.has_value() &&
            lease.has_value()          &&
            lease->selected_lines.size() == 3U &&
            source_snapshot->visible_line_provenance.size() >= 3U)
        {
            const std::vector<term::Terminal_render_line_provenance>& provenance =
                source_snapshot->visible_line_provenance;
            ok &= check(lease->selected_lines[0].row_offset == 0 &&
                lease->selected_lines[0].retained_line_id ==
                    provenance[0].retained_line_id &&
                lease->selected_lines[0].content_generation ==
                    provenance[0].content_generation &&
                lease->selected_lines[1].row_offset == 1 &&
                lease->selected_lines[1].retained_line_id ==
                    provenance[1].retained_line_id &&
                lease->selected_lines[1].content_generation ==
                    provenance[1].content_generation &&
                lease->selected_lines[2].row_offset == 2 &&
                lease->selected_lines[2].retained_line_id ==
                    provenance[2].retained_line_id &&
                lease->selected_lines[2].content_generation ==
                    provenance[2].content_generation,
                "Phase 3 reversed multi-row descriptor fields normalize to source row order");
        }
        else {
            ok &= check(false,
                "Phase 3 reversed multi-row fixture exposes three source descriptors");
        }
    }

    {
        std::unique_ptr<term::Terminal_session> session;
        Scripted_backend* backend = make_session(session);
        ok &= check(session->start(valid_launch_config()).code ==
            term::Terminal_session_result_code::ACCEPTED,
            "Phase 3 stale source session starts");
        ok &= check(backend->emit_output(QByteArrayLiteral("alpha")),
            "Phase 3 stale source backend emits original content");
        const std::optional<term::terminal_selection_source_identity_t> stale_source =
            session->published_selection_source_identity();
        ok &= check(stale_source.has_value(),
            "Phase 3 stale source fixture publishes a real source before mutation");

        if (stale_source.has_value()) {
            ok &= check(backend->emit_output(QByteArrayLiteral("\ralpha!")),
                "Phase 3 stale source backend mutates published content");

            session->set_selection_range_from_published_source(
                {{0, 0}, {0, 5}, term::Terminal_selection_mode::NORMAL},
                *stale_source);
            ok &= check(!session->has_selection() &&
                !session->selection_visual_lease().has_value(),
                "Phase 3 stale source selection attempt captures no visual lease descriptors");
        }
    }

    {
        std::unique_ptr<term::Terminal_session> session;
        Scripted_backend* backend = make_session(session);
        ok &= check(session->start(valid_launch_config()).code ==
            term::Terminal_session_result_code::ACCEPTED,
            "Phase 3 stale-source clear session starts");
        ok &= check(backend->emit_output(QByteArrayLiteral("alpha")),
            "Phase 3 stale-source clear backend emits original content");
        const std::optional<term::terminal_selection_source_identity_t> stale_source =
            session->published_selection_source_identity();
        const term::Terminal_selection_range range = {
            {0, 0},
            {0, 5},
            term::Terminal_selection_mode::NORMAL,
        };
        ok &= check(stale_source.has_value(),
            "Phase 3 stale-source clear fixture publishes a real source before mutation");

        if (stale_source.has_value()) {
            session->set_selection_range_from_published_source(range, *stale_source);
            const term::Terminal_selection_result initial_payload =
                session->selected_text();
            ok &= check(session->has_selection() &&
                initial_payload.code == term::Terminal_selection_result_code::OK &&
                initial_payload.text == QStringLiteral("alpha"),
                "Phase 3 stale-source clear fixture creates an initial selection");

            ok &= check(backend->emit_output(QByteArrayLiteral("\ralpha!")),
                "Phase 3 stale-source clear backend mutates selected content");
            const bool had_selection_before_stale_attempt = session->has_selection();
            ok &= check(had_selection_before_stale_attempt,
                "Phase 3 stale-source clear fixture retains a selection before stale replacement");

            session->set_selection_range_from_published_source(range, *stale_source);
            ok &= check(had_selection_before_stale_attempt &&
                !session->has_selection() &&
                !session->selection_visual_lease().has_value(),
                "Phase 3 stale source attempt clears an existing selection");
        }
    }

    {
        std::unique_ptr<term::Terminal_session> session;
        Scripted_backend* backend = make_session(session);
        term::Terminal_launch_config launch_config = valid_launch_config();
        launch_config.initial_grid_size = {3, 20};
        ok &= check(session->start(launch_config).code ==
            term::Terminal_session_result_code::ACCEPTED,
            "Phase 3 mismatched source session starts");
        ok &= check(backend->emit_output(numbered_scroll_lines(6)),
            "Phase 3 mismatched source backend creates scrollback");
        const std::optional<term::Terminal_render_snapshot> source_snapshot =
            session->latest_render_snapshot();
        const std::optional<term::terminal_selection_source_identity_t> source =
            session->published_selection_source_identity();

        if (source_snapshot.has_value() && source.has_value()) {
            term::terminal_selection_source_identity_t mismatched_source = *source;
            ++mismatched_source.viewport_mapping.offset_from_tail;
            const int first_visible_logical_row =
                term::render_snapshot_first_visible_logical_row(*source_snapshot);
            const term::Terminal_selection_range range = {
                {first_visible_logical_row, 0},
                {first_visible_logical_row, 15},
                term::Terminal_selection_mode::NORMAL,
            };
            session->set_selection_range_from_published_source(range, mismatched_source);
            const term::Terminal_selection_result payload = session->selected_text();
            const std::optional<term::terminal_selection_visual_lease_t> lease =
                session->selection_visual_lease();
            ok &= check(payload.code == term::Terminal_selection_result_code::OK &&
                payload.text == QStringLiteral("scroll-line-%1")
                    .arg(first_visible_logical_row, 3, 10, QLatin1Char('0')),
                "Phase 3 mismatched viewport source still uses active-model selection fallback");
            ok &= check(session->has_selection(),
                "Phase 3 mismatched viewport source may create a fallback selection");
            ok &= check(lease.has_value() && lease->selected_lines.empty(),
                "Phase 3 mismatched snapshot identity captures no retained-line descriptors");
        }
        else {
            ok &= check(false,
                "Phase 3 mismatched source fixture publishes a selectable source");
        }
    }

    {
        std::unique_ptr<term::Terminal_session> session;
        Scripted_backend* backend = make_session(session);
        term::Terminal_launch_config launch_config = valid_launch_config();
        launch_config.initial_grid_size = {3, 20};
        ok &= check(session->start(launch_config).code ==
            term::Terminal_session_result_code::ACCEPTED,
            "Phase 3 offscreen source session starts");
        ok &= check(backend->emit_output(numbered_scroll_lines(6)),
            "Phase 3 offscreen source backend creates scrollback");
        const std::optional<term::Terminal_render_snapshot> source_snapshot =
            session->latest_render_snapshot();
        const std::optional<term::terminal_selection_source_identity_t> source =
            session->published_selection_source_identity();

        if (source_snapshot.has_value() && source.has_value()) {
            const int offscreen_logical_row =
                term::render_snapshot_first_visible_logical_row(*source_snapshot) - 1;
            const term::Terminal_selection_range range = {
                {offscreen_logical_row, 0},
                {offscreen_logical_row, 15},
                term::Terminal_selection_mode::NORMAL,
            };
            session->set_selection_range_from_published_source(range, *source);
            const term::Terminal_selection_result payload = session->selected_text();
            const std::optional<term::terminal_selection_visual_lease_t> lease =
                session->selection_visual_lease();
            const std::optional<term::Terminal_render_snapshot> selected_snapshot =
                session->latest_render_snapshot();
            ok &= check(offscreen_logical_row >= 0 &&
                payload.code == term::Terminal_selection_result_code::OK &&
                payload.text == QStringLiteral("scroll-line-%1")
                    .arg(offscreen_logical_row, 3, 10, QLatin1Char('0')),
                "Phase 3 offscreen range keeps selection payload through active-model fallback");
            ok &= check(lease.has_value() && lease->selected_lines.empty(),
                "Phase 3 offscreen visible-line provenance captures no descriptors");
            ok &= check(selected_snapshot.has_value() &&
                selected_snapshot->selection_spans.empty(),
                "Phase 3 offscreen selection emits no visible spans");
        }
        else {
            ok &= check(false,
                "Phase 3 offscreen source fixture publishes a selectable source");
        }
    }

    {
        std::unique_ptr<term::Terminal_session> session;
        Scripted_backend* backend = make_session(session);
        term::Terminal_launch_config launch_config = valid_launch_config();
        launch_config.initial_grid_size = {3, 12};
        ok &= check(session->start(launch_config).code ==
            term::Terminal_session_result_code::ACCEPTED,
            "Phase 3 missing provenance session starts");
        ok &= check(backend->emit_output(QByteArrayLiteral("visible")),
            "Phase 3 missing provenance backend emits base content");
        ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026hhidden")),
            "Phase 3 missing provenance backend holds synchronized output");
        const term::Terminal_session_result resize_result =
            session->resize(QSizeF(120.0, 80.0), {4, 12});
        const std::optional<term::Terminal_render_snapshot> geometry_snapshot =
            session->latest_render_snapshot();
        const std::optional<term::terminal_selection_source_identity_t> geometry_source =
            session->published_selection_source_identity();
        ok &= check(resize_result.code == term::Terminal_session_result_code::ACCEPTED,
            "Phase 3 missing provenance synchronized resize is accepted");
        ok &= check(geometry_snapshot.has_value() &&
            geometry_snapshot->visible_line_provenance.empty(),
            "Phase 3 missing provenance fixture publishes geometry without line descriptors");

        session->set_selection_range_from_published_source(
            {{0, 0}, {0, 4}, term::Terminal_selection_mode::NORMAL},
            geometry_source.value_or(term::terminal_selection_source_identity_t{}));
        const term::Terminal_selection_result payload = session->selected_text();
        const std::optional<term::terminal_selection_visual_lease_t> lease =
            session->selection_visual_lease();
        const std::optional<term::Terminal_render_snapshot> selected_snapshot =
            session->latest_render_snapshot();
        ok &= check(payload.code == term::Terminal_selection_result_code::OK &&
            payload.text == QStringLiteral("visi"),
            "Phase 3 missing provenance path keeps selection payload behavior");
        ok &= check(lease.has_value() && lease->selected_lines.empty(),
            "Phase 3 missing provenance path captures no retained-line descriptors");
        ok &= check(selected_snapshot.has_value() &&
            selected_snapshot->selection_spans.empty(),
            "Phase 3 missing provenance path suppresses visible spans fail-closed");
        ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026l")),
            "Phase 5 missing retained-line proof backend releases synchronized output");
        const std::optional<term::Terminal_render_snapshot> released_snapshot =
            session->latest_render_snapshot();
        ok &= check(!session->selection_visual_lease().has_value(),
            "Phase 5 missing retained-line proof detaches the visual lease fail-closed");
        ok &= check(released_snapshot.has_value() &&
            released_snapshot->selection_spans.empty(),
            "Phase 5 missing retained-line proof suppresses spans after release");
    }

    return ok;
}

bool test_selection_phase5_visual_lease_span_compatibility()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "Phase 5 compatible visual lease session starts");
    ok &= check(backend->emit_output(QByteArrayLiteral("alpha\r\nbeta")),
        "Phase 5 compatible visual lease backend emits visible rows");

    session->set_selection_range({
        { 0, 0 },
        { 0, 5 },
        term::Terminal_selection_mode::NORMAL,
    });
    const std::optional<term::Terminal_render_snapshot> selected_snapshot =
        session->latest_render_snapshot();
    const std::optional<term::terminal_selection_visual_lease_t> selected_lease =
        session->selection_visual_lease();
    const term::terminal_selection_content_basis_t selected_basis =
        selected_lease.has_value()
            ? selected_lease->source_content_basis
            : term::terminal_selection_content_basis_t{};
    ok &= check(selected_snapshot.has_value() &&
        snapshot_has_selection_span(*selected_snapshot, 0, 0, 5),
        "Phase 5 attached visible lease initially emits selection spans");
    ok &= check(selected_lease.has_value(),
        "Phase 5 attached visible selection records a visual lease");
    const std::vector<term::terminal_selection_line_lease_t> selected_lines =
        selected_lease.has_value()
            ? selected_lease->selected_lines
            : std::vector<term::terminal_selection_line_lease_t>{};

    ok &= check(backend->emit_output(QByteArrayLiteral("\r")),
        "Phase 5 cursor-only backend output is accepted");
    const std::optional<term::Terminal_render_snapshot> cursor_snapshot =
        session->latest_render_snapshot();
    const std::optional<term::terminal_selection_visual_lease_t> cursor_lease =
        session->selection_visual_lease();
    ok &= check(cursor_snapshot.has_value() &&
        snapshot_has_selection_span(*cursor_snapshot, 0, 0, 5),
        "Phase 5 cursor-only snapshot preserves compatible selection spans");
    ok &= check(cursor_lease.has_value() &&
        cursor_lease->source_content_basis == selected_basis,
        "Phase 5 cursor-only snapshot preserves the visual lease basis");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?25l")),
        "Phase 5 paint-only cursor-visibility update is accepted");
    const std::optional<term::Terminal_render_snapshot> paint_snapshot =
        session->latest_render_snapshot();
    const std::optional<term::terminal_selection_visual_lease_t> paint_lease =
        session->selection_visual_lease();
    ok &= check(paint_snapshot.has_value()                         &&
        !paint_snapshot->modes.cursor_visible                      &&
        snapshot_has_selection_span(*paint_snapshot, 0, 0, 5),
        "Phase 5 paint-only cursor-visibility snapshot preserves compatible selection spans");
    ok &= check(paint_lease.has_value() &&
        paint_lease->source_content_basis == selected_basis,
        "Phase 5 paint-only cursor-visibility snapshot preserves the visual lease basis");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[2;1Hunrelated")),
        "Phase 5 unrelated row backend output is accepted");
    const std::optional<term::Terminal_render_snapshot> unrelated_snapshot =
        session->latest_render_snapshot();
    const std::optional<term::terminal_selection_visual_lease_t> unrelated_lease =
        session->selection_visual_lease();
    ok &= check(unrelated_snapshot.has_value() &&
        snapshot_row_text(*unrelated_snapshot, 1) == QStringLiteral("unrelated") &&
        snapshot_has_selection_span(*unrelated_snapshot, 0, 0, 5),
        "Phase 5 unrelated row output keeps committed selection spans renderable");
    ok &= check(unrelated_lease.has_value() &&
        unrelated_lease->source_content_basis.content_generation >
            selected_basis.content_generation,
        "Phase 5 unrelated row output advances the visual lease by retained-line proof");
    ok &= check(unrelated_lease.has_value() &&
        unrelated_lease->selected_lines == selected_lines,
        "Phase 5 unrelated row output keeps retained-line descriptors stable");

    std::unique_ptr<term::Terminal_session> boundary_session;
    Scripted_backend* boundary_backend = make_session(boundary_session);
    ok &= check(boundary_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "Phase 4b boundary viewport-scroll lease session starts");
    ok &= check(boundary_backend->emit_output(numbered_scroll_lines(80)),
        "Phase 4b boundary viewport-scroll backend creates scrollback");
    ok &= check(boundary_session->scroll_viewport_lines(5).action ==
        term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "Phase 4b boundary viewport-scroll fixture scrolls into scrollback");
    const std::optional<term::Terminal_render_snapshot> boundary_anchor_snapshot =
        boundary_session->latest_render_snapshot();
    if (!boundary_anchor_snapshot.has_value()) {
        return false;
    }

    const int boundary_first_visible_logical_row =
        boundary_anchor_snapshot->viewport.scrollback_rows -
        boundary_anchor_snapshot->viewport.offset_from_tail;
    const int boundary_selection_row = boundary_first_visible_logical_row;
    const QString boundary_payload_text =
        QStringLiteral("scroll-line-%1")
            .arg(boundary_selection_row, 3, 10, QLatin1Char('0'));
    boundary_session->set_selection_range({
        { boundary_selection_row, 0 },
        { boundary_selection_row, 15 },
        term::Terminal_selection_mode::NORMAL,
    });
    const term::Terminal_selection_result boundary_payload =
        boundary_session->selected_text();
    const std::optional<term::Terminal_render_snapshot> boundary_selected_snapshot =
        boundary_session->latest_render_snapshot();
    ok &= check(boundary_payload.code == term::Terminal_selection_result_code::OK &&
        boundary_payload.text == boundary_payload_text,
        "Phase 4b boundary viewport-scroll selection captures durable payload");
    ok &= check(boundary_selected_snapshot.has_value() &&
        snapshot_has_selection_span(*boundary_selected_snapshot, 0, 0, 15),
        "Phase 4b boundary viewport-scroll selection emits spans while visible");

    ok &= check(boundary_session->scroll_viewport_lines(-5).action ==
        term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "Phase 4b boundary viewport-scroll fixture returns to tail");
    ok &= check(boundary_session->scroll_viewport_lines(6).action ==
        term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "Phase 4b boundary viewport-scroll fixture makes selection visible through a new mapping");
    const std::optional<term::Terminal_render_snapshot> boundary_remapped_snapshot =
        boundary_session->latest_render_snapshot();
    ok &= check(boundary_remapped_snapshot.has_value() &&
        boundary_remapped_snapshot->selection_spans.empty(),
        "Phase 4b boundary viewport remap does not project spans before Phase 5 opt-in");
    ok &= check(boundary_session->selection_visual_lease().has_value() &&
        boundary_session->selected_text().text == boundary_payload.text,
        "Phase 4b boundary viewport remap keeps lease and payload available");

    std::unique_ptr<term::Terminal_session> scroll_session;
    term::Terminal_session_config phase5_projection_config;
    phase5_projection_config.selection_viewport_projection_enabled = true;
    Scripted_backend* scroll_backend =
        make_session(scroll_session, phase5_projection_config);
    ok &= check(scroll_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "Phase 5 viewport-projection lease session starts");
    ok &= check(scroll_backend->emit_output(numbered_scroll_lines(80)),
        "Phase 5 viewport-projection backend creates scrollback");
    ok &= check(scroll_session->scroll_viewport_lines(5).action ==
        term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "Phase 5 viewport-projection fixture scrolls into scrollback");
    const std::optional<term::Terminal_render_snapshot> scroll_anchor_snapshot =
        scroll_session->latest_render_snapshot();
    if (!scroll_anchor_snapshot.has_value()) {
        return false;
    }

    const int first_visible_logical_row =
        scroll_anchor_snapshot->viewport.scrollback_rows -
        scroll_anchor_snapshot->viewport.offset_from_tail;
    const int selection_row = first_visible_logical_row;
    scroll_session->set_selection_range({
        { selection_row, 0 },
        { selection_row, 15 },
        term::Terminal_selection_mode::NORMAL,
    });
    const term::Terminal_selection_result scroll_payload =
        scroll_session->selected_text();
    const std::optional<term::Terminal_render_snapshot> scroll_selected_snapshot =
        scroll_session->latest_render_snapshot();
    ok &= check(scroll_payload.code == term::Terminal_selection_result_code::OK &&
        scroll_payload.text == QStringLiteral("scroll-line-%1")
            .arg(selection_row, 3, 10, QLatin1Char('0')),
        "Phase 5 viewport-projection selection captures durable payload");
    ok &= check(scroll_selected_snapshot.has_value() &&
        snapshot_has_selection_span(*scroll_selected_snapshot, 0, 0, 15),
        "Phase 5 viewport-projection selection emits spans while visible");

    ok &= check(scroll_session->scroll_viewport_lines(-5).action ==
        term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "Phase 5 viewport-projection fixture returns to tail");
    const std::optional<term::Terminal_render_snapshot> scroll_hidden_snapshot =
        scroll_session->latest_render_snapshot();
    ok &= check(scroll_hidden_snapshot.has_value() &&
        scroll_hidden_snapshot->selection_spans.empty(),
        "Phase 5 compatible viewport scroll hides offscreen spans");
    ok &= check(scroll_session->selection_visual_lease().has_value() &&
        scroll_session->selected_text().text == scroll_payload.text,
        "Phase 5 compatible viewport scroll keeps lease and payload while hidden");

    ok &= check(scroll_session->scroll_viewport_lines(6).action ==
        term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "Phase 5 viewport-projection fixture makes selected rows visible through a new mapping");
    const std::optional<term::Terminal_render_snapshot> scroll_shown_snapshot =
        scroll_session->latest_render_snapshot();
    ok &= check(scroll_shown_snapshot.has_value() &&
        snapshot_has_selection_span(*scroll_shown_snapshot, 1, 0, 15),
        "Phase 5 compatible viewport projection re-emits spans when visible again");

    return ok;
}

bool test_selection_spans_preserve_after_same_viewport_idempotent_selected_row_rewrite()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "same-viewport idempotent rewrite selection session starts");
    ok &= check(backend->emit_output(QByteArrayLiteral("alpha\r\nbeta")),
        "same-viewport idempotent rewrite backend emits visible rows");

    session->set_selection_range({
        { 0, 0 },
        { 0, 5 },
        term::Terminal_selection_mode::NORMAL,
    });
    const term::Terminal_selection_result original_payload =
        session->selected_text();
    const std::optional<term::Terminal_render_snapshot> selected_snapshot =
        session->latest_render_snapshot();
    const std::optional<term::terminal_selection_visual_lease_t> selected_lease =
        session->selection_visual_lease();
    const term::terminal_selection_content_basis_t selected_basis =
        selected_lease.has_value()
            ? selected_lease->source_content_basis
            : term::terminal_selection_content_basis_t{};
    const std::vector<term::terminal_selection_line_lease_t> selected_lines =
        selected_lease.has_value()
            ? selected_lease->selected_lines
            : std::vector<term::terminal_selection_line_lease_t>{};
    ok &= check(original_payload.code == term::Terminal_selection_result_code::OK &&
        original_payload.text == QStringLiteral("alpha"),
        "same-viewport idempotent rewrite captures the selected payload");
    ok &= check(selected_snapshot.has_value() &&
        snapshot_has_selection_span(*selected_snapshot, 0, 0, 5),
        "same-viewport idempotent rewrite initially emits selection spans");
    ok &= check(selected_lease.has_value() && selected_lease->selected_lines.size() == 1U,
        "same-viewport idempotent rewrite starts with retained-line proof");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[1;1Halpha")),
        "same-viewport idempotent rewrite rewrites the selected row with identical text");
    const term::Terminal_selection_result retained_payload =
        session->selected_text();
    const std::optional<term::Terminal_render_snapshot> rewritten_snapshot =
        session->latest_render_snapshot();
    const std::optional<term::terminal_selection_visual_lease_t> rewritten_lease =
        session->selection_visual_lease();
    ok &= check(retained_payload.code == term::Terminal_selection_result_code::OK &&
        retained_payload.text == original_payload.text,
        "same-viewport idempotent rewrite keeps the finalized copy payload");
    ok &= check(rewritten_lease.has_value() &&
        rewritten_lease->source_content_basis.content_generation >
            selected_basis.content_generation,
        "same-viewport idempotent rewrite advances the visual lease basis");
    ok &= check(rewritten_lease.has_value() &&
        rewritten_lease->selected_lines == selected_lines,
        "same-viewport idempotent rewrite keeps the retained-line proof");
    ok &= check(rewritten_snapshot.has_value() &&
        snapshot_row_text(*rewritten_snapshot, 0) == QStringLiteral("alpha") &&
        snapshot_has_selection_span(*rewritten_snapshot, 0, 0, 5),
        "same-viewport idempotent rewrite preserves visible selection spans");

    return ok;
}

bool test_selection_spans_preserve_during_scrollback_growth_with_retained_lines()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    term::Terminal_launch_config launch_config = valid_launch_config();
    launch_config.initial_grid_size = {4, 20};
    ok &= check(session->start(launch_config).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "Phase 4a scrollback-growth selection session starts");
    ok &= check(backend->emit_output(numbered_scroll_lines(32)),
        "Phase 4a scrollback-growth backend creates retained scrollback");
    ok &= check(session->scroll_viewport_lines(4).action ==
        term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "Phase 4a scrollback-growth fixture detaches viewport from tail");

    const std::optional<term::Terminal_render_snapshot> anchor_snapshot =
        session->latest_render_snapshot();
    if (!anchor_snapshot.has_value()) {
        return check(false, "Phase 4a scrollback-growth anchor snapshot is available");
    }

    const int first_visible_logical_row =
        term::render_snapshot_first_visible_logical_row(*anchor_snapshot);
    const int selection_row = first_visible_logical_row + 1;
    const QString expected_payload =
        QStringLiteral("scroll-line-%1").arg(selection_row, 3, 10, QLatin1Char('0'));
    session->set_selection_range({
        {selection_row, 0},
        {selection_row, 15},
        term::Terminal_selection_mode::NORMAL,
    });

    const term::Terminal_selection_result selected_payload =
        session->selected_text();
    const std::optional<term::Terminal_render_snapshot> selected_snapshot =
        session->latest_render_snapshot();
    const std::optional<term::terminal_selection_visual_lease_t> selected_lease =
        session->selection_visual_lease();
    ok &= check(selected_payload.code == term::Terminal_selection_result_code::OK &&
        selected_payload.text == expected_payload,
        "Phase 4a scrollback-growth selection captures the retained row payload");
    ok &= check(selected_snapshot.has_value() &&
        snapshot_has_selection_span(*selected_snapshot, 1, 0, 15),
        "Phase 4a scrollback-growth selection initially emits a visible span");
    ok &= check(selected_lease.has_value() && selected_lease->selected_lines.size() == 1U,
        "Phase 4a scrollback-growth selection starts with complete retained-line proof");

    ok &= check(backend->emit_output(numbered_scroll_lines(3)),
        "Phase 4a scrollback-growth backend appends unrelated tail output");
    const term::Terminal_selection_result preserved_payload =
        session->selected_text();
    const std::optional<term::Terminal_render_snapshot> grown_snapshot =
        session->latest_render_snapshot();
    const std::optional<term::terminal_selection_visual_lease_t> grown_lease =
        session->selection_visual_lease();
    ok &= check(preserved_payload.code == term::Terminal_selection_result_code::OK &&
        preserved_payload.text == expected_payload,
        "Phase 4a scrollback-growth preserves the finalized copy payload");
    ok &= check(grown_snapshot.has_value() &&
        term::render_snapshot_first_visible_logical_row(*grown_snapshot) ==
            first_visible_logical_row,
        "Phase 4a scrollback-growth keeps the detached viewport on the selected content");
    ok &= check(grown_lease.has_value(),
        "Phase 4a scrollback-growth preserves the visual lease by retained-line proof");
    ok &= check(grown_snapshot.has_value() &&
        snapshot_has_selection_span(*grown_snapshot, 1, 0, 15),
        "Phase 4a scrollback-growth preserves visible selection spans");

    return ok;
}

bool test_selection_spans_detach_when_selected_row_mutates()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "mutating selection session starts");
    ok &= check(backend->emit_output(QByteArrayLiteral("original\r\nstable")),
        "mutating selection backend emits visible rows");

    session->set_selection_range({
        { 0, 0 },
        { 0, 8 },
        term::Terminal_selection_mode::NORMAL,
    });
    const term::Terminal_selection_result original_payload =
        session->selected_text();
    const std::optional<term::Terminal_render_snapshot> selected_snapshot =
        session->latest_render_snapshot();
    ok &= check(original_payload.code == term::Terminal_selection_result_code::OK &&
        original_payload.text == QStringLiteral("original"),
        "mutating selection captures the original payload");
    ok &= check(selected_snapshot.has_value() &&
        snapshot_has_selection_span(*selected_snapshot, 0, 0, 8),
        "mutating selection initially publishes a visible span");
    const std::optional<term::terminal_selection_visual_lease_t> selected_lease =
        session->selection_visual_lease();
    ok &= check(selected_snapshot.has_value() && selected_lease.has_value() &&
        !selected_lease->selected_lines.empty(),
        "Phase 3 selected-row mutation fixture starts with captured descriptors");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[1;1Hmutated!")),
        "mutating selection backend rewrites the selected row");
    const term::Terminal_selection_result retained_payload =
        session->selected_text();
    const std::optional<term::Terminal_render_snapshot> mutated_snapshot =
        session->latest_render_snapshot();
    ok &= check(retained_payload.code == term::Terminal_selection_result_code::OK &&
        retained_payload.text == QStringLiteral("original"),
        "mutating selection keeps the finalized copy payload");
    ok &= check(session->has_selection(),
        "mutating selection keeps public copyability active after visual detach");
    ok &= check(!session->selection_visual_lease().has_value(),
        "mutating selection detaches the stale visual lease");
    ok &= check(mutated_snapshot.has_value() &&
        snapshot_row_text(*mutated_snapshot, 0) == QStringLiteral("mutated!"),
        "mutating selection snapshot contains replacement row text");

    ok &= check(mutated_snapshot.has_value() &&
        mutated_snapshot->selection_spans.empty(),
        "mutating selection detaches spans instead of highlighting replacement text");
    ok &= check(backend->emit_output(QByteArrayLiteral("\r")),
        "mutating selection cursor-only follow-up output is accepted");
    const term::Terminal_selection_result followup_payload =
        session->selected_text();
    const std::optional<term::Terminal_render_snapshot> followup_snapshot =
        session->latest_render_snapshot();
    ok &= check(session->has_selection() &&
        followup_payload.code == term::Terminal_selection_result_code::OK &&
        followup_payload.text == QStringLiteral("original"),
        "mutating selection follow-up keeps durable copyability and payload");
    ok &= check(!session->selection_visual_lease().has_value(),
        "mutating selection follow-up keeps the stale visual lease detached");
    ok &= check(followup_snapshot.has_value() &&
        followup_snapshot->selection_spans.empty(),
        "mutating selection cursor-only follow-up does not reattach stale spans");

    return ok;
}

bool test_selection_spans_detach_when_retained_row_moves()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "retained-row movement selection session starts");
    ok &= check(backend->emit_output(QByteArrayLiteral("alpha\r\nbeta\r\ngamma")),
        "retained-row movement backend emits visible rows");

    session->set_selection_range({
        { 1, 0 },
        { 1, 4 },
        term::Terminal_selection_mode::NORMAL,
    });
    const term::Terminal_selection_result original_payload =
        session->selected_text();
    const std::optional<term::Terminal_render_snapshot> selected_snapshot =
        session->latest_render_snapshot();
    ok &= check(original_payload.code == term::Terminal_selection_result_code::OK &&
        original_payload.text == QStringLiteral("beta"),
        "retained-row movement selection captures the original payload");
    ok &= check(selected_snapshot.has_value() &&
        snapshot_has_selection_span(*selected_snapshot, 1, 0, 4),
        "retained-row movement initially publishes a visible span");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[1;1H\x1b[L")),
        "retained-row movement backend inserts a line above the selection");
    const term::Terminal_selection_result retained_payload =
        session->selected_text();
    const std::optional<term::Terminal_render_snapshot> moved_snapshot =
        session->latest_render_snapshot();
    ok &= check(retained_payload.code == term::Terminal_selection_result_code::OK &&
        retained_payload.text == QStringLiteral("beta"),
        "retained-row movement keeps the finalized copy payload");
    ok &= check(moved_snapshot.has_value() &&
        snapshot_row_text(*moved_snapshot, 2) == QStringLiteral("beta"),
        "retained-row movement snapshot keeps the retained text at its moved row");
    ok &= check(!session->selection_visual_lease().has_value(),
        "retained-row movement detaches the stale visual lease");
    ok &= check(moved_snapshot.has_value() &&
        moved_snapshot->selection_spans.empty(),
        "retained-row movement suppresses spans after row-layout mismatch");

    std::unique_ptr<term::Terminal_session> delete_session;
    Scripted_backend* delete_backend = make_session(delete_session);
    ok &= check(delete_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "retained-row delete movement selection session starts");
    ok &= check(delete_backend->emit_output(QByteArrayLiteral("alpha\r\nbeta\r\ngamma")),
        "retained-row delete movement backend emits visible rows");

    delete_session->set_selection_range({
        { 2, 0 },
        { 2, 5 },
        term::Terminal_selection_mode::NORMAL,
    });
    const term::Terminal_selection_result delete_original_payload =
        delete_session->selected_text();
    const std::optional<term::Terminal_render_snapshot> delete_selected_snapshot =
        delete_session->latest_render_snapshot();
    ok &= check(delete_original_payload.code == term::Terminal_selection_result_code::OK &&
        delete_original_payload.text == QStringLiteral("gamma"),
        "retained-row delete movement selection captures the original payload");
    ok &= check(delete_selected_snapshot.has_value() &&
        snapshot_has_selection_span(*delete_selected_snapshot, 2, 0, 5),
        "retained-row delete movement initially publishes a visible span");

    ok &= check(delete_backend->emit_output(QByteArrayLiteral("\x1b[2;1H\x1b[M")),
        "retained-row delete movement backend deletes a line above the selection");
    const term::Terminal_selection_result delete_retained_payload =
        delete_session->selected_text();
    const std::optional<term::Terminal_render_snapshot> delete_moved_snapshot =
        delete_session->latest_render_snapshot();
    ok &= check(delete_retained_payload.code == term::Terminal_selection_result_code::OK &&
        delete_retained_payload.text == QStringLiteral("gamma"),
        "retained-row delete movement keeps the finalized copy payload");
    ok &= check(delete_moved_snapshot.has_value() &&
        snapshot_row_text(*delete_moved_snapshot, 1) == QStringLiteral("gamma"),
        "retained-row delete movement snapshot keeps the retained text at its moved row");
    ok &= check(!delete_session->selection_visual_lease().has_value(),
        "retained-row delete movement detaches the stale visual lease");
    ok &= check(delete_moved_snapshot.has_value() &&
        delete_moved_snapshot->selection_spans.empty(),
        "retained-row delete movement suppresses spans after row-layout mismatch");

    return ok;
}

bool test_selection_spans_preserve_after_unchanged_synchronized_output_release()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "unchanged sync-release selection session starts");
    ok &= check(backend->emit_output(QByteArrayLiteral("original\r\nstable")),
        "unchanged sync-release backend emits visible rows");

    session->set_selection_range({
        { 0, 0 },
        { 0, 8 },
        term::Terminal_selection_mode::NORMAL,
    });
    const term::Terminal_selection_result original_payload =
        session->selected_text();
    const std::optional<term::Terminal_render_snapshot> selected_snapshot =
        session->latest_render_snapshot();
    ok &= check(original_payload.code == term::Terminal_selection_result_code::OK &&
        original_payload.text == QStringLiteral("original"),
        "unchanged sync-release selection captures the original payload");
    ok &= check(selected_snapshot.has_value() &&
        snapshot_has_selection_span(*selected_snapshot, 0, 0, 8),
        "unchanged sync-release selection initially publishes a visible span");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026h\x1b[2;1Hchanged")),
        "unchanged sync-release backend mutates an unselected row while unpublished");
    const term::Terminal_selection_result held_payload =
        session->selected_text();
    ok &= check(held_payload.code == term::Terminal_selection_result_code::OK &&
        held_payload.text == QStringLiteral("original"),
        "unchanged sync-release selection retains payload while output is hidden");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026l")),
        "unchanged sync-release backend publishes held mutation");
    const term::Terminal_selection_result retained_payload =
        session->selected_text();
    const std::optional<term::Terminal_render_snapshot> released_snapshot =
        session->latest_render_snapshot();
    ok &= check(retained_payload.code == term::Terminal_selection_result_code::OK &&
        retained_payload.text == QStringLiteral("original"),
        "unchanged sync-release selection retains payload after publication");
    ok &= check(session->has_selection(),
        "unchanged sync-release selection keeps public copyability active");
    ok &= check(session->selection_visual_lease().has_value(),
        "unchanged sync-release selection preserves the visual lease");
    ok &= check(released_snapshot.has_value() &&
        snapshot_row_text(*released_snapshot, 0) == QStringLiteral("original") &&
        snapshot_row_text(*released_snapshot, 1) == QStringLiteral("changed"),
        "unchanged sync-release snapshot contains the unchanged selection and mutation");

    ok &= check(released_snapshot.has_value() &&
        snapshot_has_selection_span(*released_snapshot, 0, 0, 8),
        "unchanged sync-release preserves spans by retained-line proof");
    ok &= check(backend->emit_output(QByteArrayLiteral("\r")),
        "unchanged sync-release cursor-only follow-up output is accepted");
    const term::Terminal_selection_result followup_payload =
        session->selected_text();
    const std::optional<term::Terminal_render_snapshot> followup_snapshot =
        session->latest_render_snapshot();
    ok &= check(session->has_selection() &&
        followup_payload.code == term::Terminal_selection_result_code::OK &&
        followup_payload.text == QStringLiteral("original"),
        "unchanged sync-release follow-up keeps durable copyability and payload");
    ok &= check(session->selection_visual_lease().has_value(),
        "unchanged sync-release follow-up keeps the visual lease attached");
    ok &= check(followup_snapshot.has_value() &&
        snapshot_has_selection_span(*followup_snapshot, 0, 0, 8),
        "unchanged sync-release cursor-only follow-up keeps visible spans");

    return ok;
}

bool test_selection_spans_detach_when_synchronized_release_mutates_selected_row()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "mutating sync-release selection session starts");
    ok &= check(backend->emit_output(QByteArrayLiteral("original\r\nstable")),
        "mutating sync-release backend emits visible rows");

    session->set_selection_range({
        { 0, 0 },
        { 0, 8 },
        term::Terminal_selection_mode::NORMAL,
    });
    const term::Terminal_selection_result original_payload =
        session->selected_text();
    const std::optional<term::Terminal_render_snapshot> selected_snapshot =
        session->latest_render_snapshot();
    ok &= check(original_payload.code == term::Terminal_selection_result_code::OK &&
        original_payload.text == QStringLiteral("original"),
        "mutating sync-release selection captures the original payload");
    ok &= check(selected_snapshot.has_value() &&
        snapshot_has_selection_span(*selected_snapshot, 0, 0, 8),
        "mutating sync-release selection initially publishes a visible span");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026h\x1b[1;1Hmutated!")),
        "mutating sync-release backend rewrites the selected row while unpublished");
    const term::Terminal_selection_result held_payload =
        session->selected_text();
    ok &= check(held_payload.code == term::Terminal_selection_result_code::OK &&
        held_payload.text == QStringLiteral("original"),
        "mutating sync-release selection retains payload while output is hidden");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026l")),
        "mutating sync-release backend publishes held selected-row mutation");
    const term::Terminal_selection_result retained_payload =
        session->selected_text();
    const std::optional<term::Terminal_render_snapshot> released_snapshot =
        session->latest_render_snapshot();
    ok &= check(retained_payload.code == term::Terminal_selection_result_code::OK &&
        retained_payload.text == QStringLiteral("original"),
        "mutating sync-release keeps the finalized copy payload");
    ok &= check(!session->selection_visual_lease().has_value(),
        "mutating sync-release detaches the stale visual lease");
    ok &= check(released_snapshot.has_value() &&
        snapshot_row_text(*released_snapshot, 0) == QStringLiteral("mutated!"),
        "mutating sync-release snapshot contains replacement selected-row text");
    ok &= check(released_snapshot.has_value() &&
        released_snapshot->selection_spans.empty(),
        "mutating sync-release suppresses stale spans");

    return ok;
}

bool test_selection_spans_detach_when_synchronized_release_moves_retained_row()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> insert_session;
    Scripted_backend* insert_backend = make_session(insert_session);
    ok &= check(insert_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "insert sync-release movement selection session starts");
    ok &= check(insert_backend->emit_output(QByteArrayLiteral("alpha\r\nbeta\r\ngamma")),
        "insert sync-release movement backend emits visible rows");

    insert_session->set_selection_range({
        { 1, 0 },
        { 1, 4 },
        term::Terminal_selection_mode::NORMAL,
    });
    const term::Terminal_selection_result insert_original_payload =
        insert_session->selected_text();
    const std::optional<term::Terminal_render_snapshot> insert_selected_snapshot =
        insert_session->latest_render_snapshot();
    ok &= check(insert_original_payload.code == term::Terminal_selection_result_code::OK &&
        insert_original_payload.text == QStringLiteral("beta"),
        "insert sync-release movement selection captures the original payload");
    ok &= check(insert_selected_snapshot.has_value() &&
        snapshot_has_selection_span(*insert_selected_snapshot, 1, 0, 4),
        "insert sync-release movement initially publishes a visible span");

    ok &= check(insert_backend->emit_output(QByteArrayLiteral("\x1b[?2026h\x1b[1;1H\x1b[L")),
        "insert sync-release movement inserts a line above the selection while unpublished");
    ok &= check(insert_backend->emit_output(QByteArrayLiteral("\x1b[?2026l")),
        "insert sync-release movement publishes the held row insertion");
    const term::Terminal_selection_result insert_retained_payload =
        insert_session->selected_text();
    const std::optional<term::Terminal_render_snapshot> insert_moved_snapshot =
        insert_session->latest_render_snapshot();
    ok &= check(insert_retained_payload.code == term::Terminal_selection_result_code::OK &&
        insert_retained_payload.text == QStringLiteral("beta"),
        "insert sync-release movement keeps the finalized copy payload");
    ok &= check(insert_moved_snapshot.has_value() &&
        snapshot_row_text(*insert_moved_snapshot, 2) == QStringLiteral("beta"),
        "insert sync-release movement snapshot keeps the retained text at its moved row");
    ok &= check(!insert_session->selection_visual_lease().has_value(),
        "insert sync-release movement detaches the stale visual lease");
    ok &= check(insert_moved_snapshot.has_value() &&
        insert_moved_snapshot->selection_spans.empty(),
        "insert sync-release movement suppresses stale spans");

    std::unique_ptr<term::Terminal_session> delete_session;
    Scripted_backend* delete_backend = make_session(delete_session);
    ok &= check(delete_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "delete sync-release movement selection session starts");
    ok &= check(delete_backend->emit_output(QByteArrayLiteral("alpha\r\nbeta\r\ngamma")),
        "delete sync-release movement backend emits visible rows");

    delete_session->set_selection_range({
        { 2, 0 },
        { 2, 5 },
        term::Terminal_selection_mode::NORMAL,
    });
    const term::Terminal_selection_result delete_original_payload =
        delete_session->selected_text();
    const std::optional<term::Terminal_render_snapshot> delete_selected_snapshot =
        delete_session->latest_render_snapshot();
    ok &= check(delete_original_payload.code == term::Terminal_selection_result_code::OK &&
        delete_original_payload.text == QStringLiteral("gamma"),
        "delete sync-release movement selection captures the original payload");
    ok &= check(delete_selected_snapshot.has_value() &&
        snapshot_has_selection_span(*delete_selected_snapshot, 2, 0, 5),
        "delete sync-release movement initially publishes a visible span");

    ok &= check(delete_backend->emit_output(QByteArrayLiteral("\x1b[?2026h\x1b[2;1H\x1b[M")),
        "delete sync-release movement deletes a line above the selection while unpublished");
    ok &= check(delete_backend->emit_output(QByteArrayLiteral("\x1b[?2026l")),
        "delete sync-release movement publishes the held row deletion");
    const term::Terminal_selection_result delete_retained_payload =
        delete_session->selected_text();
    const std::optional<term::Terminal_render_snapshot> delete_moved_snapshot =
        delete_session->latest_render_snapshot();
    ok &= check(delete_retained_payload.code == term::Terminal_selection_result_code::OK &&
        delete_retained_payload.text == QStringLiteral("gamma"),
        "delete sync-release movement keeps the finalized copy payload");
    ok &= check(delete_moved_snapshot.has_value() &&
        snapshot_row_text(*delete_moved_snapshot, 1) == QStringLiteral("gamma"),
        "delete sync-release movement snapshot keeps the retained text at its moved row");
    ok &= check(!delete_session->selection_visual_lease().has_value(),
        "delete sync-release movement detaches the stale visual lease");
    ok &= check(delete_moved_snapshot.has_value() &&
        delete_moved_snapshot->selection_spans.empty(),
        "delete sync-release movement suppresses stale spans");

    return ok;
}

bool test_selection_spans_fail_closed_at_phase4c_boundaries()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> alternate_session;
    Scripted_backend* alternate_backend = make_session(alternate_session);
    ok &= check(alternate_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "Phase 4c active-buffer boundary session starts");
    ok &= check(alternate_backend->emit_output(QByteArrayLiteral("primary")),
        "Phase 4c active-buffer boundary backend emits primary text");

    alternate_session->set_selection_range({
        { 0, 0 },
        { 0, 4 },
        term::Terminal_selection_mode::NORMAL,
    });
    const term::Terminal_selection_result alternate_original_payload =
        alternate_session->selected_text();
    const std::optional<term::Terminal_render_snapshot> alternate_selected_snapshot =
        alternate_session->latest_render_snapshot();
    ok &= check(alternate_original_payload.code == term::Terminal_selection_result_code::OK &&
        alternate_original_payload.text == QStringLiteral("prim"),
        "Phase 4c active-buffer boundary captures the primary payload");
    ok &= check(alternate_selected_snapshot.has_value() &&
        snapshot_has_selection_span(*alternate_selected_snapshot, 0, 0, 4),
        "Phase 4c active-buffer boundary initially publishes a primary span");

    ok &= check(alternate_backend->emit_output(QByteArrayLiteral("\x1b[?1049halternate")),
        "Phase 4c active-buffer boundary enters alternate screen");
    const term::Terminal_selection_result alternate_hidden_payload =
        alternate_session->selected_text();
    const std::optional<term::Terminal_render_snapshot> alternate_snapshot =
        alternate_session->latest_render_snapshot();
    ok &= check(alternate_hidden_payload.code == term::Terminal_selection_result_code::OK &&
        alternate_hidden_payload.text == QStringLiteral("prim"),
        "Phase 4c active-buffer boundary keeps the finalized primary payload in alternate");
    ok &= check(!alternate_session->selection_visual_lease().has_value(),
        "Phase 4c active-buffer boundary detaches the visual lease on alternate entry");
    ok &= check(alternate_snapshot.has_value() &&
        alternate_snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE &&
        alternate_snapshot->selection_spans.empty(),
        "Phase 4c active-buffer boundary emits no primary spans in alternate");

    ok &= check(alternate_backend->emit_output(QByteArrayLiteral("\x1b[?1049l")),
        "Phase 4c active-buffer boundary returns to primary screen");
    const term::Terminal_selection_result alternate_return_payload =
        alternate_session->selected_text();
    const std::optional<term::Terminal_render_snapshot> alternate_return_snapshot =
        alternate_session->latest_render_snapshot();
    ok &= check(alternate_return_payload.code == term::Terminal_selection_result_code::OK &&
        alternate_return_payload.text == QStringLiteral("prim"),
        "Phase 4c active-buffer boundary keeps the finalized payload after return");
    ok &= check(!alternate_session->selection_visual_lease().has_value(),
        "Phase 4c active-buffer boundary does not reattach after returning to primary");
    ok &= check(alternate_return_snapshot.has_value() &&
        alternate_return_snapshot->viewport.active_buffer == term::Terminal_buffer_id::PRIMARY &&
        alternate_return_snapshot->selection_spans.empty(),
        "Phase 4c active-buffer boundary emits no stale spans after return");

    std::unique_ptr<term::Terminal_session> reverse_alternate_session;
    Scripted_backend* reverse_alternate_backend =
        make_session(reverse_alternate_session);
    ok &= check(reverse_alternate_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "Phase 4c reverse active-buffer boundary session starts");
    ok &= check(reverse_alternate_backend->emit_output(QByteArrayLiteral("primary")),
        "Phase 4c reverse active-buffer boundary emits primary text");
    ok &= check(reverse_alternate_backend->emit_output(QByteArrayLiteral("\x1b[?1049halternate")),
        "Phase 4c reverse active-buffer boundary enters alternate screen");

    reverse_alternate_session->set_selection_range({
        { 0, 0 },
        { 0, 4 },
        term::Terminal_selection_mode::NORMAL,
    });
    const term::Terminal_selection_result reverse_alternate_payload =
        reverse_alternate_session->selected_text();
    const std::optional<term::terminal_selection_visual_lease_t>
        reverse_alternate_attached_lease =
            reverse_alternate_session->selection_visual_lease();
    const std::optional<term::Terminal_render_snapshot> reverse_alternate_snapshot =
        reverse_alternate_session->latest_render_snapshot();
    ok &= check(reverse_alternate_payload.code == term::Terminal_selection_result_code::OK &&
        reverse_alternate_payload.text == QStringLiteral("alte"),
        "Phase 4c reverse active-buffer boundary captures the alternate payload");
    ok &= check(reverse_alternate_attached_lease.has_value() &&
        reverse_alternate_attached_lease->buffer_id == term::Terminal_buffer_id::ALTERNATE,
        "Phase 4c reverse active-buffer boundary attaches an alternate visual lease");
    ok &= check(reverse_alternate_snapshot.has_value() &&
        reverse_alternate_snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE &&
        snapshot_has_selection_span(*reverse_alternate_snapshot, 0, 0, 4),
        "Phase 4c reverse active-buffer boundary initially publishes an alternate span");

    ok &= check(reverse_alternate_backend->emit_output(QByteArrayLiteral("\x1b[?1049l")),
        "Phase 4c reverse active-buffer boundary returns to primary screen");
    const term::Terminal_selection_result reverse_alternate_return_payload =
        reverse_alternate_session->selected_text();
    const std::optional<term::Terminal_render_snapshot> reverse_alternate_return_snapshot =
        reverse_alternate_session->latest_render_snapshot();
    ok &= check(reverse_alternate_return_payload.code ==
            term::Terminal_selection_result_code::OK &&
        reverse_alternate_return_payload.text == QStringLiteral("alte"),
        "Phase 4c reverse active-buffer boundary keeps the finalized alternate payload");
    ok &= check(!reverse_alternate_session->selection_visual_lease().has_value(),
        "Phase 4c reverse active-buffer boundary clears the alternate visual lease");
    ok &= check(reverse_alternate_return_snapshot.has_value() &&
        reverse_alternate_return_snapshot->viewport.active_buffer ==
            term::Terminal_buffer_id::PRIMARY &&
        reverse_alternate_return_snapshot->selection_spans.empty(),
        "Phase 4c reverse active-buffer boundary emits no stale primary spans after return");

    std::unique_ptr<term::Terminal_session> synchronized_alternate_session;
    Scripted_backend* synchronized_alternate_backend =
        make_session(synchronized_alternate_session);
    ok &= check(synchronized_alternate_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "Phase 4c synchronized active-buffer boundary session starts");
    ok &= check(synchronized_alternate_backend->emit_output(QByteArrayLiteral("primary")),
        "Phase 4c synchronized active-buffer boundary emits primary text");

    synchronized_alternate_session->set_selection_range({
        { 0, 0 },
        { 0, 4 },
        term::Terminal_selection_mode::NORMAL,
    });
    const std::optional<term::terminal_selection_visual_lease_t>
        synchronized_alternate_attached_lease =
            synchronized_alternate_session->selection_visual_lease();
    ok &= check(synchronized_alternate_attached_lease.has_value() &&
        synchronized_alternate_attached_lease->buffer_id == term::Terminal_buffer_id::PRIMARY,
        "Phase 4c synchronized active-buffer boundary starts with an attached visual lease");
    ok &= check(synchronized_alternate_backend->emit_output(
            QByteArrayLiteral("\x1b[?2026h\x1b[?1049halternate\x1b[?1049l\x1b[?2026l")),
        "Phase 4c synchronized active-buffer boundary releases an alternate round trip");
    const term::Terminal_selection_result synchronized_alternate_payload =
        synchronized_alternate_session->selected_text();
    const std::optional<term::Terminal_render_snapshot> synchronized_alternate_snapshot =
        synchronized_alternate_session->latest_render_snapshot();
    ok &= check(synchronized_alternate_payload.code == term::Terminal_selection_result_code::OK &&
        synchronized_alternate_payload.text == QStringLiteral("prim"),
        "Phase 4c synchronized active-buffer boundary keeps the finalized payload");
    ok &= check(!synchronized_alternate_session->selection_visual_lease().has_value(),
        "Phase 4c synchronized active-buffer boundary detaches despite matching primary provenance");
    ok &= check(synchronized_alternate_snapshot.has_value() &&
        synchronized_alternate_snapshot->viewport.active_buffer == term::Terminal_buffer_id::PRIMARY &&
        synchronized_alternate_snapshot->selection_spans.empty(),
        "Phase 4c synchronized active-buffer boundary emits no stale returned primary spans");

    std::unique_ptr<term::Terminal_session> region_session;
    Scripted_backend* region_backend = make_session(region_session);
    ok &= check(region_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "Phase 4c partial-region discard session starts");
    ok &= check(region_backend->emit_output(QByteArrayLiteral("alpha\r\nbeta\r\ngamma\r\ndelta")),
        "Phase 4c partial-region discard backend emits visible rows");

    region_session->set_selection_range({
        { 1, 0 },
        { 1, 4 },
        term::Terminal_selection_mode::NORMAL,
    });
    const term::Terminal_selection_result region_original_payload =
        region_session->selected_text();
    const std::optional<term::Terminal_render_snapshot> region_selected_snapshot =
        region_session->latest_render_snapshot();
    ok &= check(region_original_payload.code == term::Terminal_selection_result_code::OK &&
        region_original_payload.text == QStringLiteral("beta"),
        "Phase 4c partial-region discard captures the selected row payload");
    ok &= check(region_selected_snapshot.has_value() &&
        snapshot_has_selection_span(*region_selected_snapshot, 1, 0, 4),
        "Phase 4c partial-region discard initially publishes a visible span");

    ok &= check(region_backend->emit_output(
            QByteArrayLiteral("\x1b[2;3r\x1b[3;1H\x1b" "D\x1b[r")),
        "Phase 4c partial-region discard scrolls the selected row out of the region");
    const term::Terminal_selection_result region_retained_payload =
        region_session->selected_text();
    const std::optional<term::Terminal_render_snapshot> region_discard_snapshot =
        region_session->latest_render_snapshot();
    ok &= check(region_retained_payload.code == term::Terminal_selection_result_code::OK &&
        region_retained_payload.text == QStringLiteral("beta"),
        "Phase 4c partial-region discard keeps the finalized copy payload");
    ok &= check(!region_session->selection_visual_lease().has_value(),
        "Phase 4c partial-region discard detaches the stale visual lease");
    ok &= check(region_discard_snapshot.has_value() &&
        snapshot_row_text(*region_discard_snapshot, 1) == QStringLiteral("gamma"),
        "Phase 4c partial-region discard moves replacement content into the selected row");
    ok &= check(region_discard_snapshot.has_value() &&
        region_discard_snapshot->selection_spans.empty(),
        "Phase 4c partial-region discard suppresses stale spans");

    std::unique_ptr<term::Terminal_session> clear_scrollback_session;
    Scripted_backend* clear_scrollback_backend = make_session(clear_scrollback_session);
    ok &= check(clear_scrollback_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "Phase 4c clear-scrollback boundary session starts");
    ok &= check(clear_scrollback_backend->emit_output(numbered_scroll_lines(80)),
        "Phase 4c clear-scrollback boundary backend creates scrollback");
    ok &= check(clear_scrollback_session->scroll_viewport_lines(3).action ==
        term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "Phase 4c clear-scrollback boundary exposes scrollback rows");

    const std::optional<term::Terminal_render_snapshot> clear_scrollback_source_snapshot =
        clear_scrollback_session->latest_render_snapshot();
    if (clear_scrollback_source_snapshot.has_value()) {
        const int first_visible_logical_row =
            clear_scrollback_source_snapshot->viewport.scrollback_rows -
            clear_scrollback_source_snapshot->viewport.offset_from_tail;
        const int selection_row = first_visible_logical_row + 1;
        const QString expected_text =
            QStringLiteral("scroll-line-%1")
                .arg(selection_row, 3, 10, QLatin1Char('0'));
        clear_scrollback_session->set_selection_range({
            { selection_row, 0 },
            { selection_row, 15 },
            term::Terminal_selection_mode::NORMAL,
        });
        const term::Terminal_selection_result clear_scrollback_original_payload =
            clear_scrollback_session->selected_text();
        const std::optional<term::Terminal_render_snapshot> clear_scrollback_selected_snapshot =
            clear_scrollback_session->latest_render_snapshot();
        ok &= check(clear_scrollback_original_payload.code ==
                term::Terminal_selection_result_code::OK &&
            clear_scrollback_original_payload.text == expected_text,
            "Phase 4c clear-scrollback boundary captures the scrollback payload");
        ok &= check(clear_scrollback_selected_snapshot.has_value() &&
            snapshot_has_selection_span(*clear_scrollback_selected_snapshot, 1, 0, 15),
            "Phase 4c clear-scrollback boundary initially publishes a scrollback span");

        ok &= check(clear_scrollback_backend->emit_output(QByteArrayLiteral("\x1b[3J")),
            "Phase 4c clear-scrollback boundary clears retained scrollback");
        const term::Terminal_selection_result clear_scrollback_retained_payload =
            clear_scrollback_session->selected_text();
        const std::optional<term::Terminal_render_snapshot> clear_scrollback_cleared_snapshot =
            clear_scrollback_session->latest_render_snapshot();
        ok &= check(clear_scrollback_retained_payload.code ==
                term::Terminal_selection_result_code::OK &&
            clear_scrollback_retained_payload.text == expected_text,
            "Phase 4c clear-scrollback boundary keeps the finalized copy payload");
        ok &= check(!clear_scrollback_session->selection_visual_lease().has_value(),
            "Phase 4c clear-scrollback boundary detaches the purged retained-line proof");
        ok &= check(clear_scrollback_cleared_snapshot.has_value() &&
            clear_scrollback_cleared_snapshot->selection_spans.empty(),
            "Phase 4c clear-scrollback boundary suppresses purged scrollback spans");
    }
    else {
        ok &= check(false, "Phase 4c clear-scrollback boundary source snapshot is available");
    }

    return ok;
}

bool test_selection_spans_detach_when_resize_invalidates_selected_columns()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "resize-invalidated selection session starts");
    ok &= check(backend->emit_output(
        QByteArrayLiteral("0123456789012345678901234567890123456789"
                          "012345678901234567890123456789")),
        "resize-invalidated backend emits selectable text beyond the resized grid");

    session->set_selection_range({
        { 0, 50 },
        { 0, 70 },
        term::Terminal_selection_mode::NORMAL,
    });
    const term::Terminal_selection_result original_payload =
        session->selected_text();
    ok &= check(original_payload.code == term::Terminal_selection_result_code::OK &&
        original_payload.text == QStringLiteral("01234567890123456789"),
        "resize-invalidated selection captures original payload");

    const term::Terminal_session_result resize_result =
        session->resize(QSizeF(400.0, 240.0), {24, 40});
    const term::Terminal_selection_result retained_payload =
        session->selected_text();
    const std::optional<term::Terminal_render_snapshot> resized_snapshot =
        session->latest_render_snapshot();
    ok &= check(resize_result.code == term::Terminal_session_result_code::ACCEPTED,
        "resize-invalidated incompatible column resize is accepted");
    ok &= check(retained_payload.code == term::Terminal_selection_result_code::OK &&
        retained_payload.text == QStringLiteral("01234567890123456789"),
        "resize-invalidated incompatible resize retains copy payload");
    ok &= check(resized_snapshot.has_value() &&
        resized_snapshot->grid_size.columns == 40,
        "resize-invalidated snapshot records resized grid");
    ok &= check(resized_snapshot.has_value() &&
        resized_snapshot->selection_spans.empty(),
        "resize-invalidated columns emit no stale spans");
    ok &= check(!session->selection_visual_lease().has_value(),
        "resize-invalidated columns clear retained-line visual proof");

    return ok;
}

bool test_selection_unicode_cluster_payloads()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> wide_continuation_session;
    Scripted_backend* wide_continuation_backend =
        make_session(wide_continuation_session);
    ok &= check(wide_continuation_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "wide-continuation selection session starts");
    ok &= check(wide_continuation_backend->emit_output(
        QByteArrayLiteral("A") + QStringLiteral("\u754c").toUtf8() +
        QByteArrayLiteral("B")),
        "wide-continuation backend emits wide glyph text");
    wide_continuation_session->set_selection_range({
        { 0, 2 },
        { 0, 3 },
        term::Terminal_selection_mode::NORMAL,
    });
    const term::Terminal_selection_result trailing_wide_text =
        wide_continuation_session->selected_text();
    ok &= check(trailing_wide_text.code == term::Terminal_selection_result_code::OK &&
        trailing_wide_text.text.isEmpty(),
        "trailing wide continuation cell is not copied as independent text");

    std::unique_ptr<term::Terminal_session> combining_session;
    Scripted_backend* combining_backend = make_session(combining_session);
    ok &= check(combining_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "combining-mark selection session starts");
    ok &= check(combining_backend->emit_output(
        QStringLiteral("e\u0301x").toUtf8()),
        "combining-mark backend emits composed cell text");
    combining_session->set_selection_range({
        { 0, 0 },
        { 0, 1 },
        term::Terminal_selection_mode::NORMAL,
    });
    const term::Terminal_selection_result combining_text =
        combining_session->selected_text();
    ok &= check(combining_text.code == term::Terminal_selection_result_code::OK &&
        combining_text.text == QStringLiteral("e\u0301"),
        "combining mark remains attached to the selected base cell");

    std::unique_ptr<term::Terminal_session> variation_session;
    Scripted_backend* variation_backend = make_session(variation_session);
    ok &= check(variation_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "variation selection session starts");
    ok &= check(variation_backend->emit_output(
        QStringLiteral("\u2764\ufe0fx").toUtf8()),
        "variation backend emits variation-sequence text");
    variation_session->set_selection_range({
        { 0, 0 },
        { 0, 2 },
        term::Terminal_selection_mode::NORMAL,
    });
    const term::Terminal_selection_result variation_text =
        variation_session->selected_text();
    ok &= check(variation_text.code == term::Terminal_selection_result_code::OK &&
        variation_text.text == QStringLiteral("\u2764\ufe0f"),
        "variation selector remains attached to the selected base cell");

    return ok;
}

bool test_output_activity_notifications_are_session_level()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "output-activity session starts");

    ok &= check(backend->emit_output(QByteArrayLiteral("A\a")),
        "output-activity backend emits printable text and bell");
    const std::optional<std::size_t> activity_index = first_notification_index(
        *session,
        term::Terminal_session_notification_kind::OUTPUT_ACTIVITY);
    const std::optional<std::size_t> bell_index = first_notification_index(
        *session,
        term::Terminal_session_notification_kind::BELL_REQUESTED);
    const std::optional<std::size_t> snapshot_index = first_notification_index(
        *session,
        term::Terminal_session_notification_kind::SNAPSHOT_READY);
    ok &= check(activity_index.has_value() &&
        bell_index.has_value() &&
        snapshot_index.has_value() &&
        *activity_index < *bell_index &&
        *bell_index < *snapshot_index,
        "output activity precedes parser notifications and snapshot publication");

    const std::vector<term::Terminal_session_notification> first_activity_notifications = notifications_of_kind(
        *session,
        term::Terminal_session_notification_kind::OUTPUT_ACTIVITY);
    ok &= check(first_activity_notifications.size() == 1U &&
        first_activity_notifications[0].sequence == session->last_processed_sequence(),
        "first backend output emits exactly one session-level output activity");

    const std::size_t   activity_count_after_first = first_activity_notifications.size();
    const std::uint64_t generation_after_first     = session->render_snapshot_generation();
    ok                                            &= check(backend->emit_output(QByteArray()),
        "empty backend output command is accepted by scripted backend");
    ok                                            &= check(notification_count(
        *session,
        term::Terminal_session_notification_kind::OUTPUT_ACTIVITY) ==
        activity_count_after_first,
        "empty backend output command does not emit output activity");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026hB")),
        "output-activity backend enters synchronized output");
    const std::uint64_t held_generation = session->render_snapshot_generation();
    ok &= check(held_generation == generation_after_first,
        "synchronized-output entry does not publish held output");
    ok &= check(backend->emit_output(QByteArrayLiteral("C")),
        "output-activity backend emits held synchronized output");
    ok &= check(notification_count(
        *session,
        term::Terminal_session_notification_kind::OUTPUT_ACTIVITY) ==
        activity_count_after_first + 2U,
        "held synchronized-output command emits activity without snapshot publication");
    ok &= check(session->render_snapshot_generation() == held_generation,
        "held synchronized-output activity does not depend on snapshot publication");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026l")),
        "output-activity backend releases synchronized output");
    ok &= check(notification_count(
        *session,
        term::Terminal_session_notification_kind::OUTPUT_ACTIVITY) ==
        activity_count_after_first + 3U,
        "release command emits one additional output activity notification");

    std::unique_ptr<term::Terminal_session> invalid_session;
    make_session(invalid_session);
    const term::Terminal_session_result invalid_result =
        invalid_session->write_user_bytes(QByteArrayLiteral("not-running"));
    ok &= check(invalid_result.code == term::Terminal_session_result_code::INVALID_STATE,
        "invalid session command remains rejected");
    ok &= check(notification_count(
        *invalid_session,
        term::Terminal_session_notification_kind::OUTPUT_ACTIVITY) == 0U,
        "invalid non-output path does not emit output activity");

    std::unique_ptr<term::Terminal_session> overflow_session;
    Scripted_backend* overflow_backend =
        make_session(overflow_session, tight_session_config());
    ok &= check(overflow_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "output-activity overflow session starts");
    ok &= check(overflow_backend->emit_output(QByteArrayLiteral("123456789")),
        "output-activity overflow backend emits rejected chunk");
    ok &= check(notification_count(
        *overflow_session,
        term::Terminal_session_notification_kind::OUTPUT_ACTIVITY) == 0U,
        "rejected backend output overflow does not emit output activity");

    return ok;
}

bool test_synchronized_output_defers_content_until_release()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "synchronized-output session starts");

    ok &= check(backend->emit_output(QByteArrayLiteral("A\x1b[?2026hB")),
        "synchronized-output fake backend emits visible prefix and enters sync");
    const std::optional<term::Terminal_render_snapshot> prefix_snapshot =
        session->latest_render_snapshot();
    const std::uint64_t prefix_generation = session->render_snapshot_generation();
    ok &= check(prefix_snapshot.has_value() &&
        snapshot_row_text(*prefix_snapshot, 0) == QStringLiteral("A"),
        "visible prefix before synchronized output publishes from the same chunk");
    const std::optional<term::Terminal_render_snapshot> held_snapshot =
        session->latest_render_snapshot();
    ok &= check(held_snapshot.has_value() &&
        snapshot_row_text(*held_snapshot, 0) == QStringLiteral("A") &&
        session->render_snapshot_generation() == prefix_generation,
        "synchronized output does not publish hidden text in latest snapshot");

    ok &= check(backend->emit_output(QByteArrayLiteral("C")),
        "fake backend continues synchronized output");
    ok &= check(session->render_snapshot_generation() == prefix_generation,
        "continued synchronized output keeps render publication held");

    ok &= check(session->resize(QSizeF(1000.0, 500.0), {25, 100}).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "resize during synchronized output is accepted");
    const std::optional<term::Terminal_render_snapshot> resized_held_snapshot =
        session->latest_render_snapshot();
    ok &= check(resized_held_snapshot.has_value() &&
        prefix_snapshot.has_value() &&
        resized_held_snapshot->grid_size.rows == 25 &&
        resized_held_snapshot->grid_size.columns == 100 &&
        snapshot_row_text(*resized_held_snapshot, 0) == QStringLiteral("A") &&
        !snapshot_contains_text(*resized_held_snapshot, QStringLiteral("B")) &&
        !snapshot_contains_text(*resized_held_snapshot, QStringLiteral("C")) &&
        session->render_snapshot_generation() == prefix_generation + 1U,
        "resize during synchronized output publishes geometry without hidden content");
    ok &= check(resized_held_snapshot.has_value() &&
        resized_held_snapshot->dirty_row_ranges.size() == 1U &&
        resized_held_snapshot->dirty_row_ranges.front().first_row == 0 &&
        resized_held_snapshot->dirty_row_ranges.front().row_count == 25,
        "synchronized-output resize geometry snapshot requests public repaint");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026lD")),
        "fake backend releases synchronized output");
    const std::optional<term::Terminal_render_snapshot> released_snapshot =
        session->latest_render_snapshot();
    ok &= check(released_snapshot.has_value() &&
        released_snapshot->grid_size.rows == 25 &&
        released_snapshot->grid_size.columns == 100 &&
        snapshot_row_text(*released_snapshot, 0) == QStringLiteral("ABCD") &&
        session->render_snapshot_generation() == prefix_generation + 2U,
        "synchronized output release publishes accumulated model state");
    const std::optional<term::Terminal_screen_model_result> released_result =
        session->last_model_ingest_result();
    ok &= check(released_result.has_value() &&
        released_result->viewport_changed &&
        released_result->dirty_rows.size() == 25U,
        "synchronized output release preserves resize invalidation");
    ok &= check(released_snapshot.has_value() &&
        released_snapshot->dirty_row_ranges.size() == 1U &&
        released_snapshot->dirty_row_ranges.front().first_row == 0 &&
        released_snapshot->dirty_row_ranges.front().row_count == 25,
        "synchronized output release publishes preserved resize invalidation");

    std::unique_ptr<term::Terminal_session> not_started_force_release_session;
    make_session(not_started_force_release_session);
    ok &= check(
        not_started_force_release_session->force_release_synchronized_output().code ==
            term::Terminal_session_result_code::INVALID_STATE,
        "force-release before start is rejected");

    std::unique_ptr<term::Terminal_session> force_release_session;
    Scripted_backend* force_release_backend = make_session(force_release_session);
    ok &= check(force_release_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "force-release synchronized-output session starts");
    ok &= check(force_release_backend->emit_output(QByteArrayLiteral("\x1b[?2026hXY")),
        "force-release session enters synchronized output with hidden text");
    const std::uint64_t force_held_generation =
        force_release_session->render_snapshot_generation();
    const std::optional<term::Terminal_render_snapshot> force_held_snapshot =
        force_release_session->latest_render_snapshot();
    ok &= check(!force_held_snapshot.has_value() ||
        snapshot_row_text(*force_held_snapshot, 0).isEmpty(),
        "force-release session holds synchronized output before explicit release");

    const term::Terminal_session_result force_result =
        force_release_session->force_release_synchronized_output();
    const std::optional<term::Terminal_render_snapshot> force_snapshot =
        force_release_session->latest_render_snapshot();
    ok &= check(force_result.code == term::Terminal_session_result_code::ACCEPTED,
        "force-release synchronized-output command is accepted");
    ok &= check(force_snapshot.has_value() &&
        snapshot_row_text(*force_snapshot, 0) == QStringLiteral("XY") &&
        !force_snapshot->modes.synchronized_output &&
        force_release_session->render_snapshot_generation() == force_held_generation + 1U,
        "force-release synchronized-output publishes accumulated damage once");

    const std::uint64_t force_released_generation =
        force_release_session->render_snapshot_generation();
    ok &= check(force_release_session->force_release_synchronized_output().code ==
        term::Terminal_session_result_code::ACCEPTED,
        "second force-release without synchronized output is accepted as a no-op");
    ok &= check(force_release_session->render_snapshot_generation() ==
        force_released_generation,
        "second force-release does not publish another snapshot");

    std::unique_ptr<term::Terminal_session> ordered_force_release_session;
    Scripted_backend* ordered_force_release_backend =
        make_session(ordered_force_release_session);
    ok &= check(ordered_force_release_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "ordered force-release synchronized-output session starts");

    term::Terminal_session_result ordered_force_result;
    bool ordered_force_called = false;
    ordered_force_release_backend->outputs_during_write       = {
        QByteArrayLiteral("\x1b[?2026hXY"),
    };
    ordered_force_release_backend->after_outputs_during_write = [&]() {
        ordered_force_called = true;
        ordered_force_result =
            ordered_force_release_session->force_release_synchronized_output();
    };

    ok &= check(ordered_force_release_session->write_user_bytes(
        QByteArrayLiteral("release-after-output")).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "ordered force-release trigger write is accepted");
    ok &= check(ordered_force_called &&
        ordered_force_result.code == term::Terminal_session_result_code::ACCEPTED,
        "ordered force-release call inside backend callback lifetime is accepted");

    const std::vector<term::Terminal_session_command> ordered_force_commands =
        ordered_force_release_session->processed_commands();
    ok &= check(ordered_force_commands.size() == 4U &&
        ordered_force_commands[0].kind == term::Terminal_session_command_kind::START &&
        ordered_force_commands[1].kind == term::Terminal_session_command_kind::USER_WRITE &&
        ordered_force_commands[2].kind ==
            term::Terminal_session_command_kind::BACKEND_OUTPUT &&
        ordered_force_commands[3].kind ==
            term::Terminal_session_command_kind::FORCE_RELEASE_SYNCHRONIZED_OUTPUT &&
        ordered_force_commands[2].sequence < ordered_force_commands[3].sequence,
        "force-release command is ordered after pending backend callback output");
    ok &= check(!ordered_force_commands.empty() &&
        ordered_force_release_session->last_processed_sequence() ==
            ordered_force_commands.back().sequence,
        "force-release command advances last processed sequence");

    const std::optional<term::Terminal_render_snapshot> ordered_force_snapshot =
        ordered_force_release_session->latest_render_snapshot();
    ok &= check(ordered_force_snapshot.has_value() &&
        snapshot_row_text(*ordered_force_snapshot, 0) == QStringLiteral("XY") &&
        !ordered_force_snapshot->modes.synchronized_output &&
        ordered_force_snapshot->metadata.sequence ==
            ordered_force_commands.back().sequence,
        "ordered force-release publishes output that was already queued by callback");

    std::unique_ptr<term::Terminal_session> same_csi_session;
    Scripted_backend* same_csi_backend = make_session(same_csi_session);
    ok &= check(same_csi_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "same-CSI synchronized-output session starts");

    ok &= check(same_csi_backend->emit_output(QByteArrayLiteral("\x1b[?1049;2026hX")),
        "same-CSI output enters alternate screen and synchronized output");
    const std::optional<term::Terminal_render_snapshot> same_csi_snapshot =
        same_csi_session->latest_render_snapshot();
    ok &= check(same_csi_snapshot.has_value() &&
        same_csi_snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE &&
        snapshot_row_text(*same_csi_snapshot, 0).isEmpty() &&
        same_csi_session->render_snapshot_generation() == 1U,
        "same-CSI pre-sync alternate-screen change publishes without hidden text");

    ok &= check(same_csi_backend->emit_output(QByteArrayLiteral("\x1b[?2026l")),
        "same-CSI synchronized output releases hidden alternate-screen text");
    const std::optional<term::Terminal_render_snapshot> same_csi_release =
        same_csi_session->latest_render_snapshot();
    ok &= check(same_csi_release.has_value() &&
        snapshot_row_text(*same_csi_release, 0) == QStringLiteral("X") &&
        same_csi_session->render_snapshot_generation() == 2U,
        "same-CSI release publishes hidden text after synchronization ends");

    std::unique_ptr<term::Terminal_session> post_sync_mode_session;
    Scripted_backend* post_sync_mode_backend = make_session(post_sync_mode_session);
    ok &= check(post_sync_mode_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "post-sync-mode same-CSI session starts");
    ok &= check(post_sync_mode_backend->emit_output(
        QByteArrayLiteral("\x1b[?1049;2026;1000hX")),
        "same-CSI output applies a mode after synchronized-output entry");
    const std::optional<term::Terminal_render_snapshot> post_sync_mode_snapshot =
        post_sync_mode_session->latest_render_snapshot();
    ok &= check(post_sync_mode_snapshot.has_value() &&
        post_sync_mode_snapshot->viewport.active_buffer ==
            term::Terminal_buffer_id::ALTERNATE &&
        post_sync_mode_snapshot->modes.mouse_tracking ==
            term::Terminal_mouse_tracking_mode::NONE &&
        snapshot_row_text(*post_sync_mode_snapshot, 0).isEmpty() &&
        post_sync_mode_session->render_snapshot_generation() == 1U,
        "same-CSI post-2026 mode does not leak into boundary snapshot");
    ok &= check(post_sync_mode_backend->emit_output(QByteArrayLiteral("\x1b[?2026l")),
        "post-sync-mode same-CSI session releases synchronized output");
    const std::optional<term::Terminal_render_snapshot> post_sync_mode_release =
        post_sync_mode_session->latest_render_snapshot();
    ok &= check(post_sync_mode_release.has_value() &&
        post_sync_mode_release->modes.mouse_tracking ==
            term::Terminal_mouse_tracking_mode::BUTTON &&
        snapshot_row_text(*post_sync_mode_release, 0) == QStringLiteral("X") &&
        post_sync_mode_session->render_snapshot_generation() == 2U,
        "same-CSI post-2026 mode publishes only after synchronization ends");

    std::unique_ptr<term::Terminal_session> split_same_csi_session;
    Scripted_backend* split_same_csi_backend = make_session(split_same_csi_session);
    ok &= check(split_same_csi_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "split same-CSI synchronized-output session starts");
    ok &= check(split_same_csi_backend->emit_output(QByteArrayLiteral("\x1b[?1049;")),
        "split same-CSI backend emits incomplete private mode sequence");
    ok &= check(!split_same_csi_session->latest_render_snapshot().has_value(),
        "incomplete same-CSI synchronized-output sequence waits for completion");
    ok &= check(split_same_csi_backend->emit_output(QByteArrayLiteral("2026hX")),
        "split same-CSI backend completes private mode sequence with hidden text");
    const std::optional<term::Terminal_render_snapshot> split_same_csi_snapshot =
        split_same_csi_session->latest_render_snapshot();
    ok &= check(split_same_csi_snapshot.has_value() &&
        split_same_csi_snapshot->viewport.active_buffer ==
            term::Terminal_buffer_id::ALTERNATE &&
        snapshot_row_text(*split_same_csi_snapshot, 0).isEmpty() &&
        split_same_csi_session->render_snapshot_generation() == 1U,
        "split same-CSI pre-sync state publishes before hidden text");
    ok &= check(split_same_csi_backend->emit_output(QByteArrayLiteral("\x1b[?2026l")),
        "split same-CSI synchronized output releases hidden text");
    const std::optional<term::Terminal_render_snapshot> split_same_csi_release =
        split_same_csi_session->latest_render_snapshot();
    ok &= check(split_same_csi_release.has_value() &&
        snapshot_row_text(*split_same_csi_release, 0) == QStringLiteral("X") &&
        split_same_csi_session->render_snapshot_generation() == 2U,
        "split same-CSI release publishes hidden text after synchronization ends");

    std::unique_ptr<term::Terminal_session> split_post_mode_session;
    Scripted_backend* split_post_mode_backend = make_session(split_post_mode_session);
    ok &= check(split_post_mode_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "split post-mode same-CSI synchronized-output session starts");
    ok &= check(split_post_mode_backend->emit_output(QByteArrayLiteral("\x1b[?1049;2026;")),
        "split post-mode same-CSI backend emits incomplete sequence");
    ok &= check(split_post_mode_backend->emit_output(QByteArrayLiteral("1000hX")),
        "split post-mode same-CSI backend completes sequence with hidden mode and text");
    const std::optional<term::Terminal_render_snapshot> split_post_mode_snapshot =
        split_post_mode_session->latest_render_snapshot();
    ok &= check(split_post_mode_snapshot.has_value() &&
        split_post_mode_snapshot->viewport.active_buffer ==
            term::Terminal_buffer_id::ALTERNATE &&
        split_post_mode_snapshot->modes.mouse_tracking ==
            term::Terminal_mouse_tracking_mode::NONE &&
        snapshot_row_text(*split_post_mode_snapshot, 0).isEmpty() &&
        split_post_mode_session->render_snapshot_generation() == 1U,
        "split same-CSI post-2026 mode does not leak into boundary snapshot");
    ok &= check(split_post_mode_backend->emit_output(QByteArrayLiteral("\x1b[?2026l")),
        "split post-mode same-CSI synchronized output releases hidden text and mode");
    const std::optional<term::Terminal_render_snapshot> split_post_mode_release =
        split_post_mode_session->latest_render_snapshot();
    ok &= check(split_post_mode_release.has_value() &&
        split_post_mode_release->modes.mouse_tracking ==
            term::Terminal_mouse_tracking_mode::BUTTON &&
        snapshot_row_text(*split_post_mode_release, 0) == QStringLiteral("X") &&
        split_post_mode_session->render_snapshot_generation() == 2U,
        "split same-CSI post-2026 mode publishes only after synchronization ends");

    std::unique_ptr<term::Terminal_session> malformed_intermediate_session;
    Scripted_backend* malformed_intermediate_backend =
        make_session(malformed_intermediate_session);
    ok &= check(malformed_intermediate_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "malformed intermediate synchronized-output scanner session starts");
    ok &= check(malformed_intermediate_backend->emit_output(
        QByteArrayLiteral("\x1b[?1049;2026 hZ")),
        "malformed CSI with intermediate byte stays on parser recovery path");
    const std::optional<term::Terminal_render_snapshot> malformed_intermediate_snapshot =
        malformed_intermediate_session->latest_render_snapshot();
    ok &= check(malformed_intermediate_snapshot.has_value() &&
        malformed_intermediate_snapshot->viewport.active_buffer ==
            term::Terminal_buffer_id::PRIMARY &&
        snapshot_row_text(*malformed_intermediate_snapshot, 0) == QStringLiteral("Z"),
        "malformed CSI is not rewritten into actionable synchronized-output modes");

    std::unique_ptr<term::Terminal_session> malformed_colon_session;
    Scripted_backend* malformed_colon_backend = make_session(malformed_colon_session);
    ok &= check(malformed_colon_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "malformed colon synchronized-output scanner session starts");
    ok &= check(malformed_colon_backend->emit_output(QByteArrayLiteral("\x1b[?1049:2026hZ")),
        "malformed CSI with colon sub-parameter stays on parser recovery path");
    const std::optional<term::Terminal_render_snapshot> malformed_colon_snapshot =
        malformed_colon_session->latest_render_snapshot();
    ok &= check(malformed_colon_snapshot.has_value() &&
        malformed_colon_snapshot->viewport.active_buffer == term::Terminal_buffer_id::PRIMARY &&
        !malformed_colon_snapshot->modes.synchronized_output &&
        snapshot_row_text(*malformed_colon_snapshot, 0) == QStringLiteral("Z"),
        "colon-separated private modes are not rewritten into actionable modes");

    std::unique_ptr<term::Terminal_session> malformed_sync_suffix_session;
    Scripted_backend* malformed_sync_suffix_backend =
        make_session(malformed_sync_suffix_session);
    ok &= check(malformed_sync_suffix_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "malformed sync-suffix scanner session starts");
    ok &= check(malformed_sync_suffix_backend->emit_output(
        QByteArrayLiteral("\x1b[?1049;2026:1hZ")),
        "malformed 2026 sub-parameter suffix stays on parser recovery path");
    const std::optional<term::Terminal_render_snapshot> malformed_sync_suffix_snapshot =
        malformed_sync_suffix_session->latest_render_snapshot();
    ok &= check(malformed_sync_suffix_snapshot.has_value() &&
        malformed_sync_suffix_snapshot->viewport.active_buffer ==
            term::Terminal_buffer_id::PRIMARY &&
        !malformed_sync_suffix_snapshot->modes.synchronized_output &&
        snapshot_row_text(*malformed_sync_suffix_snapshot, 0) == QStringLiteral("Z"),
        "2026 sub-parameter suffix is not rewritten into synchronized-output modes");

    std::unique_ptr<term::Terminal_session> malformed_long_prefix_session;
    Scripted_backend* malformed_long_prefix_backend =
        make_session(malformed_long_prefix_session);
    ok &= check(malformed_long_prefix_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "malformed long-prefix scanner session starts");
    ok &= check(malformed_long_prefix_backend->emit_output(
        QByteArrayLiteral("\x1b[?9999999999;2026hZ")),
        "overlong parameter before 2026 stays on parser recovery path");
    const std::optional<term::Terminal_render_snapshot> malformed_long_prefix_snapshot =
        malformed_long_prefix_session->latest_render_snapshot();
    ok &= check(malformed_long_prefix_snapshot.has_value() &&
        malformed_long_prefix_snapshot->viewport.active_buffer ==
            term::Terminal_buffer_id::PRIMARY &&
        !malformed_long_prefix_snapshot->modes.synchronized_output &&
        snapshot_row_text(*malformed_long_prefix_snapshot, 0) == QStringLiteral("Z"),
        "overlong pre-2026 parameter is not rewritten into synchronized-output modes");

    std::unique_ptr<term::Terminal_session> malformed_group_count_session;
    Scripted_backend* malformed_group_count_backend =
        make_session(malformed_group_count_session);
    ok &= check(malformed_group_count_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "malformed group-count scanner session starts");
    QByteArray too_many_groups = QByteArrayLiteral("\x1b[?");
    for (std::size_t i = 0U; i < term::k_csi_parameter_group_limit; ++i) {
        if (i > 0U) {
            too_many_groups.append(';');
        }
        too_many_groups.append('1');
    }
    too_many_groups.append(QByteArrayLiteral(";2026hZ"));
    ok &= check(malformed_group_count_backend->emit_output(std::move(too_many_groups)),
        "overlong parameter group list stays on parser recovery path");
    const std::optional<term::Terminal_render_snapshot> malformed_group_count_snapshot =
        malformed_group_count_session->latest_render_snapshot();
    ok &= check(malformed_group_count_snapshot.has_value() &&
        malformed_group_count_snapshot->viewport.active_buffer ==
            term::Terminal_buffer_id::PRIMARY &&
        !malformed_group_count_snapshot->modes.synchronized_output &&
        snapshot_row_text(*malformed_group_count_snapshot, 0) == QStringLiteral("Z"),
        "overlong parameter group list is not rewritten into synchronized-output modes");

    std::unique_ptr<term::Terminal_session> long_parameter_session;
    Scripted_backend* long_parameter_backend = make_session(long_parameter_session);
    ok &= check(long_parameter_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "long-parameter synchronized-output scanner session starts");
    ok &= check(long_parameter_backend->emit_output(
        QByteArrayLiteral("\x1b[?999999999999999999999hZ")),
        "long CSI parameter output is accepted by callback path");
    const std::optional<term::Terminal_render_snapshot> long_parameter_snapshot =
        long_parameter_session->latest_render_snapshot();
    ok &= check(long_parameter_snapshot.has_value() &&
        snapshot_row_text(*long_parameter_snapshot, 0) == QStringLiteral("Z"),
        "long CSI parameter scan does not overflow before parser recovery");

    std::unique_ptr<term::Terminal_session> osc_utf8_c1_session;
    Scripted_backend* osc_utf8_c1_backend = make_session(osc_utf8_c1_session);
    ok &= check(osc_utf8_c1_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "OSC UTF-8 C1-continuation scanner session starts");
    QByteArray osc_utf8_c1_payload = QByteArrayLiteral("\x1b]2;");
    osc_utf8_c1_payload.append(decode_hex("e29a9b"));
    osc_utf8_c1_payload.append(QByteArrayLiteral("?25;2026hleaked\a\x1b[?2026lZ"));
    ok &= check(osc_utf8_c1_backend->emit_output(std::move(osc_utf8_c1_payload)),
        "OSC UTF-8 C1-continuation payload is accepted");
    const std::optional<term::Terminal_render_snapshot> osc_utf8_c1_snapshot =
        osc_utf8_c1_session->latest_render_snapshot();
    ok &= check(osc_utf8_c1_snapshot.has_value() &&
        !osc_utf8_c1_snapshot->modes.synchronized_output &&
        snapshot_row_text(*osc_utf8_c1_snapshot, 0) == QStringLiteral("Z"),
        "OSC UTF-8 C1-continuation payload does not leak sync-like title bytes");

    std::unique_ptr<term::Terminal_session> split_osc_utf8_c1_session;
    Scripted_backend* split_osc_utf8_c1_backend =
        make_session(split_osc_utf8_c1_session);
    ok &= check(split_osc_utf8_c1_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "split OSC UTF-8 C1-continuation scanner session starts");
    QByteArray split_osc_utf8_c1_prefix = QByteArrayLiteral("\x1b]2;");
    split_osc_utf8_c1_prefix.append(decode_hex("e29a"));
    ok &= check(split_osc_utf8_c1_backend->emit_output(std::move(split_osc_utf8_c1_prefix)),
        "split OSC UTF-8 C1-continuation prefix is accepted");
    ok &= check(!split_osc_utf8_c1_session->latest_render_snapshot().has_value(),
        "split OSC UTF-8 C1-continuation prefix publishes no snapshot");
    QByteArray split_osc_utf8_c1_suffix = decode_hex("9b");
    split_osc_utf8_c1_suffix.append(QByteArrayLiteral("?25;2026hleaked\a\x1b[?2026lZ"));
    ok &= check(split_osc_utf8_c1_backend->emit_output(std::move(split_osc_utf8_c1_suffix)),
        "split OSC UTF-8 C1-continuation suffix is accepted");
    const std::optional<term::Terminal_render_snapshot> split_osc_utf8_c1_snapshot =
        split_osc_utf8_c1_session->latest_render_snapshot();
    ok &= check(split_osc_utf8_c1_snapshot.has_value() &&
        !split_osc_utf8_c1_snapshot->modes.synchronized_output &&
        snapshot_row_text(*split_osc_utf8_c1_snapshot, 0) == QStringLiteral("Z"),
        "split OSC UTF-8 C1-continuation payload does not leak sync-like title bytes");

    return ok;
}

bool test_viewport_scroll_public_session_path()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "viewport-scroll session starts");
    ok &= check(backend->emit_output(numbered_scroll_lines(80)),
        "viewport-scroll backend creates scrollback");

    const std::optional<term::Terminal_render_snapshot> tail_snapshot =
        session->latest_render_snapshot();
    ok &= check(tail_snapshot.has_value() &&
        tail_snapshot->viewport.scrollback_rows > 0 &&
        tail_snapshot->viewport.offset_from_tail == 0,
        "viewport-scroll session starts at scrollback tail");
    const std::uint64_t tail_generation = session->render_snapshot_generation();

    const term::Terminal_viewport_scroll_result scroll_result =
        session->scroll_viewport_lines(3);
    const std::optional<term::Terminal_render_snapshot> scrolled_snapshot =
        session->latest_render_snapshot();
    ok &= check(scroll_result.action ==
        term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "session viewport scroll reports movement");
    ok &= check(scrolled_snapshot.has_value() &&
        scrolled_snapshot->viewport.offset_from_tail == 3 &&
        session->render_snapshot_generation() == tail_generation + 1U,
        "session viewport scroll publishes one detached snapshot");
    ok &= check(tail_snapshot.has_value() &&
        scrolled_snapshot.has_value() &&
        snapshot_row_text(*scrolled_snapshot, 3) ==
            snapshot_row_text(*tail_snapshot, 0),
        "session viewport scroll shifts previous tail rows by the requested delta");
    ok &= check(scrolled_snapshot.has_value() &&
        term::validate_render_snapshot(*scrolled_snapshot).status ==
            term::Terminal_render_snapshot_status::OK,
        "session viewport scroll snapshot validates");

    const term::Terminal_session_result write_result =
        session->write_user_bytes(QByteArrayLiteral("x"));
    const std::optional<term::Terminal_render_snapshot> returned_snapshot =
        session->latest_render_snapshot();
    ok &= check(write_result.code == term::Terminal_session_result_code::ACCEPTED,
        "user write while detached is accepted");
    ok &= check(returned_snapshot.has_value() &&
        returned_snapshot->viewport.offset_from_tail == 0,
        "accepted user write returns the viewport to tail");
    ok &= check(session->render_snapshot_generation() == tail_generation + 2U,
        "accepted user write publishes one tail-restored snapshot");

    const auto scroll_again = [&](int delta, const char* message) {
        const term::Terminal_viewport_scroll_result result =
            session->scroll_viewport_lines(delta);
        const std::optional<term::Terminal_render_snapshot> snapshot =
            session->latest_render_snapshot();
        bool local_ok = check(result.action ==
            term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
            message);
        local_ok &= check(snapshot.has_value() &&
            snapshot->viewport.offset_from_tail == delta,
            "viewport is detached before sibling input path");
        return local_ok;
    };

    ok &= scroll_again(2, "viewport detaches before key input");
    const std::uint64_t key_generation = session->render_snapshot_generation();
    QKeyEvent key_event(
        QEvent::KeyPress,
        Qt::Key_A,
        Qt::NoModifier,
        QStringLiteral("a"));
    const term::Terminal_key_event_result key_result =
        session->write_key_event(key_event);
    const std::optional<term::Terminal_render_snapshot> key_snapshot =
        session->latest_render_snapshot();
    ok &= check(key_result.handled &&
        key_result.result.code == term::Terminal_session_result_code::ACCEPTED,
        "accepted key input writes through session");
    ok &= check(key_snapshot.has_value() &&
        key_snapshot->viewport.offset_from_tail == 0 &&
        session->render_snapshot_generation() == key_generation + 1U,
        "accepted key input returns the viewport to tail");

    ok &= scroll_again(2, "viewport detaches before IME commit");
    const std::uint64_t ime_generation = session->render_snapshot_generation();
    const term::Terminal_ime_commit_result ime_result =
        session->write_ime_commit(QStringLiteral("i"));
    const std::optional<term::Terminal_render_snapshot> ime_snapshot =
        session->latest_render_snapshot();
    ok &= check(ime_result.handled &&
        ime_result.result.code == term::Terminal_session_result_code::ACCEPTED,
        "accepted IME commit writes through session");
    ok &= check(ime_snapshot.has_value() &&
        ime_snapshot->viewport.offset_from_tail == 0 &&
        session->render_snapshot_generation() == ime_generation + 1U,
        "accepted IME commit returns the viewport to tail");

    ok &= scroll_again(2, "viewport detaches before paste");
    const std::uint64_t paste_generation = session->render_snapshot_generation();
    const term::Terminal_paste_text_result paste_result =
        session->write_paste_text(
            QStringLiteral("p"),
            term::Terminal_paste_framing_policy::DISABLED);
    const std::optional<term::Terminal_render_snapshot> paste_snapshot =
        session->latest_render_snapshot();
    ok &= check(paste_result.handled &&
        paste_result.result.code == term::Terminal_session_result_code::ACCEPTED,
        "accepted paste writes through session");
    ok &= check(paste_snapshot.has_value() &&
        paste_snapshot->viewport.offset_from_tail == 0 &&
        session->render_snapshot_generation() == paste_generation + 1U,
        "accepted paste returns the viewport to tail");

    std::unique_ptr<term::Terminal_session> synchronized_session;
    Scripted_backend* synchronized_backend = make_session(synchronized_session);
    ok &= check(synchronized_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "synchronized viewport-scroll session starts");
    ok &= check(synchronized_backend->emit_output(numbered_scroll_lines(80)),
        "synchronized viewport-scroll backend creates scrollback");
    const std::optional<term::Terminal_render_snapshot> synchronized_tail_snapshot =
        synchronized_session->latest_render_snapshot();
    ok &= check(synchronized_tail_snapshot.has_value() &&
        synchronized_tail_snapshot->viewport.offset_from_tail == 0,
        "synchronized viewport-scroll setup publishes tail viewport");
    const std::uint64_t synchronized_tail_generation =
        synchronized_session->render_snapshot_generation();
    ok &= check(synchronized_backend->emit_output(QByteArrayLiteral("\x1b[?2026hheld")),
        "synchronized viewport-scroll backend enters synchronized output");
    ok &= check(synchronized_session->render_snapshot_generation() ==
        synchronized_tail_generation,
        "synchronized output entry suppresses publication before scroll");

    // This is the no-drain wheel/session path. It must accept viewport movement
    // from the published baseline while synchronized output defers publication.
    const term::Terminal_viewport_scroll_result held_scroll_result =
        synchronized_tail_snapshot.has_value()
            ? synchronized_session->scroll_viewport_lines_from_published_state(
                1,
                synchronized_tail_snapshot->viewport)
            : term::Terminal_viewport_scroll_result{};
    ok &= check(held_scroll_result.action ==
        term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "published-state viewport scroll during synchronized output still updates viewport state");
    ok &= check(synchronized_session->render_snapshot_generation() ==
        synchronized_tail_generation,
        "published-state viewport scroll during synchronized output does not publish hidden state");
    const std::optional<term::Terminal_render_snapshot> hidden_scroll_snapshot =
        synchronized_session->latest_render_snapshot();
    ok &= check(hidden_scroll_snapshot.has_value() &&
        hidden_scroll_snapshot->viewport.offset_from_tail == 0,
        "published-state viewport scroll keeps hidden synchronized output unpublished");

    ok &= check(synchronized_backend->emit_output(QByteArrayLiteral("\x1b[?2026l")),
        "synchronized viewport-scroll backend releases synchronized output");
    const std::optional<term::Terminal_render_snapshot> released_snapshot =
        synchronized_session->latest_render_snapshot();
    ok &= check(released_snapshot.has_value() &&
        released_snapshot->viewport.offset_from_tail == 1 &&
        synchronized_session->render_snapshot_generation() ==
            synchronized_tail_generation + 1U,
        "synchronized output release publishes the deferred scrolled viewport");
    ok &= check(released_snapshot.has_value() &&
        released_snapshot->dirty_row_ranges.size() == 1U &&
        released_snapshot->dirty_row_ranges.front().first_row == 0 &&
        released_snapshot->dirty_row_ranges.front().row_count ==
            released_snapshot->grid_size.rows,
        "synchronized output release preserves deferred viewport damage metadata");

    std::unique_ptr<term::Terminal_session> no_payload_sync_session;
    Scripted_backend* no_payload_sync_backend = make_session(no_payload_sync_session);
    ok &= check(no_payload_sync_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "no-payload synchronized viewport-scroll session starts");
    ok &= check(no_payload_sync_backend->emit_output(numbered_scroll_lines(80)),
        "no-payload synchronized viewport-scroll backend creates scrollback");
    const std::optional<term::Terminal_render_snapshot> no_payload_tail_snapshot =
        no_payload_sync_session->latest_render_snapshot();
    ok &= check(no_payload_tail_snapshot.has_value() &&
        no_payload_tail_snapshot->viewport.offset_from_tail == 0,
        "no-payload synchronized viewport-scroll setup publishes tail viewport");
    const std::uint64_t no_payload_tail_generation =
        no_payload_sync_session->render_snapshot_generation();
    ok &= check(no_payload_sync_backend->emit_output(QByteArrayLiteral("\x1b[?2026h")),
        "no-payload synchronized viewport-scroll backend enters synchronized output");
    ok &= check(no_payload_sync_session->render_snapshot_generation() ==
        no_payload_tail_generation,
        "no-payload synchronized output entry suppresses publication before scroll");

    const term::Terminal_viewport_scroll_result no_payload_scroll_result =
        no_payload_tail_snapshot.has_value()
            ? no_payload_sync_session->scroll_viewport_lines_from_published_state(
                2,
                no_payload_tail_snapshot->viewport)
            : term::Terminal_viewport_scroll_result{};
    ok &= check(no_payload_scroll_result.action ==
        term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "no-payload published-state viewport scroll during synchronized output updates viewport state");
    ok &= check(no_payload_sync_session->render_snapshot_generation() ==
        no_payload_tail_generation,
        "no-payload published-state viewport scroll during synchronized output does not publish");
    const std::optional<term::Terminal_render_snapshot> no_payload_held_snapshot =
        no_payload_sync_session->latest_render_snapshot();
    ok &= check(no_payload_held_snapshot.has_value() &&
        no_payload_held_snapshot->viewport.offset_from_tail == 0,
        "no-payload published-state viewport scroll keeps synchronized output unpublished");

    ok &= check(no_payload_sync_backend->emit_output(QByteArrayLiteral("\x1b[?2026l")),
        "no-payload synchronized viewport-scroll backend releases synchronized output");
    const std::optional<term::Terminal_render_snapshot> no_payload_released_snapshot =
        no_payload_sync_session->latest_render_snapshot();
    ok &= check(no_payload_released_snapshot.has_value() &&
        no_payload_released_snapshot->viewport.offset_from_tail == 2 &&
        no_payload_sync_session->render_snapshot_generation() ==
            no_payload_tail_generation + 1U,
        "no-payload synchronized output release publishes exact deferred viewport offset");
    ok &= check(no_payload_released_snapshot.has_value() &&
        no_payload_released_snapshot->dirty_row_ranges.size() == 1U &&
        no_payload_released_snapshot->dirty_row_ranges.front().first_row == 0 &&
        no_payload_released_snapshot->dirty_row_ranges.front().row_count ==
            no_payload_released_snapshot->grid_size.rows,
        "no-payload synchronized output release preserves deferred viewport dirty metadata");

    term::Terminal_session_config negative_scrollback_config;
    negative_scrollback_config.scrollback_limit = -5;
    std::unique_ptr<term::Terminal_session> negative_limit_session;
    Scripted_backend* negative_limit_backend =
        make_session(negative_limit_session, negative_scrollback_config);
    ok &= check(negative_limit_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "negative scrollback limit is clamped at the session boundary");
    ok &= check(negative_limit_backend->emit_output(numbered_scroll_lines(80)),
        "negative scrollback limit session accepts output");
    const std::optional<term::Terminal_render_snapshot> negative_limit_snapshot =
        negative_limit_session->latest_render_snapshot();
    ok &= check(negative_limit_snapshot.has_value() &&
        negative_limit_snapshot->viewport.scrollback_rows == 0,
        "negative scrollback limit behaves as zero scrollback");

    std::unique_ptr<term::Terminal_session> live_limit_session;
    Scripted_backend* live_limit_backend = make_session(live_limit_session);
    ok &= check(live_limit_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "live scrollback-limit session starts");
    ok &= check(live_limit_backend->emit_output(numbered_scroll_lines(80)),
        "live scrollback-limit session creates scrollback");
    const term::Terminal_viewport_scroll_result live_limit_scroll =
        live_limit_session->scroll_viewport_lines(10);
    ok &= check(live_limit_scroll.action ==
        term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "live scrollback-limit session detaches viewport before shrink");
    const std::uint64_t live_limit_generation =
        live_limit_session->render_snapshot_generation();
    live_limit_session->set_scrollback_limit(2);
    const std::optional<term::Terminal_render_snapshot> live_limit_snapshot =
        live_limit_session->latest_render_snapshot();
    ok &= check(live_limit_snapshot.has_value() &&
        live_limit_snapshot->viewport.scrollback_rows == 2 &&
        live_limit_snapshot->viewport.offset_from_tail <= 2 &&
        live_limit_session->render_snapshot_generation() == live_limit_generation + 1U,
        "live scrollback-limit shrink clamps detached viewport and publishes");

    return ok;
}

bool test_resize_preserves_primary_scrollback()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "resize scrollback session starts");
    ok &= check(backend->emit_output(numbered_scroll_lines(80)),
        "resize scrollback backend creates scrollback");

    const std::optional<term::Terminal_render_snapshot> before_resize =
        session->latest_render_snapshot();
    ok &= check(before_resize.has_value() &&
        before_resize->viewport.active_buffer    == term::Terminal_buffer_id::PRIMARY &&
        before_resize->viewport.scrollback_rows  >  0                                &&
        before_resize->viewport.offset_from_tail == 0,
        "resize scrollback fixture starts at primary tail with scrollback");
    const int previous_scrollback_rows = before_resize.has_value()
        ? before_resize->viewport.scrollback_rows
        : 0;

    const term::Terminal_session_result resize_result =
        session->resize(QSizeF(1000.0, 420.0), {20, 100});
    ok &= check(resize_result.code == term::Terminal_session_result_code::ACCEPTED,
        "resize with scrollback is accepted");
    const std::optional<term::Terminal_render_snapshot> after_resize =
        session->latest_render_snapshot();
    ok &= check(after_resize.has_value() &&
        after_resize->grid_size.rows            == 20                       &&
        after_resize->grid_size.columns         == 100                      &&
        after_resize->viewport.visible_rows     == 20                       &&
        after_resize->viewport.scrollback_rows  == previous_scrollback_rows &&
        after_resize->viewport.offset_from_tail == 0,
        "resize preserves primary scrollback at tail");

    const term::Terminal_viewport_scroll_result scroll_result =
        session->scroll_viewport_lines(3);
    const std::optional<term::Terminal_render_snapshot> scrolled =
        session->latest_render_snapshot();
    ok &= check(scroll_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "resized primary scrollback remains scrollable");
    ok &= check(scrolled.has_value() &&
        scrolled->viewport.scrollback_rows  == previous_scrollback_rows &&
        scrolled->viewport.offset_from_tail == 3,
        "resized primary scrollback publishes detached viewport");

    std::unique_ptr<term::Terminal_session> detached_session;
    Scripted_backend* detached_backend = make_session(detached_session);
    ok &= check(detached_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "detached resize scrollback session starts");
    ok &= check(detached_backend->emit_output(numbered_scroll_lines(80)),
        "detached resize scrollback backend creates scrollback");
    ok &= check(detached_session->scroll_viewport_lines(4).action ==
        term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "detached resize fixture scrolls before resize");
    const std::optional<term::Terminal_render_snapshot> detached_before_resize =
        detached_session->latest_render_snapshot();
    const int detached_scrollback_rows = detached_before_resize.has_value()
        ? detached_before_resize->viewport.scrollback_rows
        : 0;
    ok &= check(detached_session->resize(QSizeF(900.0, 360.0), {18, 90}).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "detached resize with scrollback is accepted");
    const std::optional<term::Terminal_render_snapshot> detached_after_resize =
        detached_session->latest_render_snapshot();
    ok &= check(detached_after_resize.has_value() &&
        detached_after_resize->viewport.scrollback_rows  == detached_scrollback_rows &&
        detached_after_resize->viewport.offset_from_tail == 4,
        "resize preserves detached primary scrollback offset");

    return ok;
}

bool test_resize_repaint_clear_keeps_primary_scrollback_when_recovery_is_enabled()
{
    bool ok = true;

    term::Terminal_session_config config;
    config.recover_scrollback_from_primary_repaints = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session, config);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "resize repaint-clear session starts");
    ok &= check(backend->emit_output(numbered_scroll_lines(80)),
        "resize repaint-clear backend creates scrollback");

    const std::optional<term::Terminal_render_snapshot> before_resize =
        session->latest_render_snapshot();
    const int previous_scrollback_rows = before_resize.has_value()
        ? before_resize->viewport.scrollback_rows
        : 0;
    ok &= check(previous_scrollback_rows > 0,
        "resize repaint-clear fixture starts with scrollback");

    ok &= check(session->resize(QSizeF(1000.0, 420.0), {20, 100}).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "resize repaint-clear resize is accepted");
    ok &= check(backend->emit_output(QByteArrayLiteral(
            "\x1b[?25l\x1b[H\x1b[?25h\x1b[2J\x1b[3Jredraw")),
        "backend emits resize-triggered full repaint clear");

    const std::optional<term::Terminal_render_snapshot> after_resize_repaint =
        session->latest_render_snapshot();
    ok &= check(after_resize_repaint.has_value() &&
        after_resize_repaint->viewport.scrollback_rows == previous_scrollback_rows,
        "resize-triggered repaint clear preserves primary scrollback");
    ok &= check(session->scroll_viewport_lines(3).action ==
        term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "primary scrollback remains scrollable after resize repaint clear");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[H\x1b[2J\x1b[3J")),
        "backend emits a later explicit scrollback clear");
    const std::optional<term::Terminal_render_snapshot> after_later_clear =
        session->latest_render_snapshot();
    ok &= check(after_later_clear.has_value() &&
        after_later_clear->viewport.scrollback_rows == 0,
        "later explicit ED3 still clears primary scrollback");

    std::unique_ptr<term::Terminal_session> echo_session;
    Scripted_backend* echo_backend = make_session(echo_session, config);
    ok &= check(echo_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "resize echo-clear session starts");
    ok &= check(echo_backend->emit_output(numbered_scroll_lines(80)),
        "resize echo-clear backend creates scrollback");
    ok &= check(echo_session->resize(QSizeF(1000.0, 420.0), {20, 100}).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "resize echo-clear resize is accepted");
    ok &= check(echo_backend->emit_output(QByteArrayLiteral("clear\r\n\x1b[H\x1b[2J\x1b[3J")),
        "backend emits echoed clear command after resize");
    const std::optional<term::Terminal_render_snapshot> after_echo_clear =
        echo_session->latest_render_snapshot();
    ok &= check(after_echo_clear.has_value() &&
        after_echo_clear->viewport.scrollback_rows == 0,
        "visible output before ED3 treats clear as explicit after resize");

    return ok;
}

bool test_cursor_home_line_repaint_does_not_synthesize_primary_scrollback()
{
    bool ok = true;

    term::Terminal_session_config config;
    config.recover_scrollback_from_primary_repaints = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session, config);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "line-repaint recovery session starts");
    ok &= check(session->resize(QSizeF(80.0, 24.0), {3, 8}).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "line-repaint recovery grid is compact");
    ok &= check(backend->emit_output(QByteArrayLiteral("aaa\r\nbbb\r\nccc")),
        "line-repaint recovery fixture fills the primary screen");

    const std::optional<term::Terminal_render_snapshot> before_repaint =
        session->latest_render_snapshot();
    ok &= check(before_repaint.has_value() &&
        before_repaint->viewport.scrollback_rows == 0,
        "line-repaint recovery fixture starts without scrollback");

    ok &= check(backend->emit_output(QByteArrayLiteral(
            "\x1b[?2026h"
            "\x1b[?25l"
            "\x1b[H\x1b[K" "bbb"
            "\x1b[2;1H\x1b[K" "ccc"
            "\x1b[3;1H\x1b[K" "ddd"
            "\x1b[?25h"
            "\x1b[?2026l")),
        "backend emits synchronized cursor-home line repaint");

    const std::optional<term::Terminal_render_snapshot> after_repaint =
        session->latest_render_snapshot();
    ok &= check(after_repaint.has_value() &&
        after_repaint->viewport.scrollback_rows == 0,
        "cursor-home EL line repaint does not synthesize primary scrollback");
    ok &= check(after_repaint.has_value() &&
        snapshot_row_text(*after_repaint, 0) == QStringLiteral("bbb") &&
        snapshot_row_text(*after_repaint, 1) == QStringLiteral("ccc") &&
        snapshot_row_text(*after_repaint, 2) == QStringLiteral("ddd"),
        "cursor-home EL line repaint updates visible rows");
    ok &= check(session->scroll_viewport_lines(1).action ==
        term::Terminal_viewport_scroll_action::AT_BOUNDARY,
        "cursor-home EL line repaint leaves primary viewport at scrollback boundary");
    return ok;
}

bool test_cursor_home_text_then_el_repaint_synthesizes_primary_scrollback()
{
    bool ok = true;

    term::Terminal_session_config config;
    config.recover_scrollback_from_primary_repaints = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session, config);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "text-then-EL recovery session starts");
    ok &= check(session->resize(QSizeF(80.0, 24.0), {3, 8}).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "text-then-EL recovery grid is compact");
    ok &= check(backend->emit_output(QByteArrayLiteral("aaa\r\nbbb\r\nccc")),
        "text-then-EL recovery fixture fills the primary screen");

    const std::optional<term::Terminal_render_snapshot> before_repaint =
        session->latest_render_snapshot();
    ok &= check(before_repaint.has_value() &&
        before_repaint->viewport.scrollback_rows == 0,
        "text-then-EL recovery fixture starts without scrollback");

    ok &= check(backend->emit_output(QByteArrayLiteral(
            "\x1b[?2026h"
            "\x1b[?25l"
            "\x1b[H" "bbb\x1b[K\r\n"
            "ccc\x1b[K\r\n"
            "ddd\x1b[K"
            "\x1b[?25h"
            "\x1b[?2026l")),
        "backend emits synchronized cursor-home text-then-EL repaint");

    const std::optional<term::Terminal_render_snapshot> after_repaint =
        session->latest_render_snapshot();
    ok &= check(after_repaint.has_value() &&
        after_repaint->viewport.scrollback_rows  == 1 &&
        after_repaint->viewport.offset_from_tail == 0,
        "cursor-home text-then-EL repaint synthesizes primary scrollback");
    ok &= check(after_repaint.has_value() &&
        snapshot_row_text(*after_repaint, 0) == QStringLiteral("bbb") &&
        snapshot_row_text(*after_repaint, 1) == QStringLiteral("ccc") &&
        snapshot_row_text(*after_repaint, 2) == QStringLiteral("ddd"),
        "cursor-home text-then-EL repaint updates visible rows");

    const term::Terminal_viewport_scroll_result scroll_result =
        session->scroll_viewport_lines(1);
    const std::optional<term::Terminal_render_snapshot> scrolled_snapshot =
        session->latest_render_snapshot();
    ok &= check(scroll_result.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "cursor-home text-then-EL recovered scrollback is scrollable");
    ok &= check(scrolled_snapshot.has_value() &&
        snapshot_row_text(*scrolled_snapshot, 0) == QStringLiteral("aaa"),
        "cursor-home text-then-EL repaint keeps displaced row in scrollback");

    return ok;
}

bool test_parser_notifications_reach_session_notifications()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "parser-notification session starts");

    ok &= check(backend->emit_output(QByteArrayLiteral("\a\x1b]2;demo\x1b\\")),
        "fake backend emits bell and title notification sequences");

    const std::optional<term::Terminal_session_notification> bell = first_notification(
        *session,
        term::Terminal_session_notification_kind::BELL_REQUESTED);
    ok &= check(bell.has_value(),
        "parser bell notification reaches session notifications");
    const std::optional<term::Terminal_render_snapshot> bell_snapshot =
        session->latest_render_snapshot();
    ok &= check(bell_snapshot.has_value() && bell_snapshot->metadata.visual_bell_active,
        "default bell policy marks the render snapshot for visual bell");

    const std::optional<term::Terminal_session_notification> title = first_notification(
        *session,
        term::Terminal_session_notification_kind::TITLE_CHANGED);
    ok &= check(title.has_value() && title->message == QStringLiteral("demo"),
        "parser title notification reaches session notifications");

    term::Terminal_session_config config;
    config.write_queue_limits.high_water_bytes = 1U;
    config.write_queue_limits.hard_limit_bytes = 2U;

    std::unique_ptr<term::Terminal_session> ordered_session;
    Scripted_backend* ordered_backend = make_session(ordered_session, config);
    ok &= check(ordered_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "ordered parser-action session starts");
    ok &= check(ordered_backend->emit_output(QByteArrayLiteral("\x1b[6n\x1b]2;after\x1b\\")),
        "fake backend emits query before title notification");
    const std::vector<term::Terminal_session_notification> ordered_notifications =
        ordered_session->notifications();
    std::size_t backend_error_index = ordered_notifications.size();
    std::size_t title_index         = ordered_notifications.size();
    for (std::size_t i = 0U; i < ordered_notifications.size(); ++i) {
        if (ordered_notifications[i].kind ==
            term::Terminal_session_notification_kind::BACKEND_ERROR)
        {
            backend_error_index = std::min(backend_error_index, i);
        }

        if (ordered_notifications[i].kind ==
            term::Terminal_session_notification_kind::TITLE_CHANGED)
        {
            title_index = std::min(title_index, i);
        }
    }
    ok &= check(backend_error_index < title_index,
        "parser terminal reply handling preserves action order before later title notification");
    ok &= check(backend_error_index < ordered_notifications.size() &&
        title_index < ordered_notifications.size(),
        "parser terminal reply ordering test observes both compared notifications");

    return ok;
}

bool test_bell_policy_coalesces_with_deterministic_clock()
{
    bool ok = true;

    std::uint64_t bell_time_ms = 10U;
    term::Terminal_session_config config;
    config.bell_policy.audible_enabled       = true;
    config.bell_policy.visual_enabled        = true;
    config.bell_policy.coalescing_window_ms  = 100U;
    config.bell_policy.max_events_per_window = 1U;
    config.bell_clock_ms                     = [&bell_time_ms]() { return bell_time_ms; };

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session, config);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "deterministic bell-policy session starts");

    ok &= check(backend->emit_output(QByteArrayLiteral("\a\a")),
        "deterministic bell-policy backend emits repeated bells");
    ok &= check(notification_count(
        *session,
        term::Terminal_session_notification_kind::BELL_REQUESTED) == 1U,
        "deterministic bell policy coalesces repeated bells in one window");
    std::optional<term::Terminal_render_snapshot> snapshot =
        session->latest_render_snapshot();
    ok &= check(snapshot.has_value() && snapshot->metadata.visual_bell_active,
        "allowed visual bell marks snapshot visual-bell flag");
    const std::uint64_t generation_after_first_bell =
        session->render_snapshot_generation();

    bell_time_ms  = 109U;
    ok           &= check(backend->emit_output(QByteArrayLiteral("\a")),
        "deterministic bell-policy backend emits bell before boundary");
    ok           &= check(notification_count(
        *session,
        term::Terminal_session_notification_kind::BELL_REQUESTED) == 1U &&
        session->render_snapshot_generation() == generation_after_first_bell,
        "bell before coalescing boundary emits no notification and no visual-bell snapshot");

    bell_time_ms  = 110U;
    ok           &= check(backend->emit_output(QByteArrayLiteral("\a")),
        "deterministic bell-policy backend emits bell at boundary");
    snapshot      = session->latest_render_snapshot();
    ok           &= check(notification_count(
        *session,
        term::Terminal_session_notification_kind::BELL_REQUESTED) == 2U,
        "bell policy allows another bell at coalescing boundary");
    ok           &= check(session->render_snapshot_generation() == generation_after_first_bell + 1U &&
        snapshot.has_value() &&
        snapshot->metadata.visual_bell_active,
        "second allowed visual bell advances generation and marks visual bell active");

    term::Terminal_session_config audible_only_config;
    audible_only_config.bell_policy.audible_enabled = true;
    audible_only_config.bell_policy.visual_enabled  = false;
    audible_only_config.bell_clock_ms               = []() { return 10U; };

    std::unique_ptr<term::Terminal_session> audible_only_session;
    Scripted_backend* audible_only_backend =
        make_session(audible_only_session, audible_only_config);
    ok &= check(audible_only_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "audible-only bell-policy session starts");
    ok &= check(audible_only_backend->emit_output(QByteArrayLiteral("\a")),
        "audible-only bell-policy backend emits bell");
    ok &= check(notification_count(
        *audible_only_session,
        term::Terminal_session_notification_kind::BELL_REQUESTED) == 1U,
        "audible-only bell remains observable as a bell notification");
    ok &= check(!audible_only_session->latest_render_snapshot().has_value(),
        "audible-only bell does not set the visual-bell snapshot flag");

    term::Terminal_session_config visual_only_config;
    visual_only_config.bell_policy.audible_enabled = false;
    visual_only_config.bell_policy.visual_enabled  = true;
    visual_only_config.bell_clock_ms               = []() { return 10U; };

    std::unique_ptr<term::Terminal_session> visual_only_session;
    Scripted_backend* visual_only_backend =
        make_session(visual_only_session, visual_only_config);
    ok       &= check(visual_only_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "visual-only bell-policy session starts");
    ok       &= check(visual_only_backend->emit_output(QByteArrayLiteral("\a")),
        "visual-only bell-policy backend emits bell");
    snapshot  = visual_only_session->latest_render_snapshot();
    ok       &= check(notification_count(
        *visual_only_session,
        term::Terminal_session_notification_kind::BELL_REQUESTED) == 1U &&
        snapshot.has_value() &&
        snapshot->metadata.visual_bell_active,
        "visual-only bell remains observable and marks visual-bell snapshot flag");

    term::Terminal_session_config disabled_config;
    disabled_config.bell_policy.audible_enabled = false;
    disabled_config.bell_policy.visual_enabled  = false;
    disabled_config.bell_clock_ms               = []() { return 10U; };

    std::unique_ptr<term::Terminal_session> disabled_session;
    Scripted_backend* disabled_backend = make_session(disabled_session, disabled_config);
    ok &= check(disabled_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "disabled bell-policy session starts");
    ok &= check(disabled_backend->emit_output(QByteArrayLiteral("\a")),
        "disabled bell-policy backend emits bell");
    ok &= check(notification_count(
        *disabled_session,
        term::Terminal_session_notification_kind::BELL_REQUESTED) == 0U &&
        !disabled_session->latest_render_snapshot().has_value(),
        "disabled bell policy suppresses bell notification and visual snapshot");

    return ok;
}

bool test_parser_state_crosses_backend_output_chunks()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "split-parser session starts");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[31")),
        "fake backend emits first half of SGR sequence");
    ok &= check(backend->emit_output(QByteArrayLiteral("mred")),
        "fake backend emits second half of SGR sequence");

    const std::optional<term::Terminal_render_snapshot> snapshot =
        session->latest_render_snapshot();
    ok &= check(snapshot.has_value(), "split parser output publishes snapshot");
    if (snapshot.has_value()) {
        ok &= check(snapshot_row_text(*snapshot, 0) == QStringLiteral("red"),
            "parser state crosses backend output chunk boundary");
    }

    const std::optional<term::Terminal_screen_model_result> ingest_result =
        session->last_model_ingest_result();
    ok &= check(ingest_result.has_value() &&
        action_kind_count(*ingest_result, term::Parser_action_kind::STYLE_MUTATION) == 1 &&
        action_kind_count(*ingest_result, term::Parser_action_kind::SCREEN_MUTATION) == 1,
        "split parser ingest result contains completed SGR and text actions");

    return ok;
}

bool test_backend_output_replies_use_write_path()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "reply-path session starts");

    ok &= check(backend->emit_output(
        QByteArrayLiteral("A\x1b[6n\x1b[c\x1b[>c\x1b[?25$p\x1b[18t")),
        "fake backend emits output with terminal queries");

    ok &= check(backend->writes.size() == 5U,
        "generated terminal replies reach backend write path");
    if (backend->writes.size() == 5U) {
        ok &= check(backend->writes[0] == QByteArrayLiteral("\x1b[1;2R"),
            "DSR reply reports cursor after preceding output");
        ok &= check(backend->writes[1] == QByteArrayLiteral("\x1b[?1;2c"),
            "DA1 reply is emitted through backend write");
        ok &= check(backend->writes[2] == QByteArrayLiteral("\x1b[>0;0;0c"),
            "DA2 reply is emitted through backend write");
        ok &= check(backend->writes[3] == QByteArrayLiteral("\x1b[?25;1$y"),
            "DECRQM reply is emitted through backend write");
        ok &= check(backend->writes[4] == QByteArrayLiteral("\x1b[8;24;80t"),
            "text-area size reply is emitted through backend write");
    }

    const std::vector<term::Terminal_session_command> commands =
        session->processed_commands();
    std::size_t output_index = commands.size();
    for (std::size_t i = 0U; i < commands.size(); ++i) {
        if (commands[i].kind == term::Terminal_session_command_kind::BACKEND_OUTPUT) {
            output_index = i;
            break;
        }
    }
    ok &= check(output_index + 5U < commands.size(),
        "reply command stream contains backend output followed by replies");
    if (output_index + 5U < commands.size()) {
        ok &= check(commands[output_index + 1U].kind ==
            term::Terminal_session_command_kind::TERMINAL_REPLY &&
            commands[output_index + 2U].kind ==
                term::Terminal_session_command_kind::TERMINAL_REPLY &&
            commands[output_index + 3U].kind ==
                term::Terminal_session_command_kind::TERMINAL_REPLY &&
            commands[output_index + 4U].kind ==
                term::Terminal_session_command_kind::TERMINAL_REPLY &&
            commands[output_index + 5U].kind ==
                term::Terminal_session_command_kind::TERMINAL_REPLY,
            "generated replies are processed after backend output");
    }

    const std::optional<term::Terminal_render_snapshot> snapshot =
        session->latest_render_snapshot();
    ok &= check(snapshot.has_value() &&
        output_index < commands.size() &&
        snapshot->metadata.sequence == commands[output_index].sequence,
        "snapshot metadata keeps backend-output sequence when replies are generated");

    return ok;
}

bool test_mixed_output_query_ordering()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "mixed output/query session starts");

    ok &= check(backend->emit_output(QByteArrayLiteral("ab\x1b[6ncd")),
        "fake backend emits mixed output and cursor query");

    ok &= check(backend->writes.size() == 1U &&
        backend->writes.front() == QByteArrayLiteral("\x1b[1;3R"),
        "DSR reply preserves cursor position at query point");

    const std::optional<term::Terminal_render_snapshot> snapshot =
        session->latest_render_snapshot();
    ok &= check(snapshot.has_value(),
        "mixed output/query publishes final screen snapshot");
    if (snapshot.has_value()) {
        ok &= check(snapshot_row_text(*snapshot, 0) == QStringLiteral("abcd"),
            "mixed output/query leaves final screen text intact");
        ok &= check(snapshot->cursor.position.row == 0 &&
            snapshot->cursor.position.column == 4,
            "mixed output/query leaves cursor after final text");
    }

    return ok;
}

bool test_terminal_canvas_fixture_script_through_session()
{
    bool ok = true;

    term::Terminal_session_config config;
    config.output_queue_limits.high_water_bytes    = 1024U;
    config.output_queue_limits.hard_limit_bytes    = 256U * 1024U;
    config.output_queue_limits.high_water_commands = 64U;
    config.output_queue_limits.hard_limit_commands = 1024U;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session, config);
    term::Terminal_launch_config launch_config = valid_launch_config();
    launch_config.identity.term                = QStringLiteral("xterm-256color");
    launch_config.identity.colorterm           = QStringLiteral("truecolor");
    const std::vector<term::terminal_canvas_fixture_record_t>& fixture_records =
        term::terminal_canvas_fixture_contract_script();

    int expected_non_reply_user_write_count = 0;
    for (const term::terminal_canvas_fixture_record_t& record : fixture_records) {
        if (record.kind  == term::Terminal_canvas_fixture_record_kind::EXPECT_INPUT &&
            record.label != std::string_view("reply-handling"))
        {
            ++expected_non_reply_user_write_count;
        }
    }

    ok &= check(fixture_acknowledgement_order_is_valid(),
        "fixture acknowledgement records follow their input or resize records");
    ok &= check(session->start(launch_config).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "fixture session starts");
    ok &= check(session->process_state() == term::Terminal_process_state::RUNNING &&
        session->backend_ready(),
        "fixture session is running before replay");
    ok &= check(session->grid_size().rows == 24 && session->grid_size().columns == 80,
        "fixture session starts with contract grid size");

    std::vector<std::string_view> seen_labels;
    QByteArray generated_reply_payload;
    bool waiting_for_reply_expect     = false;
    bool prompt_was_visible           = false;
    bool leave_alternate_was_seen     = false;
    bool prompt_editing_ack_was_seen  = false;
    bool resize_report_was_seen       = false;
    bool bracketed_paste_ack_was_seen = false;
    bool focus_reporting_ack_was_seen = false;
    bool mouse_sgr_ack_was_seen       = false;
    int  injected_user_write_count    = 0;
    std::vector<term::Terminal_session_command_kind> expected_command_kinds = {
        term::Terminal_session_command_kind::START,
    };

    for (const term::terminal_canvas_fixture_record_t& record : fixture_records) {
        seen_labels.push_back(record.label);

        switch (record.kind) {
            case term::Terminal_canvas_fixture_record_kind::CHECKPOINT:
                if (record.label == std::string_view("startup")) {
                    ok &= check(session->process_state() == term::Terminal_process_state::RUNNING,
                        "startup checkpoint observes running session");
                }
                else {
                    ok &= check(false, "fixture checkpoint label is known");
                }
                break;

            case term::Terminal_canvas_fixture_record_kind::OUTPUT:
        {
                                const QByteArray payload = decode_hex(record.payload_hex);
                                ok &= check(!payload.isEmpty(), "fixture output payload decodes");

                                const std::size_t write_count_before = backend->writes.size();
                                const bool        output_emitted     = backend->emit_output(payload);
                                ok &= check(output_emitted,
                                    "fixture output record is accepted by scripted backend");
                                if (output_emitted) {
                                    expected_command_kinds.push_back(
                                        term::Terminal_session_command_kind::BACKEND_OUTPUT);
                                }

                                const std::optional<term::Terminal_render_snapshot> snapshot =
                                    session->latest_render_snapshot();
                                if (record.label == std::string_view("enter-alternate-screen")) {
                                    ok &= check(snapshot.has_value() &&
                                        snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE,
                                        "fixture enter-alternate-screen switches session snapshot to alternate");
                                }
                                else
                                if (record.label == std::string_view("prompt")) {
                                    prompt_was_visible = snapshot.has_value() &&
                                        snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE &&
                                        snapshot_contains_text(*snapshot, QStringLiteral("term>"));
                                    ok &= check(prompt_was_visible,
                                        "fixture prompt is visible in alternate screen snapshot");
                                }
                                else
                                if (record.label == std::string_view("prompt-editing-keys-ack")) {
                                    prompt_editing_ack_was_seen = snapshot.has_value() &&
                                        snapshot_contains_text(
                                            *snapshot, QStringLiteral("input prompt-editing-keys ok"));
                                    ok &= check(prompt_editing_ack_was_seen,
                                        "fixture prompt-editing ack appears in session snapshot");
                                }
                                else
                                if (record.label == std::string_view("resize-report")) {
                                    resize_report_was_seen = snapshot.has_value() &&
                                        snapshot_contains_text(*snapshot, QStringLiteral("resize 33x120"));
                                    ok &= check(resize_report_was_seen,
                                        "fixture resize report appears in session snapshot");
                                }
                                else
                                if (record.label == term::k_terminal_canvas_fixture_enable_input_modes_label) {
                                    ok &= check(snapshot.has_value() &&
                                        snapshot->modes.bracketed_paste &&
                                        snapshot->modes.focus_reporting &&
                                        snapshot->modes.sgr_mouse_encoding &&
                                        snapshot->modes.mouse_tracking ==
                                            term::Terminal_mouse_tracking_mode::DRAG,
                                        "fixture enable-input-modes updates session mode snapshot");
                                }
                                else
                                if (record.label == std::string_view("bracketed-paste-ack")) {
                                    bracketed_paste_ack_was_seen = snapshot.has_value() &&
                                        snapshot_contains_text(
                                            *snapshot, QStringLiteral("input bracketed-paste ok"));
                                    ok &= check(bracketed_paste_ack_was_seen,
                                        "fixture bracketed-paste ack appears in session snapshot");
                                }
                                else
                                if (record.label == std::string_view("focus-reporting-ack")) {
                                    focus_reporting_ack_was_seen = snapshot.has_value() &&
                                        snapshot_contains_text(
                                            *snapshot, QStringLiteral("input focus-reporting ok"));
                                    ok &= check(focus_reporting_ack_was_seen,
                                        "fixture focus-reporting ack appears in session snapshot");
                                }
                                else
                                if (record.label == std::string_view("mouse-sgr-1006-ack")) {
                                    mouse_sgr_ack_was_seen = snapshot.has_value() &&
                                        snapshot_contains_text(
                                            *snapshot, QStringLiteral("input mouse-sgr-1006 ok"));
                                    ok &= check(mouse_sgr_ack_was_seen,
                                        "fixture mouse SGR ack appears in session snapshot");
                                }
                                else
                                if (record.label == std::string_view("reply-handling")) {
                                    const std::size_t generated_reply_write_count =
                                        backend->writes.size() - write_count_before;
                                    generated_reply_payload = concatenate_writes(backend->writes, write_count_before);
                                    waiting_for_reply_expect = true;
                                    ok &= check(generated_reply_write_count == 3U,
                                        "fixture reply-handling output generates exactly three terminal replies");
                                    ok &= check(generated_reply_write_count == 3U && !generated_reply_payload.isEmpty(),
                                        "fixture reply-handling output captures generated terminal reply payload");
                                    expected_command_kinds.push_back(
                                        term::Terminal_session_command_kind::TERMINAL_REPLY);
                                    expected_command_kinds.push_back(
                                        term::Terminal_session_command_kind::TERMINAL_REPLY);
                                    expected_command_kinds.push_back(
                                        term::Terminal_session_command_kind::TERMINAL_REPLY);
                                }
                                else
                                if (record.label == std::string_view("leave-alternate-screen")) {
                                    leave_alternate_was_seen = snapshot.has_value() &&
                                        snapshot->viewport.active_buffer == term::Terminal_buffer_id::PRIMARY;
                                    ok &= check(leave_alternate_was_seen,
                                        "fixture leave-alternate-screen returns session snapshot to primary");
                                }

                                break;
        }

            case term::Terminal_canvas_fixture_record_kind::EXPECT_INPUT:
        {
                                const QByteArray payload = decode_hex(record.payload_hex);
                                ok &= check(!payload.isEmpty(), "fixture expected input payload decodes");

                                if (record.label == std::string_view("reply-handling")) {
                                    ok                       &= check(waiting_for_reply_expect,
                                        "fixture reply-handling expected input follows generated replies");
                                    ok                       &= check(generated_reply_payload == payload,
                                        "fixture reply-handling generated replies match expected input payload");
                                    waiting_for_reply_expect  = false;
                                    break;
                                }

                                const std::size_t write_count_before = backend->writes.size();
                                if (record.label == std::string_view("bracketed-paste")) {
                                    const term::Terminal_paste_text_result paste_result =
                                        session->write_paste_text(
                                            QStringLiteral("line1\nline2"),
                                            term::Terminal_paste_framing_policy::APPLICATION_CONTROLLED);
                                    ok &= check(paste_result.handled &&
                                        paste_result.result.code == term::Terminal_session_result_code::ACCEPTED,
                                        "fixture bracketed-paste input is injected through paste path");
                                    const QByteArray paste_payload = concatenate_writes(
                                        backend->writes,
                                        write_count_before);
                                    ok &= check(paste_payload == payload,
                                        "fixture bracketed-paste paste path reaches backend with expected frame");
                                    if (paste_result.handled &&
                                        paste_result.result.code == term::Terminal_session_result_code::ACCEPTED &&
                                        paste_payload            == payload)
                                    {
                                        ++injected_user_write_count;
                                        expected_command_kinds.push_back(
                                            term::Terminal_session_command_kind::USER_PASTE);
                                    }
                                    break;
                                }

                                const term::Terminal_session_result write_result =
                                    session->write_user_bytes(payload);
                                ok &= check(write_result.code == term::Terminal_session_result_code::ACCEPTED,
                                    "fixture expected input is injected as user bytes");
                                ok &= check(backend->writes.size() == write_count_before + 1U &&
                                    backend->writes.back() == payload,
                                    "fixture expected input reaches backend write path in command order");
                                if (write_result.code      == term::Terminal_session_result_code::ACCEPTED &&
                                    backend->writes.size() == write_count_before + 1U                      &&
                                    backend->writes.back() == payload)
                                {
                                    ++injected_user_write_count;
                                    expected_command_kinds.push_back(term::Terminal_session_command_kind::USER_WRITE);
                                }
                                break;
        }

            case term::Terminal_canvas_fixture_record_kind::RESIZE:
        {
                                const std::size_t resize_count_before = backend->resize_requests.size();
                                const std::size_t resize_transaction_count_before =
                                    session->resize_transactions().size();
                                const term::Terminal_session_result resize_result =
                                    session->resize(
                                        QSizeF(
                                            static_cast<qreal>(record.columns) * 10.0,
                                            static_cast<qreal>(record.rows) * 20.0),
                                        {record.rows, record.columns});
                                ok &= check(resize_result.code == term::Terminal_session_result_code::ACCEPTED,
                                    "fixture resize is accepted by session");
                                ok &= check(backend->resize_requests.size() == resize_count_before + 1U &&
                                    backend->resize_requests.back().grid_size.rows == record.rows &&
                                    backend->resize_requests.back().grid_size.columns == record.columns,
                                    "fixture resize reaches backend request path");

                                const std::vector<term::Terminal_resize_transaction> resize_transactions =
                                    session->resize_transactions();
                                ok &= check(resize_transactions.size() == resize_transaction_count_before + 1U &&
                                    resize_transactions.back().target_grid_size.rows == record.rows &&
                                    resize_transactions.back().target_grid_size.columns == record.columns &&
                                    resize_transactions.back().active_buffer ==
                                        term::Terminal_buffer_id::ALTERNATE &&
                                    resize_transactions.back().backend_result ==
                                        term::Terminal_backend_resize_result::APPLIED &&
                                    resize_transactions.back().backend_geometry_in_sync,
                                    "fixture resize records alternate-screen transaction");

                                const std::optional<term::Terminal_render_snapshot> snapshot =
                                    session->latest_render_snapshot();
                                ok &= check(snapshot.has_value() &&
                                    snapshot->grid_size.rows == record.rows &&
                                    snapshot->grid_size.columns == record.columns &&
                                    snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE &&
                                    snapshot->metadata.backend_geometry_in_sync &&
                                    snapshot_contains_text(*snapshot, QStringLiteral("term>")),
                                    "fixture resize publishes alternate-screen grid snapshot");
                                expected_command_kinds.push_back(term::Terminal_session_command_kind::RESIZE);
                                break;
        }

            case term::Terminal_canvas_fixture_record_kind::REPEAT_OUTPUT:
        {
                                constexpr int k_high_volume_chunk_repeats = 1024;

                                const QByteArray payload = decode_hex(record.payload_hex);
                                ok &= check(!payload.isEmpty(), "fixture repeat-output payload decodes");

                                const std::size_t output_chunk_count_before = session->output_chunks().size();
                                const std::size_t backpressure_count_before = notification_count(
                                    *session,
                                    term::Terminal_session_notification_kind::OUTPUT_BACKPRESSURE_CHANGED);
                                const std::size_t backend_error_count_before = notification_count(
                                    *session,
                                    term::Terminal_session_notification_kind::BACKEND_ERROR);
                                const std::size_t pause_request_count_before =
                                    backend->output_pause_requests.size();

                                int remaining_repeats = record.repeat_count;
                                std::vector<QByteArray> emitted_output_chunks;
                                while (remaining_repeats > 0) {
                                    const int chunk_repeats =
                                        std::min(remaining_repeats, k_high_volume_chunk_repeats);
                                    const QByteArray output_chunk   = repeat_bytes(payload, chunk_repeats);
                                    const bool       output_emitted = backend->emit_output(output_chunk);
                                    ok &= check(output_emitted,
                                        "fixture high-volume output chunk is accepted by scripted backend");
                                    if (output_emitted) {
                                        emitted_output_chunks.push_back(output_chunk);
                                        expected_command_kinds.push_back(
                                            term::Terminal_session_command_kind::BACKEND_OUTPUT);
                                    }
                                    remaining_repeats -= chunk_repeats;
                                }

                                const std::optional<term::Terminal_render_snapshot> snapshot =
                                    session->latest_render_snapshot();
                                ok &= check(snapshot.has_value() &&
                                    snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE &&
                                    snapshot_contains_text(*snapshot, QStringLiteral("stream-row")) &&
                                    snapshot->metadata.sequence == session->last_processed_sequence(),
                                    "fixture high-volume output reaches latest alternate-screen snapshot");
                                if (snapshot.has_value()) {
                                    ok &= check(term::validate_render_snapshot(*snapshot).status ==
                                        term::Terminal_render_snapshot_status::OK,
                                        "fixture high-volume snapshot validates");
                                }

                                const std::vector<term::Terminal_session_command> commands =
                                    session->processed_commands();
                                ok &= check(!commands.empty() &&
                                    commands.back().kind ==
                                        term::Terminal_session_command_kind::BACKEND_OUTPUT,
                                    "fixture high-volume output leaves backend output as latest command");
                                const std::vector<QByteArray> output_chunks = session->output_chunks();
                                ok &= check(output_chunks.size() ==
                                    output_chunk_count_before + emitted_output_chunks.size(),
                                    "fixture high-volume output chunks remain explicit in session trace");
                                for (std::size_t i = 0U; i < emitted_output_chunks.size() &&
                                    output_chunk_count_before + i < output_chunks.size(); ++i)
                                {
                                    ok &= check(output_chunks[output_chunk_count_before + i] ==
                                        emitted_output_chunks[i],
                                        "fixture high-volume traced output chunk matches emitted chunk");
                                }
                                ok &= check(notification_count(
                                    *session,
                                    term::Terminal_session_notification_kind::BACKEND_ERROR) ==
                                    backend_error_count_before,
                                    "fixture high-volume output does not overflow or report backend errors");
                                // Scripted backend callbacks are synchronous here: each chunk exceeds
                                // the high-water byte limit, drains before emit_output returns, and
                                // therefore produces one pause/resume notification pair.
                                ok &= check(notification_count(
                                    *session,
                                    term::Terminal_session_notification_kind::OUTPUT_BACKPRESSURE_CHANGED) ==
                                    backpressure_count_before + 2U * emitted_output_chunks.size(),
                                    "fixture high-volume output records exact output backpressure count");
                                ok &= check(backend->output_pause_requests.size() ==
                                    pause_request_count_before + 2U * emitted_output_chunks.size(),
                                    "fixture high-volume output records exact backend pause request count");
                                for (std::size_t i = pause_request_count_before;
                                    i < backend->output_pause_requests.size(); ++i)
                                {
                                    const bool expected_paused = ((i - pause_request_count_before) % 2U) == 0U;
                                    ok &= check(backend->output_pause_requests[i] == expected_paused,
                                        "fixture high-volume backend pause requests alternate true and false");
                                }
                                ok &= check(!backend->output_paused && !session->output_backpressure_active(),
                                    "fixture high-volume output leaves backend output unpaused");
                                ok &= check(backend->terminate_count == 0 &&
                                    session->process_state() == term::Terminal_process_state::RUNNING,
                                    "fixture high-volume output keeps session running without overflow");
                                break;
        }

            case term::Terminal_canvas_fixture_record_kind::EXIT:
                backend->emit_exit({term::Terminal_exit_reason::EXITED, record.exit_code});
                expected_command_kinds.push_back(term::Terminal_session_command_kind::BACKEND_EXIT);
                ok &= check(session->process_state() == term::Terminal_process_state::EXITED,
                    "fixture exit moves session to exited state");
                ok &= check(session->exit_status().has_value() &&
                    session->exit_status()->reason == term::Terminal_exit_reason::EXITED &&
                    session->exit_status()->exit_code == record.exit_code,
                    "fixture exit status is retained by session");
                ok &= check(notification_count(
                    *session,
                    term::Terminal_session_notification_kind::PROCESS_EXITED) == 1U,
                    "fixture exit emits one clean process notification");
                break;
        }
    }

    ok &= check(!waiting_for_reply_expect,
        "fixture replay consumed generated reply expectation");
    ok &= check(prompt_was_visible && leave_alternate_was_seen,
        "fixture replay covered prompt and alternate-screen exit milestones");
    ok &= check(prompt_editing_ack_was_seen &&
        resize_report_was_seen &&
        bracketed_paste_ack_was_seen &&
        focus_reporting_ack_was_seen &&
        mouse_sgr_ack_was_seen,
        "fixture replay observed all acknowledgement outputs");
    ok &= check(injected_user_write_count == expected_non_reply_user_write_count,
        "fixture replay injects non-reply EXPECT_INPUT records as user writes");
    ok &= check(fixture_required_labels_were_seen(seen_labels),
        "fixture replay covers all required labels");
    ok &= check_processed_sequences_are_monotonic(
        *session,
        "fixture replay processed command stream stays monotonic");
    ok &= check(processed_command_kinds_match(
        session->processed_commands(),
        expected_command_kinds,
        "fixture replay processed command kinds match contract order"),
        "fixture replay processed command kinds match contract order");
    ok &= check(notification_count(
        *session,
        term::Terminal_session_notification_kind::BACKEND_ERROR) == 0U,
        "fixture replay completes without backend error notifications");

    return ok;
}

bool test_generated_reply_write_failure_reports_backend_error()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "generated-reply failure session starts");

    backend->fail_write = true;
    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[6n")),
        "fake backend emits query that generates a terminal reply");

    const std::optional<term::Terminal_session_notification> error = first_notification(
        *session,
        term::Terminal_session_notification_kind::BACKEND_ERROR);
    ok &= check(error.has_value() && error->backend_error.has_value() &&
        error->backend_error->code == term::Terminal_backend_error_code::WRITE_FAILED,
        "generated terminal reply write failure reports backend error");
    ok &= check(backend->writes.empty(),
        "failed generated terminal reply write records no accepted backend write");
    ok &= check(!session->latest_render_snapshot().has_value(),
        "query-only backend output does not publish a render snapshot");

    const std::vector<term::Terminal_session_command> commands =
        session->processed_commands();
    ok &= check(commands.size() >= 3U &&
        commands[commands.size() - 2U].kind ==
            term::Terminal_session_command_kind::BACKEND_OUTPUT &&
        commands.back().kind == term::Terminal_session_command_kind::TERMINAL_REPLY,
        "failed generated terminal reply still uses command stream ordering");

    return ok;
}

bool test_generated_reply_byte_enqueue_failure_reports_backend_error()
{
    bool ok = true;

    term::Terminal_session_config config;
    config.write_queue_limits.high_water_bytes = 1U;
    config.write_queue_limits.hard_limit_bytes = 2U;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session, config);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "generated-reply enqueue-failure session starts");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[6n")),
        "fake backend emits query whose reply exceeds write queue");

    const std::optional<term::Terminal_session_notification> error = first_notification(
        *session,
        term::Terminal_session_notification_kind::BACKEND_ERROR);
    ok &= check(error.has_value() && error->backend_error.has_value() &&
        error->backend_error->code == term::Terminal_backend_error_code::WRITE_FAILED,
        "generated terminal reply enqueue failure reports backend error");
    ok &= check(backend->writes.empty(),
        "failed generated terminal reply enqueue does not write inline");

    const std::vector<term::Terminal_session_command> commands =
        session->processed_commands();
    ok &= check(!commands.empty() &&
        commands.back().kind == term::Terminal_session_command_kind::BACKEND_OUTPUT,
        "failed generated reply enqueue leaves no terminal reply command to process");

    return ok;
}

bool test_generated_reply_command_enqueue_failure_reports_backend_error()
{
    bool ok = true;

    term::Terminal_session_config config;
    config.write_queue_limits.high_water_commands = 1U;
    config.write_queue_limits.hard_limit_commands = 1U;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session, config);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "generated-reply command-overflow session starts");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[6n\x1b[c")),
        "fake backend emits multiple generated replies");

    ok &= check(backend->writes.size() == 1U &&
        backend->writes.front() == QByteArrayLiteral("\x1b[1;1R"),
        "first generated reply is processed before command-count overflow");

    const std::optional<term::Terminal_session_notification> error = first_notification(
        *session,
        term::Terminal_session_notification_kind::BACKEND_ERROR);
    ok &= check(error.has_value() && error->backend_error.has_value() &&
        error->backend_error->code == term::Terminal_backend_error_code::WRITE_FAILED,
        "generated reply command-count overflow reports backend error");

    const std::vector<term::Terminal_session_command> commands =
        session->processed_commands();
    ok &= check(commands.size() >= 3U &&
        commands[commands.size() - 2U].kind ==
            term::Terminal_session_command_kind::BACKEND_OUTPUT &&
        commands.back().kind == term::Terminal_session_command_kind::TERMINAL_REPLY,
        "failed later reply does not create an inline write or extra command");

    return ok;
}

bool test_reentrant_start_callbacks_preserve_order()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend     = make_session(session);
    backend->outputs_during_start = {QByteArrayLiteral("ready")};
    backend->errors_during_start.push_back({
        term::Terminal_backend_error_code::WRITE_FAILED,
        QStringLiteral("scripted backend warning"),
    });
    backend->exit_during_start = term::Terminal_backend_exit{
        term::Terminal_exit_reason::EXITED,
        3,
    };

    const term::Terminal_session_result start_result = session->start(valid_launch_config());

    ok &= check(start_result.code == term::Terminal_session_result_code::ACCEPTED,
        "start with reentrant callbacks returns start result");
    ok &= check(session->processed_commands().size() == 4U,
        "reentrant start processes start, output, error, and exit");
    ok &= check(session->processed_commands()[0].kind ==
        term::Terminal_session_command_kind::START,
        "reentrant start command is first");
    ok &= check(session->processed_commands()[1].kind ==
        term::Terminal_session_command_kind::BACKEND_OUTPUT,
        "reentrant output follows start");
    ok &= check(session->processed_commands()[2].kind ==
        term::Terminal_session_command_kind::BACKEND_ERROR,
        "reentrant backend error follows output");
    ok &= check(session->processed_commands()[3].kind ==
        term::Terminal_session_command_kind::BACKEND_EXIT,
        "reentrant backend exit follows backend error");
    ok &= check_processed_sequences_are_monotonic(
        *session,
        "reentrant callback processed command stream stays monotonic");
    ok &= check(session->process_state() == term::Terminal_process_state::EXITED,
        "exit callback during start does not leave session running");
    ok &= check(session->exit_status().has_value() &&
        session->exit_status()->exit_code == 3,
        "reentrant backend exit records exit status");
    ok &= check(notification_count(
        *session,
        term::Terminal_session_notification_kind::BACKEND_ERROR) == 1U,
        "reentrant backend error is reported once");

    return ok;
}

bool test_callback_during_write_is_serialized()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "write-callback session starts");

    backend->outputs_during_write = {QByteArrayLiteral("write-output")};
    const term::Terminal_session_result write_result =
        session->write_user_bytes(QByteArrayLiteral("abc"));

    ok &= check(write_result.code == term::Terminal_session_result_code::ACCEPTED,
        "write with backend callback is accepted");
    ok &= check(session->output_chunks().size() == 1U &&
        session->output_chunks().front() == QByteArrayLiteral("write-output"),
        "write-time backend callback output is delivered");

    const std::vector<term::Terminal_session_command>& commands =
        session->processed_commands();
    ok &= check(commands.size() >= 3U &&
        commands[commands.size() - 2U].kind ==
            term::Terminal_session_command_kind::USER_WRITE &&
        commands[commands.size() - 1U].kind ==
            term::Terminal_session_command_kind::BACKEND_OUTPUT,
        "write-time backend callback is queued after active write command");
    ok &= check_processed_sequences_are_monotonic(
        *session,
        "write-time callback processed command stream stays monotonic");

    return ok;
}

bool test_destructor_ignores_late_backend_callbacks()
{
    bool ok = true;

    auto callback_attempted = std::make_shared<std::atomic_bool>(false);

    {
        std::unique_ptr<term::Terminal_session> session;
        Scripted_backend* backend = make_session(session);
        ok &= check(session->start(valid_launch_config()).code ==
            term::Terminal_session_result_code::ACCEPTED,
            "late-callback session starts");

        backend->emit_output_from_worker_after_delay(
            QByteArrayLiteral("late-output"),
            callback_attempted);
        backend->emit_output_on_destroy = true;
        session.reset();
    }

    ok &= check(callback_attempted->load(),
        "late backend worker attempted callback during session teardown");

    return ok;
}

bool wait_for_worker_callback(const std::shared_ptr<std::atomic_bool>& callback_attempted)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (callback_attempted->load()) {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    while (std::chrono::steady_clock::now() < deadline);

    return false;
}

bool test_worker_thread_callback_is_delivered()
{
    bool ok = true;

    auto callback_attempted = std::make_shared<std::atomic_bool>(false);
    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "worker-callback session starts");

    backend->emit_output_from_worker_after_delay(
        QByteArrayLiteral("worker-output"),
        callback_attempted);

    ok &= check(wait_for_worker_callback(callback_attempted),
        "worker callback attempted output delivery");
    const std::vector<QByteArray> output = session->output_chunks();
    ok &= check(output.size() == 1U && output.front() == QByteArrayLiteral("worker-output"),
        "worker callback output reaches session snapshot");

    return ok;
}

bool test_deferred_callback_ingress_merges_adjacent_output()
{
    bool ok = true;

    term::Terminal_session_config config = tight_session_config();
    config.backend_event_notifier                  = [] {};
    config.output_queue_limits.high_water_bytes    = 64U;
    config.output_queue_limits.hard_limit_bytes    = 64U;
    config.output_queue_limits.high_water_commands = 2U;
    config.output_queue_limits.hard_limit_commands = 2U;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session, config);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "deferred-ingress merge session starts");

    ok &= check(backend->emit_output(QByteArrayLiteral("ab")),
        "deferred-ingress merge emits first output chunk");
    ok &= check(backend->emit_output(QByteArrayLiteral("cd")),
        "deferred-ingress merge emits second output chunk");
    ok &= check(backend->emit_output(QByteArrayLiteral("ef")),
        "deferred-ingress merge emits third output chunk past command cap");
    ok &= check(session->output_chunks().empty(),
        "deferred-ingress merge does not drain inline");

    session->process_backend_callback_events();

    const std::vector<QByteArray> chunks = session->output_chunks();
    ok &= check(chunks.size() == 1U && chunks.front() == QByteArrayLiteral("abcdef"),
        "deferred-ingress merge drains adjacent output as one ordered chunk");
    ok &= check(!has_backend_error_code(
        session->notifications(),
        term::Terminal_backend_error_code::OUTPUT_OVERFLOW),
        "deferred-ingress merge avoids command-count overflow");

    return ok;
}

bool test_budgeted_backend_callback_drain_yields_inside_coalesced_output()
{
    bool ok = true;

    constexpr qsizetype expected_budgeted_slice_bytes = 4096;

    term::Terminal_session_config config;
    config.backend_event_notifier = [] {};

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session, config);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "budgeted backend drain session starts");

    QByteArray output;
    for (int i = 0; i < 900; ++i) {
        output += QByteArrayLiteral("budget-fill\r\n");
    }
    output += QByteArrayLiteral("budget-tail");

    ok &= check(backend->emit_output(output),
        "budgeted backend drain queues large coalesced output");
    backend->emit_exit({term::Terminal_exit_reason::TERMINATED, 0});

    const bool first_drain_complete = session->process_backend_callback_events_for(
        std::chrono::steady_clock::duration::zero());
    const std::vector<QByteArray> first_chunks = session->output_chunks();
    ok &= check(!first_drain_complete,
        "budgeted backend drain yields before draining all queued callback work");
    ok &= check(session->has_pending_backend_callback_events(),
        "budgeted backend drain leaves ordered continuation work pending");
    ok &= check(!session->exit_status().has_value(),
        "budgeted backend drain keeps later callback commands behind output remainder");
    ok &= check(first_chunks.size() == 1U &&
        first_chunks.front().size() == expected_budgeted_slice_bytes,
        "budgeted backend drain processes exactly one bounded output slice first");

    bool complete = first_drain_complete;
    for (int i = 0; !complete && i < 16; ++i) {
        complete = session->process_backend_callback_events_for(
            std::chrono::steady_clock::duration::zero());
    }

    const std::optional<term::Terminal_render_snapshot> snapshot =
        session->latest_render_snapshot();
    ok &= check(complete && !session->has_pending_backend_callback_events(),
        "budgeted backend drain finishes after repeated owner drains");
    ok &= check(session->exit_status().has_value() &&
        session->exit_status()->reason == term::Terminal_exit_reason::TERMINATED,
        "budgeted backend drain preserves output-before-exit callback ordering");
    ok &= check(snapshot.has_value() &&
        snapshot_contains_text(*snapshot, QStringLiteral("budget-tail")),
        "budgeted backend drain eventually publishes the final output tail");
    ok &= check_processed_sequences_are_monotonic(
        *session,
        "budgeted backend drain command trace stays monotonic");

    return ok;
}

bool test_budgeted_backend_callback_drain_holds_output_command_backpressure()
{
    bool ok = true;

    term::Terminal_session_config config;
    config.backend_event_notifier                  = [] {};
    config.output_queue_limits.high_water_bytes    = 1024U * 1024U;
    config.output_queue_limits.hard_limit_bytes    = 1024U * 1024U;
    config.output_queue_limits.high_water_commands = 1U;
    config.output_queue_limits.hard_limit_commands = 2U;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session, config);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "budgeted backpressure session starts");

    QByteArray output;
    for (int i = 0; i < 500; ++i) {
        output += QByteArrayLiteral("backpressure-fill\r\n");
    }

    ok &= check(backend->emit_output(output),
        "budgeted backpressure queues large output command");
    ok &= check(backend->output_paused && session->output_backpressure_active(),
        "budgeted backpressure starts while one output command is queued");

    const bool first_drain_complete = session->process_backend_callback_events_for(
        std::chrono::steady_clock::duration::zero());
    ok &= check(!first_drain_complete,
        "budgeted backpressure first owner drain yields with a sliced remainder");
    ok &= check(backend->output_paused && session->output_backpressure_active(),
        "budgeted backpressure remains active until the sliced command finishes");

    bool complete = first_drain_complete;
    for (int i = 0; !complete && i < 8; ++i) {
        complete = session->process_backend_callback_events_for(
            std::chrono::steady_clock::duration::zero());
    }

    ok &= check(complete && !session->has_pending_backend_callback_events(),
        "budgeted backpressure finishes after repeated owner drains");
    ok &= check(!backend->output_paused && !session->output_backpressure_active(),
        "budgeted backpressure releases only after the final continuation");

    return ok;
}

bool test_deferred_callback_ingress_pauses_backend_at_high_water()
{
    bool ok = true;

    term::Terminal_session_config config = tight_session_config();
    config.backend_event_notifier                  = [] {};
    config.output_queue_limits.high_water_bytes    = 4U;
    config.output_queue_limits.hard_limit_bytes    = 32U;
    config.output_queue_limits.high_water_commands = 64U;
    config.output_queue_limits.hard_limit_commands = 64U;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session, config);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "deferred-ingress high-water pause session starts");

    ok &= check(backend->emit_output(QByteArrayLiteral("12")),
        "deferred-ingress high-water pause emits below threshold");
    ok &= check(!backend->output_paused && backend->output_pause_requests.empty(),
        "deferred-ingress high-water pause does not pause below threshold");
    ok &= check(backend->emit_output(QByteArrayLiteral("34")),
        "deferred-ingress high-water pause emits threshold-crossing chunk");
    ok &= check(backend->output_paused &&
        backend->output_pause_requests.size() == 1U &&
        backend->output_pause_requests.back(),
        "deferred-ingress high-water pause pauses backend from callback thread");
    ok &= check(!backend->emit_output(QByteArrayLiteral("blocked")),
        "deferred-ingress high-water pause blocks further scripted output before drain");

    session->process_backend_callback_events();

    const std::vector<QByteArray> chunks = session->output_chunks();
    ok &= check(chunks.size() == 1U && chunks.front() == QByteArrayLiteral("1234"),
        "deferred-ingress high-water pause drains accepted output in order");
    ok &= check(!backend->output_paused &&
        backend->output_pause_requests.size() == 2U &&
        !backend->output_pause_requests.back(),
        "deferred-ingress high-water pause resumes backend after owner drain");
    ok &= check(!has_backend_error_code(
        session->notifications(),
        term::Terminal_backend_error_code::OUTPUT_OVERFLOW),
        "deferred-ingress high-water pause avoids overflow termination");

    return ok;
}

bool test_deferred_callback_ingress_overflow_is_bounded()
{
    bool ok = true;

    int notifier_count = 0;
    term::Terminal_session_config config = tight_session_config();
    config.output_queue_limits.high_water_bytes    = 64U;
    config.output_queue_limits.high_water_commands = 64U;
    config.backend_event_notifier                  = [&] {
        ++notifier_count;
    };

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend   = make_session(session, config);
    backend->exit_on_terminate  = false;
    ok                         &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "deferred-ingress overflow session starts");

    ok &= check(backend->emit_output(QByteArrayLiteral("1234")),
        "deferred-ingress backend emits first bounded chunk");
    ok &= check(backend->emit_output(QByteArrayLiteral("5678")),
        "deferred-ingress backend emits second bounded chunk");
    ok &= check(backend->emit_output(QByteArrayLiteral("late")),
        "deferred-ingress backend exceeds cumulative pending output limit");
    ok &= check(notifier_count >= 3,
        "deferred-ingress callbacks notify without inline drain");
    ok &= check(!session->latest_render_snapshot().has_value() &&
        !session->last_model_ingest_result().has_value() &&
        session->output_chunks().empty(),
        "deferred-ingress output does not mutate state before GUI drain");

    session->process_backend_callback_events();

    const std::optional<term::Terminal_session_notification> overflow = first_notification(
        *session,
        term::Terminal_session_notification_kind::BACKEND_ERROR);
    ok &= check(overflow.has_value() && overflow->backend_error.has_value() &&
        overflow->backend_error->code == term::Terminal_backend_error_code::OUTPUT_OVERFLOW,
        "deferred-ingress overflow reports typed backend error");
    ok &= check(backend->terminate_count == 1,
        "deferred-ingress overflow requests backend termination");
    ok &= check(!session->backend_ready(),
        "deferred-ingress overflow clears backend readiness");
    ok &= check(!session->latest_render_snapshot().has_value() &&
        !session->last_model_ingest_result().has_value() &&
        session->output_chunks().empty(),
        "deferred-ingress overflow and later output do not mutate model state");

    backend->emit_exit({term::Terminal_exit_reason::TERMINATED, 0});
    session->process_backend_callback_events();

    ok &= check(session->exit_status().has_value() &&
        session->exit_status()->reason == term::Terminal_exit_reason::TERMINATED,
        "deferred-ingress overflow still accepts later backend exit");
    ok &= check(notification_count(
        *session,
        term::Terminal_session_notification_kind::PROCESS_EXITED) == 1U,
        "deferred-ingress overflow publishes later process-exited notification");

    return ok;
}

bool test_deferred_callback_ingress_error_count_is_bounded()
{
    bool ok = true;

    term::Terminal_session_config config = tight_session_config();
    config.backend_event_notifier        = [] {};

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend   = make_session(session, config);
    backend->exit_on_terminate  = false;
    ok                         &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "deferred-ingress error-count session starts");
    const std::size_t notification_count_before = session->notifications().size();

    for (int i = 0; i < 8; ++i) {
        ok &= check(backend->emit_error({
            term::Terminal_backend_error_code::READ_FAILED,
            QStringLiteral("scripted deferred error"),
            }),
            "deferred-ingress backend emits error callback");
    }

    ok &= check(session->notifications().size() == notification_count_before,
        "deferred-ingress error callbacks do not drain inline");

    session->process_backend_callback_events();

    ok &= check(session->notifications().size() <=
        notification_count_before + config.output_queue_limits.hard_limit_commands + 1U,
        "deferred-ingress error callback count remains bounded");
    const std::vector<term::Terminal_session_notification> notifications =
        session->notifications();
    ok &= check(has_backend_error_code(
        notifications,
        term::Terminal_backend_error_code::OUTPUT_OVERFLOW),
        "deferred-ingress error callback overflow leaves typed overflow error");
    ok &= check(backend->terminate_count == 1,
        "deferred-ingress error callback overflow requests termination");

    backend->emit_exit({term::Terminal_exit_reason::TERMINATED, 0});
    session->process_backend_callback_events();

    ok &= check(session->exit_status().has_value() &&
        session->exit_status()->reason == term::Terminal_exit_reason::TERMINATED,
        "deferred-ingress error callback overflow still accepts later backend exit");
    ok &= check(notification_count(
        *session,
        term::Terminal_session_notification_kind::PROCESS_EXITED) == 1U,
        "deferred-ingress error callback overflow publishes later process-exited notification");

    return ok;
}

bool test_pending_notification_limit_preserves_recent_critical_events()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "pending-notification critical-limit session starts");

    term::Terminal_reply empty_reply;
    for (int i = 0; i < 4100; ++i) {
        (void)session->write_terminal_reply(empty_reply);
    }
    backend->emit_exit({term::Terminal_exit_reason::EXITED, 17});

    const std::vector<term::Terminal_session_notification> notifications =
        session->take_pending_notifications();
    bool process_exited_seen = false;
    for (const term::Terminal_session_notification& notification : notifications) {
        if (notification.kind            == term::Terminal_session_notification_kind::PROCESS_EXITED &&
            notification.exit.has_value()                                                            &&
            notification.exit->exit_code == 17)
        {
            process_exited_seen = true;
        }
    }

    ok &= check(notifications.size() <= 4096U,
        "pending notification drain remains bounded after critical burst");
    ok &= check(process_exited_seen,
        "pending notification limit preserves most recent critical process exit");

    return ok;
}

bool test_invalid_launch_config()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    term::Terminal_launch_config launch_config = valid_launch_config();
    launch_config.initial_grid_size = std::nullopt;

    const term::Terminal_session_result start_result = session->start(launch_config);

    ok &= check(start_result.code == term::Terminal_session_result_code::INVALID_ARGUMENT,
        "invalid launch config is rejected by session start");
    ok &= check(start_result.error.has_value() &&
        start_result.error->code ==
            term::Terminal_backend_error_code::INVALID_INITIAL_GRID_SIZE,
        "invalid launch config preserves typed backend error");
    ok &= check(session->process_state() == term::Terminal_process_state::FAILED,
        "invalid launch config moves session to failed state");
    ok &= check(backend->start_configs.empty(),
        "invalid launch config is rejected before backend start");

    const std::optional<term::Terminal_session_notification> error = first_notification(
        *session,
        term::Terminal_session_notification_kind::BACKEND_ERROR);
    ok &= check(error.has_value() && error->backend_error.has_value() &&
        error->backend_error->code ==
            term::Terminal_backend_error_code::INVALID_INITIAL_GRID_SIZE,
        "invalid launch config reports typed session notification error");

    std::unique_ptr<term::Terminal_session> oversized_session;
    Scripted_backend*            oversized_backend       = make_session(oversized_session);
    term::Terminal_launch_config oversized_launch_config = valid_launch_config();
    oversized_launch_config.initial_grid_size =
        term::terminal_grid_size_t{
            term::k_terminal_screen_model_max_rows + 1,
            80,
        };

    const term::Terminal_session_result oversized_start =
        oversized_session->start(oversized_launch_config);

    ok &= check(oversized_start.code == term::Terminal_session_result_code::INVALID_ARGUMENT,
        "oversized initial grid is rejected by session start");
    ok &= check(oversized_start.error.has_value() &&
        oversized_start.error->code ==
            term::Terminal_backend_error_code::INVALID_INITIAL_GRID_SIZE,
        "oversized initial grid preserves typed backend error");
    ok &= check(oversized_backend->start_configs.empty(),
        "oversized initial grid is rejected before backend start");
    ok &= check(!oversized_session->latest_render_snapshot().has_value(),
        "oversized initial grid does not allocate or publish a screen model snapshot");
    ok &= check(oversized_session->grid_size().rows == 0 &&
        oversized_session->grid_size().columns == 0,
        "oversized initial grid leaves session grid unset");

    std::unique_ptr<term::Terminal_session> oversized_cells_session;
    Scripted_backend*            oversized_cells_backend = make_session(oversized_cells_session);
    term::Terminal_launch_config oversized_cells_config  = valid_launch_config();
    oversized_cells_config.initial_grid_size = term::terminal_grid_size_t{2000, 600};

    const term::Terminal_session_result oversized_cells_start =
        oversized_cells_session->start(oversized_cells_config);

    ok &= check(
        oversized_cells_start.code == term::Terminal_session_result_code::INVALID_ARGUMENT,
        "initial grid exceeding cell cap is rejected by session start");
    ok &= check(oversized_cells_backend->start_configs.empty(),
        "initial grid exceeding cell cap is rejected before backend start");

    return ok;
}

bool test_write_and_reply_path()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "write-path session starts");

    const term::Terminal_session_result write_result =
        session->write_user_bytes(QByteArrayLiteral("abc"));
    const term::Terminal_reply reply{
        QByteArrayLiteral("\x1b[24;80R"),
        QStringLiteral("DSR"),
        term::Terminal_reply_kind::DSR_CURSOR_POSITION,
        term::Parser_sequence_family::CSI,
    };
    const term::Terminal_session_result reply_result = session->write_terminal_reply(reply);

    ok &= check(write_result.code == term::Terminal_session_result_code::ACCEPTED,
        "user write accepted");
    ok &= check(reply_result.code == term::Terminal_session_result_code::ACCEPTED,
        "terminal reply accepted");
    ok &= check(backend->writes.size() == 2U, "backend receives two writes");
    ok &= check(backend->writes[0] == QByteArrayLiteral("abc"),
        "user write reaches backend");
    ok &= check(backend->writes[1] == QByteArrayLiteral("\x1b[24;80R"),
        "terminal reply uses backend write path");

    const std::vector<term::Terminal_session_command>& commands = session->processed_commands();
    ok &= check(commands.size() >= 3U &&
        commands[commands.size() - 2U].kind ==
            term::Terminal_session_command_kind::USER_WRITE &&
        commands[commands.size() - 1U].kind ==
            term::Terminal_session_command_kind::TERMINAL_REPLY,
        "user write and terminal reply share ordered session command stream");
    ok &= check(commands[commands.size() - 2U].sequence <
        commands[commands.size() - 1U].sequence,
        "user write and terminal reply receive monotonic sequence numbers");
    ok &= check_processed_sequences_are_monotonic(
        *session,
        "write/reply processed command stream stays monotonic");

    return ok;
}

bool test_output_backpressure_and_overflow()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session, tight_session_config());
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "backpressure session starts");

    ok &= check(backend->emit_output(QByteArrayLiteral("1234")),
        "fake backend emits output while running and unpaused");
    ok &= check(backend->output_pause_requests.size() == 2U &&
        backend->output_pause_requests[0] &&
        !backend->output_pause_requests[1],
        "session pauses and resumes backend output around high-water");
    ok &= check(notification_count(
        *session,
        term::Terminal_session_notification_kind::OUTPUT_BACKPRESSURE_CHANGED) == 2U,
        "session publishes output backpressure transitions");
    ok &= check(!session->output_backpressure_active(),
        "session releases output backpressure after drain");
    ok &= check(session->output_chunks().back() == QByteArrayLiteral("1234"),
        "high-water output is still delivered");
    const std::optional<term::Terminal_render_snapshot> pre_overflow_snapshot =
        session->latest_render_snapshot();
    const std::uint64_t pre_overflow_generation = session->render_snapshot_generation();
    ok &= check(pre_overflow_snapshot.has_value() &&
        snapshot_row_text(*pre_overflow_snapshot, 0) == QStringLiteral("1234"),
        "running session has snapshot before overflow");

    std::unique_ptr<term::Terminal_session> startup_overflow_session;
    Scripted_backend* startup_overflow_backend =
        make_session(startup_overflow_session, tight_session_config());
    startup_overflow_backend->outputs_during_start = {QByteArrayLiteral("123456789")};
    startup_overflow_backend->exit_on_terminate    = false;
    const term::Terminal_session_result startup_overflow_start =
        startup_overflow_session->start(valid_launch_config());
    ok &= check(startup_overflow_start.code ==
        term::Terminal_session_result_code::BACKEND_REJECTED,
        "startup output overflow rejects start result");
    ok &= check(startup_overflow_start.error.has_value() &&
        startup_overflow_start.error->code ==
            term::Terminal_backend_error_code::OUTPUT_OVERFLOW,
        "startup output overflow preserves typed error");
    ok &= check(!startup_overflow_session->backend_ready(),
        "startup output overflow leaves backend not ready");
    ok &= check(startup_overflow_session->write_user_bytes(QByteArrayLiteral("after")).code ==
        term::Terminal_session_result_code::INVALID_STATE,
        "startup output overflow leaves session non-writable");
    ok &= check(startup_overflow_backend->terminate_count == 1,
        "startup output overflow requests backend termination");
    ok &= check(!startup_overflow_session->latest_render_snapshot().has_value() &&
        !startup_overflow_session->last_model_ingest_result().has_value(),
        "startup output overflow does not mutate model or publish snapshot");

    const std::size_t output_chunk_count = session->output_chunks().size();
    const std::size_t snapshot_count = notification_count(
        *session,
        term::Terminal_session_notification_kind::SNAPSHOT_READY);
    ok &= check(backend->emit_output(QByteArrayLiteral("123456789")),
        "fake backend emits overflow chunk before session rejects it");
    const std::optional<term::Terminal_session_notification> overflow = first_notification(
        *session,
        term::Terminal_session_notification_kind::BACKEND_ERROR);
    ok &= check(overflow.has_value() && overflow->backend_error.has_value() &&
        overflow->backend_error->code == term::Terminal_backend_error_code::OUTPUT_OVERFLOW,
        "output hard-limit produces typed overflow error");
    ok &= check(backend->terminate_count == 1, "output overflow terminates backend");
    ok &= check(session->process_state() == term::Terminal_process_state::EXITED,
        "overflow termination reaches exited state");
    ok &= check(session->output_chunks().size() == output_chunk_count,
        "overflow chunk is not delivered to output chunks");
    ok &= check(notification_count(
        *session, term::Terminal_session_notification_kind::SNAPSHOT_READY) ==
        snapshot_count,
        "overflow chunk does not publish output snapshot notification");
    const std::optional<term::Terminal_render_snapshot> post_overflow_snapshot =
        session->latest_render_snapshot();
    ok &= check(post_overflow_snapshot.has_value() &&
        pre_overflow_snapshot.has_value() &&
        post_overflow_snapshot->metadata.sequence ==
            pre_overflow_snapshot->metadata.sequence &&
        snapshot_row_text(*post_overflow_snapshot, 0) == QStringLiteral("1234") &&
        session->render_snapshot_generation() == pre_overflow_generation,
        "overflow chunk preserves the previous runtime snapshot");

    std::unique_ptr<term::Terminal_session> delayed_overflow_session;
    Scripted_backend* delayed_overflow_backend =
        make_session(delayed_overflow_session, tight_session_config());
    delayed_overflow_backend->exit_on_terminate = false;
    ok &= check(delayed_overflow_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "delayed-overflow session starts");
    ok &= check(!delayed_overflow_session->latest_render_snapshot().has_value() &&
        !delayed_overflow_session->last_model_ingest_result().has_value(),
        "delayed-overflow session starts without published model state");
    ok &= check(delayed_overflow_backend->emit_output(QByteArrayLiteral("123456789")),
        "delayed-overflow fake backend emits overflow chunk");
    ok &= check(!delayed_overflow_session->latest_render_snapshot().has_value() &&
        !delayed_overflow_session->last_model_ingest_result().has_value(),
        "running output overflow does not mutate model or publish snapshot");
    ok &= check(!delayed_overflow_session->backend_ready(),
        "output overflow clears backend readiness immediately");
    ok &= check(delayed_overflow_session->write_user_bytes(QByteArrayLiteral("after")).code ==
        term::Terminal_session_result_code::INVALID_STATE,
        "write after output overflow fails before backend exit");
    ok &= check(delayed_overflow_backend->terminate_count == 1,
        "delayed output overflow requests one backend termination");
    ok &= check(!delayed_overflow_session->exit_status().has_value(),
        "delayed output overflow has no exit status before backend exit");
    ok &= check(notification_count(
        *delayed_overflow_session,
        term::Terminal_session_notification_kind::PROCESS_EXITED) == 0U,
        "delayed output overflow has no process-exited notification before backend exit");
    delayed_overflow_backend->emit_exit({term::Terminal_exit_reason::TERMINATED, 0});
    ok &= check(delayed_overflow_session->exit_status().has_value() &&
        delayed_overflow_session->exit_status()->reason ==
            term::Terminal_exit_reason::TERMINATED,
        "delayed output overflow records backend exit status");
    ok &= check(notification_count(
        *delayed_overflow_session,
        term::Terminal_session_notification_kind::PROCESS_EXITED) == 1U,
        "delayed output overflow emits one process-exited notification after exit");

    std::unique_ptr<term::Terminal_session> pause_failure_session;
    Scripted_backend* pause_failure_backend =
        make_session(pause_failure_session, tight_session_config());
    pause_failure_backend->output_pause_failures_remaining = 1;
    ok &= check(pause_failure_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "pause-failure session starts");
    ok &= check(pause_failure_backend->emit_output(QByteArrayLiteral("1234")),
        "pause-failure fake backend emits high-water output");
    const std::optional<term::Terminal_session_notification> pause_failure = first_notification(
        *pause_failure_session,
        term::Terminal_session_notification_kind::BACKEND_ERROR);
    ok &= check(pause_failure.has_value() && pause_failure->backend_error.has_value() &&
        pause_failure->backend_error->code ==
            term::Terminal_backend_error_code::OUTPUT_OVERFLOW,
        "output pause failure reports typed backend error");
    ok &= check(!pause_failure_session->output_backpressure_active(),
        "failed output pause does not publish active backpressure");

    std::unique_ptr<term::Terminal_session> resume_failure_session;
    Scripted_backend* resume_failure_backend =
        make_session(resume_failure_session, tight_session_config());
    resume_failure_backend->fail_output_pause_request_number = 2;
    ok &= check(resume_failure_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "resume-failure session starts");
    ok &= check(resume_failure_backend->emit_output(QByteArrayLiteral("1234")),
        "resume-failure fake backend emits high-water output");
    ok &= check(resume_failure_session->output_backpressure_active(),
        "failed output resume leaves backpressure active");
    const std::optional<term::Terminal_session_notification> resume_failure = first_notification(
        *resume_failure_session,
        term::Terminal_session_notification_kind::BACKEND_ERROR);
    ok &= check(resume_failure.has_value() && resume_failure->backend_error.has_value() &&
        resume_failure->backend_error->code ==
            term::Terminal_backend_error_code::OUTPUT_OVERFLOW,
        "output resume failure reports typed backend error");

    return ok;
}

bool test_write_limits_and_failure()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session, tight_session_config());
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "write-limit session starts");

    const term::Terminal_session_result high_water =
        session->write_user_bytes(QByteArrayLiteral("1234"));
    ok &= check(high_water.code == term::Terminal_session_result_code::ACCEPTED &&
        high_water.high_water_reached,
        "accepted user write reports write high-water");
    ok &= check(backend->writes.size() == 1U &&
        backend->writes.front() == QByteArrayLiteral("1234"),
        "high-water user write reaches backend");

    const term::Terminal_session_result oversized =
        session->write_user_bytes(QByteArrayLiteral("123456789"));
    ok &= check(oversized.code == term::Terminal_session_result_code::QUEUE_HARD_LIMIT_REACHED,
        "oversized write is rejected before backend write");
    ok &= check(backend->writes.size() == 1U,
        "oversized write sends no partial bytes");

    backend->fail_write = true;
    const term::Terminal_session_result failed =
        session->write_user_bytes(QByteArrayLiteral("ok"));
    ok &= check(failed.code == term::Terminal_session_result_code::BACKEND_REJECTED,
        "backend write failure is reported");
    ok &= check(failed.error.has_value() &&
        failed.error->code == term::Terminal_backend_error_code::WRITE_FAILED,
        "backend write failure carries typed error");

    term::Terminal_reply oversized_reply{
        QByteArrayLiteral("123456789"),
        QStringLiteral("oversized reply"),
        term::Terminal_reply_kind::RAW,
        term::Parser_sequence_family::NONE,
    };
    const term::Terminal_session_result oversized_reply_result =
        session->write_terminal_reply(oversized_reply);
    ok &= check(oversized_reply_result.code ==
        term::Terminal_session_result_code::QUEUE_HARD_LIMIT_REACHED,
        "oversized terminal reply shares write queue hard-limit path");

    oversized_reply.wire_bytes = QByteArrayLiteral("ok");
    const term::Terminal_session_result failed_reply =
        session->write_terminal_reply(oversized_reply);
    ok &= check(failed_reply.code == term::Terminal_session_result_code::BACKEND_REJECTED,
        "terminal reply shares backend write rejection path");
    ok &= check(failed_reply.error.has_value() &&
        failed_reply.error->code == term::Terminal_backend_error_code::WRITE_FAILED,
        "terminal reply backend rejection carries typed error");

    auto              no_trace_backend     = std::make_unique<Scripted_backend>();
    Scripted_backend* no_trace_backend_ptr = no_trace_backend.get();
    term::Terminal_session no_trace_session(std::move(no_trace_backend));
    ok &= check(no_trace_session.start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "no-trace session starts");
    no_trace_backend_ptr->fail_write = true;
    const term::Terminal_session_result no_trace_failed_write =
        no_trace_session.write_user_bytes(QByteArrayLiteral("ok"));
    ok &= check(no_trace_failed_write.code ==
        term::Terminal_session_result_code::BACKEND_REJECTED,
        "no-trace backend write failure is returned to caller");
    ok &= check(no_trace_failed_write.error.has_value() &&
        no_trace_failed_write.error->code ==
            term::Terminal_backend_error_code::WRITE_FAILED,
        "no-trace backend write failure preserves typed error");
    ok &= check(no_trace_session.notifications().empty() &&
        no_trace_session.processed_commands().empty() &&
        no_trace_session.output_chunks().empty(),
        "default session trace buffers are disabled");

    return ok;
}

bool test_paste_policy_modes_and_drain()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> not_started_session;
    Scripted_backend* not_started_backend = make_session(not_started_session);
    const term::Terminal_paste_text_result not_started =
        not_started_session->write_paste_text(
            QStringLiteral("ignored"),
            term::Terminal_paste_framing_policy::ENABLED);
    ok &= check(!not_started.handled,
        "not-started paste mirrors keyboard/IME no-op behavior");
    ok &= check(not_started_backend->writes.empty(),
        "not-started paste sends no backend bytes");

    std::unique_ptr<term::Terminal_session> policy_session;
    Scripted_backend* policy_backend = make_session(policy_session);
    ok &= check(policy_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "paste policy session starts");

    const term::Terminal_paste_text_result disabled =
        policy_session->write_paste_text(
            QStringLiteral("line1\r\nline2\rc"),
            term::Terminal_paste_framing_policy::DISABLED);
    ok &= check(disabled.handled &&
        disabled.result.code == term::Terminal_session_result_code::ACCEPTED,
        "disabled paste policy is accepted");
    ok &= check(policy_backend->writes.size() == 1U &&
        policy_backend->writes.back() == QByteArrayLiteral("line1\nline2\nc"),
        "disabled paste policy writes sanitized unframed text");

    const term::Terminal_paste_text_result enabled =
        policy_session->write_paste_text(
            QStringLiteral("multi\nline"),
            term::Terminal_paste_framing_policy::ENABLED);
    ok &= check(enabled.handled &&
        enabled.result.code == term::Terminal_session_result_code::ACCEPTED,
        "enabled paste policy is accepted");
    ok &= check(concatenate_writes(policy_backend->writes, 1U) ==
        framed_paste(QByteArrayLiteral("multi\nline")),
        "enabled paste policy writes a bracketed multiline frame");

    const std::size_t write_count_before_empty_paste = policy_backend->writes.size();
    const term::Terminal_paste_text_result empty =
        policy_session->write_paste_text(
            QString(QChar(0x001b)),
            term::Terminal_paste_framing_policy::ENABLED);
    ok &= check(!empty.handled,
        "paste that sanitizes to empty is treated as a no-op");
    ok &= check(policy_backend->writes.size() == write_count_before_empty_paste,
        "paste that sanitizes to empty sends no bracket frame");

    std::unique_ptr<term::Terminal_session> app_mode_session;
    term::Terminal_session_config config;
    config.backend_event_notifier = [] {};
    Scripted_backend* app_mode_backend = make_session(app_mode_session, config);
    ok &= check(app_mode_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "app-mode paste session starts");

    const term::Terminal_paste_text_result unframed =
        app_mode_session->write_paste_text(
            QStringLiteral("plain"),
            term::Terminal_paste_framing_policy::APPLICATION_CONTROLLED);
    ok &= check(unframed.handled &&
        unframed.result.code == term::Terminal_session_result_code::ACCEPTED,
        "application-controlled paste is accepted before DECSET");
    ok &= check(app_mode_backend->writes.size() == 1U &&
        app_mode_backend->writes.back() == QByteArrayLiteral("plain"),
        "application-controlled paste stays unframed before DECSET 2004");

    ok &= check(app_mode_backend->emit_output(QByteArrayLiteral("\x1b[?2004h")),
        "app-mode backend queues bracketed-paste DECSET");
    const term::Terminal_paste_text_result framed =
        app_mode_session->write_paste_text(
            QStringLiteral("mode"),
            term::Terminal_paste_framing_policy::APPLICATION_CONTROLLED);
    ok &= check(framed.handled &&
        framed.result.code == term::Terminal_session_result_code::ACCEPTED,
        "application-controlled paste after DECSET is accepted");
    ok &= check(concatenate_writes(app_mode_backend->writes, 1U) ==
        framed_paste(QByteArrayLiteral("mode")),
        "paste drains pending DECSET output before deciding bracketed mode");

    ok &= check(app_mode_backend->emit_output(QByteArrayLiteral("\x1b[?2004l")),
        "app-mode backend queues bracketed-paste DECRST");
    const std::size_t write_count_before_reset_paste = app_mode_backend->writes.size();
    const term::Terminal_paste_text_result reset =
        app_mode_session->write_paste_text(
            QStringLiteral("reset"),
            term::Terminal_paste_framing_policy::APPLICATION_CONTROLLED);
    ok &= check(reset.handled &&
        reset.result.code == term::Terminal_session_result_code::ACCEPTED,
        "application-controlled paste after DECRST is accepted");
    ok &= check(app_mode_backend->writes.size() == write_count_before_reset_paste + 1U &&
        app_mode_backend->writes.back() == QByteArrayLiteral("reset"),
        "application-controlled paste returns to unframed after DECRST 2004");

    return ok;
}

bool test_focus_reporting_mode_writes_and_drain()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> not_started_session;
    Scripted_backend* not_started_backend = make_session(not_started_session);
    const term::Terminal_focus_event_result not_started =
        not_started_session->write_focus_event(true);
    ok &= check(!not_started.handled,
        "not-started focus reporting is a no-op");
    ok &= check(not_started_backend->writes.empty(),
        "not-started focus reporting sends no backend bytes");

    term::Terminal_session_config config;
    config.backend_event_notifier = [] {};

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session, config);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "focus reporting session starts");

    const term::Terminal_focus_event_result disabled_in =
        session->write_focus_event(true);
    const term::Terminal_focus_event_result disabled_out =
        session->write_focus_event(false);
    ok &= check(!disabled_in.handled && !disabled_out.handled,
        "disabled focus reporting events are no-ops");
    ok &= check(backend->writes.empty(),
        "disabled focus reporting emits no bytes");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?1004h")),
        "focus reporting backend queues DECSET");
    const term::Terminal_focus_event_result focus_in =
        session->write_focus_event(true);
    ok &= check(focus_in.handled &&
        focus_in.result.code == term::Terminal_session_result_code::ACCEPTED,
        "focus-in reporting is accepted after DECSET 1004");
    ok &= check(backend->writes.size() == 1U &&
        backend->writes.back() == QByteArrayLiteral("\x1b[I"),
        "focus-in reporting writes CSI I");

    const term::Terminal_focus_event_result focus_out =
        session->write_focus_event(false);
    ok &= check(focus_out.handled &&
        focus_out.result.code == term::Terminal_session_result_code::ACCEPTED,
        "focus-out reporting is accepted after DECSET 1004");
    ok &= check(backend->writes.size() == 2U &&
        backend->writes.back() == QByteArrayLiteral("\x1b[O"),
        "focus-out reporting writes CSI O");

    ok &= check(backend->emit_output(numbered_scroll_lines(80)),
        "focus reporting scrollback fixture writes output");
    const term::Terminal_viewport_scroll_result detached_scroll =
        session->scroll_viewport_lines(3);
    ok &= check(detached_scroll.action ==
        term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "focus reporting fixture detaches viewport");
    std::optional<term::Terminal_render_snapshot> detached_snapshot =
        session->latest_render_snapshot();
    const std::uint64_t detached_generation = session->render_snapshot_generation();
    ok &= check(detached_snapshot.has_value() &&
        detached_snapshot->viewport.offset_from_tail == 3,
        "focus reporting fixture starts from detached viewport");

    const std::size_t detached_write_count = backend->writes.size();
    const term::Terminal_focus_event_result detached_focus =
        session->write_focus_event(true);
    detached_snapshot  = session->latest_render_snapshot();
    ok                &= check(detached_focus.handled &&
        detached_focus.result.code == term::Terminal_session_result_code::ACCEPTED,
        "focus reporting writes while viewport is detached");
    ok                &= check(backend->writes.size() == detached_write_count + 1U &&
        backend->writes.back() == QByteArrayLiteral("\x1b[I"),
        "detached focus reporting writes CSI I");
    ok                &= check(session->render_snapshot_generation() == detached_generation,
        "focus-in reporting preserves detached viewport generation");
    ok                &= check(detached_snapshot.has_value(),
        "focus-in reporting preserves latest detached snapshot");
    ok                &= check(detached_snapshot.has_value() &&
        detached_snapshot->viewport.offset_from_tail == 3,
        "focus-in reporting preserves detached viewport position");

    const std::size_t detached_focus_out_count = backend->writes.size();
    const term::Terminal_focus_event_result detached_focus_out =
        session->write_focus_event(false);
    detached_snapshot  = session->latest_render_snapshot();
    ok                &= check(detached_focus_out.handled &&
        detached_focus_out.result.code ==
            term::Terminal_session_result_code::ACCEPTED,
        "focus-out reporting writes while viewport is detached");
    ok                &= check(backend->writes.size() == detached_focus_out_count + 1U &&
        backend->writes.back() == QByteArrayLiteral("\x1b[O"),
        "detached focus-out reporting writes CSI O");
    ok                &= check(session->render_snapshot_generation() == detached_generation,
        "focus-out reporting preserves detached viewport generation");
    ok                &= check(detached_snapshot.has_value(),
        "focus-out reporting preserves latest detached snapshot");
    ok                &= check(detached_snapshot.has_value() &&
        detached_snapshot->viewport.offset_from_tail == 3,
        "focus-out reporting preserves detached viewport position");

    backend->fail_write = true;
    const std::size_t failed_write_count = backend->writes.size();
    const term::Terminal_focus_event_result failed_focus =
        session->write_focus_event(true);
    ok                  &= check(failed_focus.handled &&
        failed_focus.result.code == term::Terminal_session_result_code::BACKEND_REJECTED,
        "backend-rejected focus reporting remains handled");
    ok                  &= check(failed_focus.result.error.has_value() &&
        failed_focus.result.error->code == term::Terminal_backend_error_code::WRITE_FAILED,
        "backend-rejected focus reporting carries typed write failure");
    ok                  &= check(backend->writes.size() == failed_write_count,
        "backend-rejected focus reporting sends no accepted backend bytes");
    backend->fail_write  = false;

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?1004l")),
        "focus reporting backend queues DECRST");
    const std::size_t write_count_before_disabled = backend->writes.size();
    const term::Terminal_focus_event_result reset =
        session->write_focus_event(true);
    ok &= check(!reset.handled,
        "focus reporting after DECRST is a no-op");
    ok &= check(backend->writes.size() == write_count_before_disabled,
        "focus reporting after DECRST emits no bytes");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?1004h")),
        "focus reporting backend queues a second DECSET");
    const term::Terminal_focus_event_result before_exit =
        session->write_focus_event(true);
    ok &= check(before_exit.handled &&
        before_exit.result.code == term::Terminal_session_result_code::ACCEPTED,
        "focus reporting re-enables before backend exit");
    backend->emit_exit({term::Terminal_exit_reason::EXITED, 0});
    const std::size_t write_count_before_exit_focus = backend->writes.size();
    const term::Terminal_focus_event_result after_exit =
        session->write_focus_event(false);
    ok &= check(!after_exit.handled,
        "post-exit focus reporting is a no-op");
    ok &= check(backend->writes.size() == write_count_before_exit_focus,
        "post-exit focus reporting sends no backend bytes");

    return ok;
}

bool test_mouse_reporting_mode_writes_and_viewport()
{
    bool ok = true;

    {
        std::unique_ptr<term::Terminal_session> not_started_session;
        Scripted_backend* not_started_backend = make_session(not_started_session);
        const term::Terminal_mouse_event_result not_started =
            not_started_session->write_mouse_event({
                term::Terminal_mouse_event_kind::PRESS,
                term::Terminal_mouse_button::LEFT,
                3,
                4,
                Qt::NoModifier,
            });
        ok &= check(!not_started.handled,
            "not-started mouse reporting is a no-op");
        ok &= check(not_started_backend->writes.empty(),
            "not-started mouse reporting sends no backend bytes");
    }

    term::Terminal_session_config config;
    config.backend_event_notifier = [] {};

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session, config);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "mouse reporting session starts");

    const term::Terminal_mouse_event press{
        term::Terminal_mouse_event_kind::PRESS,
        term::Terminal_mouse_button::LEFT,
        3,
        4,
        Qt::NoModifier,
    };
    const term::Terminal_mouse_event release{
        term::Terminal_mouse_event_kind::RELEASE,
        term::Terminal_mouse_button::LEFT,
        3,
        4,
        Qt::NoModifier,
    };
    const term::Terminal_mouse_event passive_move{
        term::Terminal_mouse_event_kind::MOVE,
        term::Terminal_mouse_button::NONE,
        3,
        4,
        Qt::NoModifier,
    };

    ok &= check(!session->write_mouse_event(press).handled,
        "mouse reporting without tracking is a no-op");
    ok &= check(backend->writes.empty(),
        "mouse reporting without tracking writes no bytes");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?1000h")),
        "mouse reporting backend queues tracking without SGR");
    ok &= check(!session->write_mouse_event(press).handled,
        "mouse reporting without SGR 1006 is a no-op");
    ok &= check(backend->writes.empty(),
        "mouse reporting without SGR 1006 writes no bytes");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?1006h")),
        "mouse reporting backend queues SGR 1006");
    const term::Terminal_mouse_event_result pressed  = session->write_mouse_event(press);
    const term::Terminal_mouse_event_result released = session->write_mouse_event(release);
    ok &= check(pressed.handled &&
        pressed.result.code == term::Terminal_session_result_code::ACCEPTED,
        "SGR mouse press is accepted");
    ok &= check(released.handled &&
        released.result.code == term::Terminal_session_result_code::ACCEPTED,
        "SGR mouse release is accepted");
    ok &= check(backend->writes.size() == 2U &&
        backend->writes[0] == QByteArrayLiteral("\x1b[<0;5;4M") &&
        backend->writes[1] == QByteArrayLiteral("\x1b[<0;5;4m"),
        "SGR mouse press and release reach backend");

    ok &= check(backend->emit_output(numbered_scroll_lines(80)),
        "mouse reporting scrollback fixture writes output");
    const term::Terminal_viewport_scroll_result detached_scroll =
        session->scroll_viewport_lines(3);
    ok &= check(detached_scroll.action ==
        term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "mouse reporting fixture detaches viewport");
    std::optional<term::Terminal_render_snapshot> detached_snapshot =
        session->latest_render_snapshot();
    ok &= check(detached_snapshot.has_value() &&
        detached_snapshot->viewport.offset_from_tail == 3,
        "mouse reporting fixture starts from detached viewport");

    const std::uint64_t detached_generation = session->render_snapshot_generation();
    const term::Terminal_mouse_event_result detached_press =
        session->write_mouse_event(press);
    detached_snapshot  = session->latest_render_snapshot();
    ok                &= check(detached_press.handled &&
        detached_press.result.code == term::Terminal_session_result_code::ACCEPTED,
        "accepted mouse input writes while viewport is detached");
    ok                &= check(detached_snapshot.has_value() &&
        detached_snapshot->viewport.offset_from_tail == 0 &&
        session->render_snapshot_generation() == detached_generation + 1U,
        "accepted mouse input returns the viewport to tail");

    const term::Terminal_viewport_scroll_result passive_detached_scroll =
        session->scroll_viewport_lines(2);
    ok &= check(passive_detached_scroll.action ==
        term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "passive mouse reporting fixture detaches viewport");
    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?1003h")),
        "mouse reporting backend queues any-event tracking");
    const term::Terminal_mouse_event_result detached_move =
        session->write_mouse_event(passive_move);
    detached_snapshot  = session->latest_render_snapshot();
    ok                &= check(detached_move.handled &&
        detached_move.result.code == term::Terminal_session_result_code::ACCEPTED,
        "accepted passive mouse move writes while viewport is detached");
    ok                &= check(detached_snapshot.has_value() &&
        detached_snapshot->viewport.offset_from_tail == 2,
        "passive mouse move preserves detached viewport");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?1003;1006l")),
        "mouse reporting backend queues DECRST before next input");
    const std::size_t reset_write_count = backend->writes.size();
    const term::Terminal_mouse_event_result reset_press =
        session->write_mouse_event(press);
    ok &= check(!reset_press.handled,
        "pending DECRST disables mouse reporting before encoding");
    ok &= check(backend->writes.size() == reset_write_count,
        "pending DECRST mouse reporting no-op writes no bytes");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?1000;1006h")),
        "mouse reporting backend re-enables tracking after DECRST fixture");
    backend->fail_write = true;
    const std::size_t failed_write_count = backend->writes.size();
    const term::Terminal_mouse_event_result failed_press =
        session->write_mouse_event(press);
    ok &= check(failed_press.handled &&
        failed_press.result.code == term::Terminal_session_result_code::BACKEND_REJECTED,
        "backend-rejected mouse reporting remains handled");
    ok &= check(failed_press.result.error.has_value() &&
        failed_press.result.error->code == term::Terminal_backend_error_code::WRITE_FAILED,
        "backend-rejected mouse reporting carries typed write failure");
    ok &= check(backend->writes.size() == failed_write_count,
        "backend-rejected mouse reporting sends no accepted backend bytes");

    {
        std::unique_ptr<term::Terminal_session> exit_session;
        Scripted_backend* exit_backend = make_session(exit_session, config);
        ok &= check(exit_session->start(valid_launch_config()).code ==
            term::Terminal_session_result_code::ACCEPTED,
            "post-exit mouse reporting session starts");
        ok &= check(exit_backend->emit_output(QByteArrayLiteral("\x1b[?1000;1006h")),
            "post-exit mouse reporting backend queues modes");
        exit_backend->emit_exit({term::Terminal_exit_reason::EXITED, 0});
        const std::size_t exited_write_count = exit_backend->writes.size();
        const term::Terminal_mouse_event_result post_exit_press =
            exit_session->write_mouse_event(press);
        ok &= check(!post_exit_press.handled,
            "post-exit mouse reporting is a silent no-op");
        ok &= check(exit_backend->writes.size() == exited_write_count,
            "post-exit mouse reporting writes no backend bytes");
    }

    return ok;
}

bool test_paste_queue_atomicity_and_failures()
{
    bool ok = true;

    {
        const QByteArray expected = framed_paste(QByteArrayLiteral("abc"));
        term::Terminal_session_config config;
        config.write_queue_limits.high_water_bytes =
            static_cast<std::size_t>(expected.size());
        config.write_queue_limits.hard_limit_bytes =
            static_cast<std::size_t>(expected.size());

        std::unique_ptr<term::Terminal_session> session;
        Scripted_backend* backend = make_session(session, config);
        ok &= check(session->start(valid_launch_config()).code ==
            term::Terminal_session_result_code::ACCEPTED,
            "exact-capacity paste session starts");

        const term::Terminal_paste_text_result result =
            session->write_paste_text(
                QStringLiteral("abc"),
                term::Terminal_paste_framing_policy::ENABLED);
        ok &= check(result.handled &&
            result.result.code == term::Terminal_session_result_code::ACCEPTED &&
            result.result.high_water_reached,
            "exact-capacity paste is accepted and propagates high-water");
        ok &= check(backend->writes.size() == 1U && backend->writes.front() == expected,
            "exact-capacity paste writes the full frame atomically");
    }

    {
        const QByteArray rejected_frame = framed_paste(QByteArrayLiteral("abcd"));
        term::Terminal_session_config config;
        config.write_queue_limits.hard_limit_bytes =
            static_cast<std::size_t>(rejected_frame.size() - 1);

        std::unique_ptr<term::Terminal_session> session;
        Scripted_backend* backend = make_session(session, config);
        ok &= check(session->start(valid_launch_config()).code ==
            term::Terminal_session_result_code::ACCEPTED,
            "over-capacity paste session starts");

        const term::Terminal_paste_text_result rejected =
            session->write_paste_text(
                QStringLiteral("abcd"),
                term::Terminal_paste_framing_policy::ENABLED);
        ok &= check(rejected.handled &&
            rejected.result.code ==
                term::Terminal_session_result_code::QUEUE_HARD_LIMIT_REACHED,
            "over-capacity paste is rejected before backend write");
        ok &= check(rejected.result.error.has_value() &&
            rejected.result.error->code == term::Terminal_backend_error_code::WRITE_FAILED &&
            rejected.result.error->message.contains(QStringLiteral("paste")),
            "over-capacity paste carries paste-specific WRITE_FAILED error");
        ok &= check(backend->writes.empty(),
            "over-capacity paste sends no prefix or partial backend bytes");
    }

    {
        term::Terminal_session_config config;
        config.write_queue_limits.hard_limit_commands = 1U;

        std::unique_ptr<term::Terminal_session> session;
        Scripted_backend* backend = make_session(session, config);
        ok &= check(session->start(valid_launch_config()).code ==
            term::Terminal_session_result_code::ACCEPTED,
            "atomic paste session starts");

        const term::Terminal_paste_text_result pasted =
            session->write_paste_text(
                QStringLiteral("abcdefg"),
                term::Terminal_paste_framing_policy::ENABLED);
        ok &= check(pasted.handled &&
            pasted.result.code == term::Terminal_session_result_code::ACCEPTED,
            "paste is accepted as one queue command");
        ok &= check(backend->writes.size() == 1U &&
            backend->writes.front() == framed_paste(QByteArrayLiteral("abcdefg")),
            "paste reaches the backend as one atomic write");
        const std::vector<term::Terminal_session_command> commands =
            session->processed_commands();
        const auto paste_command_count = std::count_if(
            commands.begin(),
            commands.end(),
            [](const term::Terminal_session_command& command) {
                return command.kind == term::Terminal_session_command_kind::USER_PASTE;
            });
        ok &= check(paste_command_count == 1,
            "paste is traced as exactly one paste command");
    }

    {
        std::unique_ptr<term::Terminal_session> session;
        Scripted_backend* backend = make_session(session);
        ok &= check(session->start(valid_launch_config()).code ==
            term::Terminal_session_result_code::ACCEPTED,
            "post-exit paste session starts");
        backend->emit_exit({term::Terminal_exit_reason::EXITED, 0});

        const term::Terminal_paste_text_result after_exit =
            session->write_paste_text(
                QStringLiteral("after"),
                term::Terminal_paste_framing_policy::DISABLED);
        ok &= check(after_exit.handled &&
            after_exit.result.code == term::Terminal_session_result_code::INVALID_STATE,
            "paste after backend exit fails explicitly");
        ok &= check(after_exit.result.error.has_value() &&
            after_exit.result.error->code == term::Terminal_backend_error_code::WRITE_FAILED,
            "paste after backend exit carries WRITE_FAILED error");
        ok &= check(backend->writes.empty(),
            "paste after backend exit does not reach backend write");
    }

    {
        std::unique_ptr<term::Terminal_session> session;
        Scripted_backend* backend = make_session(session);
        ok &= check(session->start(valid_launch_config()).code ==
            term::Terminal_session_result_code::ACCEPTED,
            "backend-rejected paste session starts");
        backend->fail_write = true;

        const term::Terminal_paste_text_result failed =
            session->write_paste_text(
                QStringLiteral("failed"),
                term::Terminal_paste_framing_policy::ENABLED);
        ok &= check(failed.handled &&
            failed.result.code == term::Terminal_session_result_code::BACKEND_REJECTED,
            "backend-rejected paste follows write failure semantics");
        ok &= check(failed.result.error.has_value() &&
            failed.result.error->code == term::Terminal_backend_error_code::WRITE_FAILED,
            "backend-rejected paste preserves WRITE_FAILED error");
        ok &= check(backend->writes.empty(),
            "scripted backend rejection records no accepted paste write");
    }

    return ok;
}

bool test_resize_transactions()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> not_started_session;
    Scripted_backend* not_started_backend = make_session(not_started_session);
    const term::Terminal_session_result not_started_resize =
        not_started_session->resize(QSizeF(640.0, 320.0), {20, 80});
    ok &= check(not_started_resize.code == term::Terminal_session_result_code::INVALID_STATE,
        "resize before start fails explicitly");
    ok &= check(not_started_backend->resize_requests.empty(),
        "resize before start does not reach backend");
    ok &= check(not_started_session->resize_transactions().back().model_result ==
        term::Terminal_model_resize_result::NOT_APPLIED,
        "resize before start records unapplied transaction");
    ok &= check(not_started_session->grid_size().rows == 0 &&
        not_started_session->grid_size().columns == 0,
        "resize before start does not change grid size");

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    ok &= check(session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "resize session starts");

    const term::Terminal_session_result resize_result =
        session->resize(QSizeF(1000.0, 500.0), {25, 100});
    ok &= check(resize_result.code == term::Terminal_session_result_code::ACCEPTED,
        "resize accepted");
    ok &= check(backend->resize_requests.size() == 1U &&
        backend->resize_requests[0].transaction_id == 1U &&
        backend->resize_requests[0].grid_size.rows == 25 &&
        backend->resize_requests[0].grid_size.columns == 100,
        "backend resize receives transaction id and grid");
    ok &= check(session->resize_transactions().back().id == 1U &&
        session->resize_transactions().back().source_geometry == QSizeF(1000.0, 500.0) &&
        session->resize_transactions().back().snapshot_geometry == QSizeF(1000.0, 500.0) &&
        session->resize_transactions().back().snapshot_grid_size.rows == 25 &&
        session->resize_transactions().back().snapshot_grid_size.columns == 100 &&
        session->resize_transactions().back().backend_result ==
        term::Terminal_backend_resize_result::APPLIED,
        "resize transaction records backend success");
    ok &= check(session->backend_geometry_in_sync(), "successful resize leaves backend in sync");
    std::optional<term::Terminal_render_snapshot> resize_snapshot =
        session->latest_render_snapshot();
    ok &= check(resize_snapshot.has_value() &&
        resize_snapshot->grid_size.rows == session->grid_size().rows &&
        resize_snapshot->grid_size.columns == session->grid_size().columns &&
        resize_snapshot->grid_size.rows == 25 &&
        resize_snapshot->grid_size.columns == 100,
        "accepted resize updates snapshot grid to session grid");

    backend->fail_resize = true;
    const term::Terminal_session_result failed_resize =
        session->resize(QSizeF(800.0, 400.0), {20, 80});
    ok              &= check(failed_resize.code == term::Terminal_session_result_code::BACKEND_REJECTED,
        "failed backend resize is reported");
    ok              &= check(!session->backend_geometry_in_sync(),
        "failed backend resize marks geometry out of sync");
    ok              &= check(backend->resize_requests.size() == 2U &&
        backend->resize_requests[1].transaction_id == 2U &&
        backend->resize_requests[1].grid_size.rows == 20 &&
        backend->resize_requests[1].grid_size.columns == 80,
        "failed resize forwards second transaction id and grid");
    ok              &= check(session->resize_transactions().back().id == 2U &&
        session->resize_transactions().back().backend_result ==
        term::Terminal_backend_resize_result::FAILED,
        "resize transaction records backend failure");
    resize_snapshot  = session->latest_render_snapshot();
    ok              &= check(resize_snapshot.has_value() &&
        resize_snapshot->grid_size.rows == session->grid_size().rows &&
        resize_snapshot->grid_size.columns == session->grid_size().columns &&
        !resize_snapshot->metadata.backend_geometry_in_sync,
        "failed backend resize still keeps model snapshot grid consistent");

    const std::size_t backend_resize_count = backend->resize_requests.size();
    const term::Terminal_session_result invalid_resize =
        session->resize(QSizeF(800.0, 400.0), {0, 80});
    ok &= check(invalid_resize.code == term::Terminal_session_result_code::INVALID_ARGUMENT,
        "invalid resize grid fails explicitly");
    ok &= check(backend->resize_requests.size() == backend_resize_count,
        "invalid resize grid does not reach backend");
    ok &= check(session->resize_transactions().back().id == 3U &&
        session->resize_transactions().back().model_result ==
        term::Terminal_model_resize_result::INVALID_GRID_SIZE,
        "invalid resize transaction records model failure");

    const std::optional<term::Terminal_render_snapshot> previous_resize_snapshot =
        resize_snapshot;
    const term::Terminal_session_result oversized_resize =
        session->resize(QSizeF(800.0, 400.0), {
            24,
            term::k_terminal_screen_model_max_columns + 1,
        });
    ok &= check(oversized_resize.code == term::Terminal_session_result_code::INVALID_ARGUMENT,
        "oversized resize grid fails explicitly");
    ok &= check(backend->resize_requests.size() == backend_resize_count,
        "oversized resize grid does not reach backend");
    ok &= check(session->resize_transactions().back().id == 4U &&
        session->resize_transactions().back().model_result ==
            term::Terminal_model_resize_result::INVALID_GRID_SIZE &&
        previous_resize_snapshot.has_value() &&
        session->resize_transactions().back().snapshot_grid_size.rows ==
            previous_resize_snapshot->grid_size.rows &&
        session->resize_transactions().back().snapshot_grid_size.columns ==
            previous_resize_snapshot->grid_size.columns,
        "oversized resize records current snapshot grid without model allocation");
    ok &= check(session->latest_render_snapshot().has_value() &&
        previous_resize_snapshot.has_value() &&
        session->latest_render_snapshot()->metadata.sequence ==
            previous_resize_snapshot->metadata.sequence,
        "oversized resize preserves previous runtime snapshot");
    const term::Terminal_session_result oversized_cells_resize =
        session->resize(QSizeF(800.0, 400.0), {2000, 600});
    ok &= check(
        oversized_cells_resize.code == term::Terminal_session_result_code::INVALID_ARGUMENT,
        "resize grid exceeding cell cap fails explicitly");
    ok &= check(backend->resize_requests.size() == backend_resize_count,
        "resize grid exceeding cell cap does not reach backend");
    ok &= check(session->resize_transactions().back().id == 5U &&
        session->resize_transactions().back().model_result ==
            term::Terminal_model_resize_result::INVALID_GRID_SIZE,
        "resize exceeding cell cap records model failure without model allocation");

    const std::vector<term::Terminal_session_notification> resize_notifications = notifications_of_kind(
        *session,
        term::Terminal_session_notification_kind::RESIZE_TRANSACTION);
    ok &= check(resize_notifications.size() == 5U,
        "each resize creates one resize notification");
    ok &= check(resize_notifications[0].resize.has_value() &&
        resize_notifications[0].resize->id == 1U &&
        resize_notifications[0].resize->backend_result ==
            term::Terminal_backend_resize_result::APPLIED,
        "success resize notification carries transaction payload");
    ok &= check(resize_notifications[1].resize.has_value() &&
        resize_notifications[1].resize->id == 2U &&
        resize_notifications[1].resize->backend_result ==
            term::Terminal_backend_resize_result::FAILED,
        "failed resize notification carries transaction payload");
    ok &= check(resize_notifications[2].resize.has_value() &&
        resize_notifications[2].resize->id == 3U &&
        resize_notifications[2].resize->model_result ==
            term::Terminal_model_resize_result::INVALID_GRID_SIZE,
        "invalid resize notification carries transaction payload");
    ok &= check(resize_notifications[3].resize.has_value() &&
        resize_notifications[3].resize->id == 4U &&
        resize_notifications[3].resize->model_result ==
            term::Terminal_model_resize_result::INVALID_GRID_SIZE,
        "oversized resize notification carries transaction payload");
    ok &= check(resize_notifications[4].resize.has_value() &&
        resize_notifications[4].resize->id == 5U &&
        resize_notifications[4].resize->model_result ==
            term::Terminal_model_resize_result::INVALID_GRID_SIZE,
        "cell-cap resize notification carries transaction payload");

    backend->emit_exit({term::Terminal_exit_reason::EXITED, 0});
    const std::size_t resize_count_after_exit = backend->resize_requests.size();
    const term::Terminal_session_result resize_after_exit =
        session->resize(QSizeF(640.0, 320.0), {20, 80});
    ok &= check(resize_after_exit.code == term::Terminal_session_result_code::INVALID_STATE,
        "resize after exit fails explicitly");
    ok &= check(backend->resize_requests.size() == resize_count_after_exit,
        "resize after exit does not reach backend");

    return ok;
}

bool test_metrics_driven_resize_controller()
{
    bool ok = true;

    term::Fake_terminal_grid_metrics_provider metrics;
    metrics.set_cell_metrics({10.0, 20.0, 15.0, 5.0});

    std::unique_ptr<term::Terminal_session> pre_start_session;
    Scripted_backend* pre_start_backend = make_session(pre_start_session);
    term::Terminal_resize_controller pre_start_controller(*pre_start_session, metrics);
    const term::Terminal_session_result pre_start_resize =
        pre_start_controller.resize_from_geometry(QSizeF(800.0, 400.0));
    ok &= check(pre_start_resize.code == term::Terminal_session_result_code::INVALID_STATE,
        "metrics-driven resize before start fails explicitly");
    ok &= check(pre_start_backend->resize_requests.empty(),
        "metrics-driven resize before start does not reach backend");
    ok &= check(pre_start_backend->start_configs.empty(),
        "metrics-driven resize before start does not start backend");

    std::unique_ptr<term::Terminal_session> initial_session;
    Scripted_backend* initial_backend = make_session(initial_session);
    term::Terminal_resize_controller initial_controller(*initial_session, metrics);
    const term::Terminal_session_result initial_start =
        initial_controller.start_from_geometry(valid_launch_config(), QSizeF(1000.0, 500.0));
    ok &= check(initial_start.code == term::Terminal_session_result_code::ACCEPTED,
        "metrics-driven initial start is accepted");
    ok &= check(initial_backend->start_configs.size() == 1U &&
        initial_backend->start_configs[0].initial_grid_size.has_value() &&
        initial_backend->start_configs[0].initial_grid_size->rows == 25 &&
        initial_backend->start_configs[0].initial_grid_size->columns == 100,
        "backend start sees metrics-computed initial grid");
    ok &= check(initial_backend->resize_requests.empty(),
        "metrics-driven initial geometry sends no backend resize");

    std::unique_ptr<term::Terminal_session> invalid_geometry_session;
    Scripted_backend* invalid_geometry_backend = make_session(invalid_geometry_session);
    term::Terminal_resize_controller invalid_geometry_controller(
        *invalid_geometry_session,
        metrics);
    const term::Terminal_session_result invalid_geometry_start =
        invalid_geometry_controller.start_from_geometry(
            valid_launch_config(),
            QSizeF(0.0, 500.0));
    ok &= check(
        invalid_geometry_start.code == term::Terminal_session_result_code::INVALID_ARGUMENT,
        "invalid initial geometry fails explicitly");
    ok &= check(invalid_geometry_start.error.has_value() &&
        invalid_geometry_start.error->code ==
            term::Terminal_backend_error_code::INVALID_INITIAL_GRID_SIZE,
        "invalid initial geometry carries typed start error");
    ok &= check(invalid_geometry_backend->start_configs.empty(),
        "invalid initial geometry does not call backend start");

    std::unique_ptr<term::Terminal_session> non_finite_geometry_session;
    Scripted_backend* non_finite_geometry_backend =
        make_session(non_finite_geometry_session);
    term::Terminal_resize_controller non_finite_geometry_controller(
        *non_finite_geometry_session,
        metrics);
    const term::Terminal_session_result non_finite_geometry_start =
        non_finite_geometry_controller.start_from_geometry(
            valid_launch_config(),
            QSizeF(std::numeric_limits<qreal>::infinity(), 500.0));
    ok &= check(
        non_finite_geometry_start.code == term::Terminal_session_result_code::INVALID_ARGUMENT,
        "non-finite initial geometry fails explicitly");
    ok &= check(non_finite_geometry_backend->start_configs.empty(),
        "non-finite initial geometry does not call backend start");

    term::Fake_terminal_grid_metrics_provider invalid_metrics;
    invalid_metrics.set_cell_metrics({0.0, 20.0, 15.0, 5.0});
    std::unique_ptr<term::Terminal_session> invalid_metrics_session;
    Scripted_backend* invalid_metrics_backend = make_session(invalid_metrics_session);
    term::Terminal_resize_controller invalid_metrics_controller(
        *invalid_metrics_session,
        invalid_metrics);
    const term::Terminal_session_result invalid_metrics_start =
        invalid_metrics_controller.start_from_geometry(
            valid_launch_config(),
            QSizeF(1000.0, 500.0));
    ok &= check(
        invalid_metrics_start.code == term::Terminal_session_result_code::INVALID_ARGUMENT,
        "invalid initial cell metrics fail explicitly");
    ok &= check(invalid_metrics_backend->start_configs.empty(),
        "invalid initial cell metrics do not call backend start");

    std::unique_ptr<term::Terminal_session> live_session;
    Scripted_backend* live_backend = make_session(live_session);
    term::Terminal_resize_controller live_controller(*live_session, metrics);
    ok &= check(live_controller.start_from_geometry(
        valid_launch_config(),
        QSizeF(800.0, 400.0)).code == term::Terminal_session_result_code::ACCEPTED,
        "metrics-driven live resize session starts");

    const term::Terminal_session_result larger_resize =
        live_controller.resize_from_geometry(QSizeF(1000.0, 500.0));
    ok &= check(larger_resize.code == term::Terminal_session_result_code::ACCEPTED,
        "metrics-driven larger resize is accepted");
    ok &= check(live_backend->resize_requests.size() == 1U &&
        live_backend->resize_requests[0].grid_size.rows == 25 &&
        live_backend->resize_requests[0].grid_size.columns == 100,
        "larger geometry forwards computed grid to backend");
    ok &= check(live_session->resize_transactions().back().source_geometry ==
        QSizeF(1000.0, 500.0),
        "larger geometry is recorded on resize transaction");

    const term::Terminal_session_result smaller_resize =
        live_controller.resize_from_geometry(QSizeF(640.0, 300.0));
    ok &= check(smaller_resize.code == term::Terminal_session_result_code::ACCEPTED,
        "metrics-driven smaller resize is accepted");
    ok &= check(live_backend->resize_requests.size() == 2U &&
        live_backend->resize_requests[1].grid_size.rows == 15 &&
        live_backend->resize_requests[1].grid_size.columns == 64 &&
        live_session->resize_transactions().back().source_geometry ==
            QSizeF(640.0, 300.0),
        "smaller geometry is recorded and forwarded");

    const std::size_t resize_count_before_same_grid = live_backend->resize_requests.size();
    const std::size_t no_op_resize_transaction_count =
        live_session->resize_transactions().size();
    const std::size_t no_op_notification_count = live_session->notifications().size();
    const std::uint64_t no_op_snapshot_generation =
        live_session->render_snapshot_generation();
    const term::Terminal_session_result same_grid_refresh =
        live_controller.refresh_from_geometry(QSizeF(649.0, 319.0));
    ok &= check(same_grid_refresh.code == term::Terminal_session_result_code::ACCEPTED,
        "same-grid geometry refresh is accepted");
    ok &= check(live_backend->resize_requests.size() == resize_count_before_same_grid,
        "same-grid geometry refresh is a backend no-op while in sync");

    metrics.set_cell_metrics({9.9, 19.5, 14.0, 5.5});
    const term::Terminal_session_result same_grid_metrics_refresh =
        live_controller.refresh_from_geometry(QSizeF(640.0, 300.0));
    ok &= check(same_grid_metrics_refresh.code == term::Terminal_session_result_code::ACCEPTED,
        "same-grid metrics refresh is accepted");
    ok &= check(live_backend->resize_requests.size() == resize_count_before_same_grid,
        "same-grid metrics refresh is a backend no-op while in sync");

    const term::Terminal_session_result same_grid_resize =
        live_controller.resize_from_geometry(QSizeF(640.0, 300.0));
    ok &= check(same_grid_resize.code == term::Terminal_session_result_code::ACCEPTED,
        "same-grid resize is accepted");
    ok &= check(live_backend->resize_requests.size() == resize_count_before_same_grid,
        "same-grid resize is a backend no-op while in sync");
    ok &= check(live_session->resize_transactions().size() ==
        no_op_resize_transaction_count &&
        live_session->notifications().size() == no_op_notification_count &&
        live_session->render_snapshot_generation() == no_op_snapshot_generation,
        "same-grid controller no-ops leave session traces and snapshot generation unchanged");

    metrics.set_cell_metrics({0.0, 20.0, 15.0, 5.0});
    const term::Terminal_session_result invalid_live_metrics =
        live_controller.refresh_from_geometry(QSizeF(640.0, 300.0));
    ok &= check(
        invalid_live_metrics.code == term::Terminal_session_result_code::INVALID_ARGUMENT,
        "invalid live metrics refresh fails explicitly");
    ok &= check(invalid_live_metrics.error.has_value() &&
        invalid_live_metrics.error->code == term::Terminal_backend_error_code::RESIZE_FAILED,
        "invalid live metrics refresh carries typed resize error");
    ok &= check(live_backend->resize_requests.size() == resize_count_before_same_grid,
        "invalid live metrics refresh does not call backend resize");

    metrics.set_cell_metrics({10.0, 20.0, 15.0, 5.0});
    const term::Terminal_session_result invalid_live_geometry =
        live_controller.refresh_from_geometry(QSizeF(0.0, 300.0));
    ok &= check(
        invalid_live_geometry.code == term::Terminal_session_result_code::INVALID_ARGUMENT,
        "invalid live geometry refresh fails explicitly");
    ok &= check(live_backend->resize_requests.size() == resize_count_before_same_grid,
        "invalid live geometry refresh does not call backend resize");

    const term::Terminal_session_result out_of_range_live_geometry =
        live_controller.refresh_from_geometry(QSizeF(
            static_cast<qreal>(std::numeric_limits<int>::max()) * 20.0,
            300.0));
    ok &= check(
        out_of_range_live_geometry.code == term::Terminal_session_result_code::INVALID_ARGUMENT,
        "out-of-range live geometry refresh fails explicitly");
    ok &= check(live_backend->resize_requests.size() == resize_count_before_same_grid,
        "out-of-range live geometry refresh does not call backend resize");

    metrics.set_cell_metrics({8.0, 16.0, 12.0, 4.0});
    const term::Terminal_session_result metrics_change_resize =
        live_controller.refresh_from_geometry(QSizeF(640.0, 300.0));
    ok &= check(metrics_change_resize.code == term::Terminal_session_result_code::ACCEPTED,
        "metrics change for same geometry is accepted");
    ok &= check(live_backend->resize_requests.size() == resize_count_before_same_grid + 1U &&
        live_backend->resize_requests.back().grid_size.rows == 18 &&
        live_backend->resize_requests.back().grid_size.columns == 80,
        "metrics change for same geometry propagates changed grid");

    std::unique_ptr<term::Terminal_session> retry_session;
    Scripted_backend* retry_backend = make_session(retry_session);
    metrics.set_cell_metrics({10.0, 20.0, 15.0, 5.0});
    term::Terminal_resize_controller retry_controller(*retry_session, metrics);
    ok &= check(retry_controller.start_from_geometry(
        valid_launch_config(),
        QSizeF(800.0, 400.0)).code == term::Terminal_session_result_code::ACCEPTED,
        "retry resize session starts");

    retry_backend->fail_resize = true;
    const term::Terminal_session_result failed_resize =
        retry_controller.resize_from_geometry(QSizeF(1000.0, 500.0));
    ok &= check(failed_resize.code == term::Terminal_session_result_code::BACKEND_REJECTED,
        "metrics-driven backend resize failure is reported");
    ok &= check(!retry_session->backend_geometry_in_sync(),
        "metrics-driven backend resize failure marks session out of sync");
    const std::size_t failed_retry_count = retry_backend->resize_requests.size();

    retry_backend->fail_resize = false;
    const term::Terminal_session_result retry_resize =
        retry_controller.refresh_from_geometry(QSizeF(1000.0, 500.0));
    ok &= check(retry_resize.code == term::Terminal_session_result_code::ACCEPTED,
        "same-grid out-of-sync refresh retries backend resize");
    ok &= check(retry_backend->resize_requests.size() == failed_retry_count + 1U &&
        retry_backend->resize_requests.back().grid_size.rows == 25 &&
        retry_backend->resize_requests.back().grid_size.columns == 100 &&
        retry_session->backend_geometry_in_sync(),
        "successful same-grid retry restores backend geometry sync");

    retry_backend->fail_resize = true;
    const term::Terminal_session_result changed_grid_failed_resize =
        retry_controller.resize_from_geometry(QSizeF(900.0, 460.0));
    ok &= check(
        changed_grid_failed_resize.code == term::Terminal_session_result_code::BACKEND_REJECTED,
        "changed-grid metrics-driven backend resize failure is reported");
    ok &= check(!retry_session->backend_geometry_in_sync(),
        "changed-grid backend resize failure marks session out of sync");
    const std::size_t changed_grid_failed_retry_count = retry_backend->resize_requests.size();

    retry_backend->fail_resize = false;
    const term::Terminal_session_result changed_grid_retry_resize =
        retry_controller.refresh_from_geometry(QSizeF(920.0, 480.0));
    ok &= check(
        changed_grid_retry_resize.code == term::Terminal_session_result_code::ACCEPTED,
        "changed-grid out-of-sync refresh retries backend resize");
    ok &= check(
        retry_backend->resize_requests.size() == changed_grid_failed_retry_count + 1U &&
            retry_backend->resize_requests.back().grid_size.rows == 24 &&
            retry_backend->resize_requests.back().grid_size.columns == 92 &&
            retry_session->backend_geometry_in_sync(),
        "successful changed-grid retry restores backend geometry sync");

    std::unique_ptr<term::Terminal_session> exit_session;
    Scripted_backend* exit_backend = make_session(exit_session);
    term::Terminal_resize_controller exit_controller(*exit_session, metrics);
    ok &= check(exit_controller.start_from_geometry(
        valid_launch_config(),
        QSizeF(800.0, 400.0)).code == term::Terminal_session_result_code::ACCEPTED,
        "metrics-driven resize-after-exit session starts");
    exit_backend->emit_exit({term::Terminal_exit_reason::EXITED, 0});
    const std::size_t resize_count_before_exit_resize =
        exit_backend->resize_requests.size();
    const term::Terminal_session_result exit_resize =
        exit_controller.resize_from_geometry(QSizeF(1000.0, 500.0));
    ok &= check(exit_resize.code == term::Terminal_session_result_code::INVALID_STATE,
        "metrics-driven resize after exit fails explicitly");
    ok &= check(exit_backend->resize_requests.size() == resize_count_before_exit_resize,
        "metrics-driven resize after exit does not reach backend");
    const term::Terminal_session_result same_grid_exit_resize =
        exit_controller.refresh_from_geometry(QSizeF(800.0, 400.0));
    ok &= check(same_grid_exit_resize.code == term::Terminal_session_result_code::INVALID_STATE,
        "same-grid metrics-driven resize after exit fails explicitly");
    ok &= check(exit_backend->resize_requests.size() == resize_count_before_exit_resize,
        "same-grid metrics-driven resize after exit does not reach backend");

    std::unique_ptr<term::Terminal_session> alternate_session;
    Scripted_backend* alternate_backend = make_session(alternate_session);
    term::Terminal_resize_controller alternate_controller(*alternate_session, metrics);
    ok &= check(alternate_controller.start_from_geometry(
        valid_launch_config(),
        QSizeF(800.0, 400.0)).code == term::Terminal_session_result_code::ACCEPTED,
        "alternate-buffer resize session starts");
    ok &= check(alternate_backend->emit_output(QByteArrayLiteral("\x1b[?1049h")),
        "alternate-buffer session enters alternate buffer");
    const term::Terminal_session_result alternate_resize =
        alternate_controller.resize_from_geometry(QSizeF(1000.0, 500.0));
    ok &= check(alternate_resize.code == term::Terminal_session_result_code::ACCEPTED,
        "alternate-buffer metrics-driven resize is accepted");
    ok &= check(alternate_session->resize_transactions().back().active_buffer ==
        term::Terminal_buffer_id::ALTERNATE,
        "alternate-buffer resize transaction records active alternate buffer");
    const std::optional<term::Terminal_render_snapshot> alternate_snapshot =
        alternate_session->latest_render_snapshot();
    ok &= check(alternate_snapshot.has_value() &&
        alternate_snapshot->viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE &&
        alternate_snapshot->grid_size.rows == 25 &&
        alternate_snapshot->grid_size.columns == 100,
        "alternate-buffer resize snapshot remains alternate");

    alternate_backend->fail_resize = true;
    const term::Terminal_session_result failed_alternate_resize =
        alternate_controller.resize_from_geometry(QSizeF(920.0, 480.0));
    ok &= check(
        failed_alternate_resize.code == term::Terminal_session_result_code::BACKEND_REJECTED,
        "failed alternate-buffer resize reports backend rejection");
    ok &= check(alternate_session->resize_transactions().back().active_buffer ==
        term::Terminal_buffer_id::ALTERNATE,
        "failed alternate-buffer resize transaction records active alternate buffer");
    const std::optional<term::Terminal_render_snapshot> failed_alternate_snapshot =
        alternate_session->latest_render_snapshot();
    ok &= check(failed_alternate_snapshot.has_value() &&
        failed_alternate_snapshot->viewport.active_buffer ==
            term::Terminal_buffer_id::ALTERNATE &&
        !failed_alternate_snapshot->metadata.backend_geometry_in_sync,
        "failed alternate-buffer resize snapshot remains alternate and out of sync");

    alternate_backend->fail_resize = false;
    ok &= check(alternate_backend->emit_output(QByteArrayLiteral("\x1b[?1049l")),
        "alternate-buffer session returns to primary buffer");
    const term::Terminal_session_result primary_resize =
        alternate_controller.resize_from_geometry(QSizeF(800.0, 400.0));
    ok &= check(primary_resize.code == term::Terminal_session_result_code::ACCEPTED,
        "primary-buffer metrics-driven resize after alternate is accepted");
    ok &= check(alternate_session->resize_transactions().back().active_buffer ==
        term::Terminal_buffer_id::PRIMARY,
        "primary-buffer resize transaction records active primary buffer");
    const std::optional<term::Terminal_render_snapshot> primary_snapshot =
        alternate_session->latest_render_snapshot();
    ok &= check(primary_snapshot.has_value() &&
        primary_snapshot->viewport.active_buffer == term::Terminal_buffer_id::PRIMARY,
        "primary-buffer resize snapshot remains primary");

    return ok;
}

bool test_metrics_driven_resize_interleaves_with_output()
{
    bool ok = true;

    term::Fake_terminal_grid_metrics_provider metrics;
    metrics.set_cell_metrics({10.0, 20.0, 15.0, 5.0});

    std::unique_ptr<term::Terminal_session> session;
    Scripted_backend* backend = make_session(session);
    term::Terminal_resize_controller controller(*session, metrics);
    ok &= check(controller.start_from_geometry(
        valid_launch_config(),
        QSizeF(800.0, 400.0)).code == term::Terminal_session_result_code::ACCEPTED,
        "interleaved metrics-driven resize session starts");
    ok &= check(backend->emit_output(QByteArrayLiteral("A")),
        "interleaved session receives prefix output");

    const term::Terminal_session_result resize_result =
        controller.resize_from_geometry(QSizeF(1000.0, 500.0));
    ok &= check(resize_result.code == term::Terminal_session_result_code::ACCEPTED,
        "interleaved metrics-driven resize is accepted");
    ok &= check(backend->emit_output(QByteArrayLiteral("B")),
        "interleaved session receives suffix output");

    const std::vector<term::Terminal_session_command> commands = session->processed_commands();
    ok &= check(commands.size() == 4U &&
        commands[0].kind == term::Terminal_session_command_kind::START &&
        commands[1].kind == term::Terminal_session_command_kind::BACKEND_OUTPUT &&
        commands[2].kind == term::Terminal_session_command_kind::RESIZE &&
        commands[3].kind == term::Terminal_session_command_kind::BACKEND_OUTPUT &&
        commands[1].sequence < commands[2].sequence &&
        commands[2].sequence < commands[3].sequence,
        "plain output and metrics-driven resize preserve exact command ordering");
    ok &= check_processed_sequences_are_monotonic(
        *session,
        "interleaved metrics-driven command stream stays monotonic");

    const std::optional<term::Terminal_render_snapshot> snapshot =
        session->latest_render_snapshot();
    ok &= check(snapshot.has_value() &&
        snapshot->grid_size.rows == 25 &&
        snapshot->grid_size.columns == 100 &&
        snapshot_row_text(*snapshot, 0) == QStringLiteral("AB") &&
        !commands.empty() &&
        snapshot->metadata.sequence == commands.back().sequence,
        "plain output interleaved with metrics-driven resize preserves snapshot consistency");

    return ok;
}

bool test_exit_failed_start_and_double_stop()
{
    bool ok = true;

    std::unique_ptr<term::Terminal_session> failed_session;
    Scripted_backend* failed_backend = make_session(failed_session);
    failed_backend->fail_start                    = true;
    failed_backend->report_error_on_start_failure = true;
    const term::Terminal_session_result start_result =
        failed_session->start(valid_launch_config());
    ok &= check(start_result.code == term::Terminal_session_result_code::BACKEND_REJECTED,
        "failed start is rejected");
    ok &= check(failed_session->process_state() == term::Terminal_process_state::FAILED,
        "failed start moves session to failed state");
    ok &= check(notification_count(
        *failed_session, term::Terminal_session_notification_kind::BACKEND_ERROR) == 1U,
        "failed start reports one backend error");

    std::unique_ptr<term::Terminal_session> exit_session;
    Scripted_backend* exit_backend = make_session(exit_session);
    ok &= check(exit_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "exit session starts");
    exit_backend->emit_exit({term::Terminal_exit_reason::EXITED, 7});
    ok &= check(exit_session->process_state() == term::Terminal_process_state::EXITED,
        "backend exit updates process state");
    ok &= check(exit_session->exit_status().has_value() &&
        exit_session->exit_status()->exit_code == 7,
        "backend exit status is retained");
    const term::Terminal_session_result write_after_exit =
        exit_session->write_user_bytes(QByteArrayLiteral("after"));
    ok &= check(write_after_exit.code == term::Terminal_session_result_code::INVALID_STATE,
        "write after exit fails explicitly");
    const term::Terminal_session_result interrupt_after_exit = exit_session->interrupt();
    ok &= check(interrupt_after_exit.code == term::Terminal_session_result_code::INVALID_STATE &&
        interrupt_after_exit.error.has_value() &&
        interrupt_after_exit.error->code ==
            term::Terminal_backend_error_code::INTERRUPT_FAILED,
        "interrupt after exit carries typed interrupt failure");
    ok &= check(exit_session->write_user_bytes(QByteArrayLiteral("123456789")).code ==
        term::Terminal_session_result_code::INVALID_STATE,
        "oversized write after exit reports lifecycle state before queue capacity");
    const term::Terminal_session_result restart_after_exit =
        exit_session->start(valid_launch_config());
    ok &= check(restart_after_exit.code == term::Terminal_session_result_code::INVALID_STATE,
        "restart after exit fails until restart lifecycle is implemented");
    ok &= check(restart_after_exit.error.has_value() &&
        restart_after_exit.error->code ==
            term::Terminal_backend_error_code::START_FAILED,
        "restart after exit carries typed start failure");
    ok &= check(exit_backend->start_configs.size() == 1U,
        "restart after exit does not reach backend start");
    exit_backend->emit_exit({term::Terminal_exit_reason::TERMINATED, 9});
    ok &= check(exit_session->exit_status().has_value() &&
        exit_session->exit_status()->reason == term::Terminal_exit_reason::EXITED &&
        exit_session->exit_status()->exit_code == 7,
        "duplicate backend exit does not overwrite first exit status");
    ok &= check(notification_count(
        *exit_session, term::Terminal_session_notification_kind::PROCESS_EXITED) == 1U,
        "duplicate backend exit does not emit a second process-exited notification");

    const std::size_t failed_start_count = failed_backend->start_configs.size();
    const term::Terminal_session_result restart_after_failure =
        failed_session->start(valid_launch_config());
    ok &= check(restart_after_failure.code == term::Terminal_session_result_code::INVALID_STATE,
        "restart after failed start fails until restart lifecycle is implemented");
    ok &= check(restart_after_failure.error.has_value() &&
        restart_after_failure.error->code == term::Terminal_backend_error_code::START_FAILED,
        "restart after failed start carries typed start failure");
    ok &= check(failed_backend->start_configs.size() == failed_start_count,
        "restart after failed start does not reach backend start");

    std::unique_ptr<term::Terminal_session> ignored_interrupt_session;
    Scripted_backend* ignored_interrupt_backend =
        make_session(ignored_interrupt_session);
    ignored_interrupt_backend->exit_on_interrupt = false;
    ok &= check(ignored_interrupt_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "ignored-interrupt session starts");
    ok &= check(ignored_interrupt_session->interrupt().code ==
        term::Terminal_session_result_code::ACCEPTED,
        "ignored interrupt is accepted");
    ok &= check(ignored_interrupt_backend->interrupt_count == 1,
        "ignored interrupt reaches backend once");
    ok &= check(ignored_interrupt_session->process_state() ==
        term::Terminal_process_state::RUNNING,
        "ignored interrupt leaves process running");
    ok &= check(ignored_interrupt_session->backend_ready(),
        "ignored interrupt leaves backend ready for recovery");
    ok &= check(ignored_interrupt_session->write_user_bytes(QByteArrayLiteral("after-int")).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "write after ignored interrupt remains accepted");
    ok &= check(ignored_interrupt_session->terminate().code ==
        term::Terminal_session_result_code::ACCEPTED,
        "terminate after ignored interrupt remains accepted");
    ok &= check(ignored_interrupt_backend->terminate_count == 1,
        "terminate after ignored interrupt reaches backend once");
    ok &= check(ignored_interrupt_session->exit_status().has_value() &&
        ignored_interrupt_session->exit_status()->reason ==
            term::Terminal_exit_reason::TERMINATED,
        "terminate after ignored interrupt records backend exit");

    std::unique_ptr<term::Terminal_session> stop_session;
    Scripted_backend* stop_backend = make_session(stop_session);
    ok &= check(stop_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "double-stop session starts");
    ok &= check(stop_session->terminate().code == term::Terminal_session_result_code::ACCEPTED,
        "first terminate accepted");
    ok &= check(stop_backend->terminate_count == 1, "backend terminate called once");
    const term::Terminal_session_result second_terminate = stop_session->terminate();
    ok &= check(second_terminate.code == term::Terminal_session_result_code::INVALID_STATE,
        "second terminate fails explicitly");
    ok &= check(second_terminate.error.has_value() &&
        second_terminate.error->code ==
            term::Terminal_backend_error_code::TERMINATE_FAILED,
        "second terminate carries typed terminate failure");
    ok &= check(stop_backend->terminate_count == 1, "second terminate does not reach backend");

    std::unique_ptr<term::Terminal_session> delayed_stop_session;
    Scripted_backend* delayed_stop_backend = make_session(delayed_stop_session);
    delayed_stop_backend->exit_on_terminate = false;
    ok &= check(delayed_stop_session->start(valid_launch_config()).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "delayed-stop session starts");
    ok &= check(delayed_stop_session->terminate().code ==
        term::Terminal_session_result_code::ACCEPTED,
        "delayed terminate accepted");
    ok &= check(!delayed_stop_session->backend_ready(),
        "delayed terminate clears backend readiness immediately");
    ok &= check(delayed_stop_session->write_user_bytes(QByteArrayLiteral("after")).code ==
        term::Terminal_session_result_code::INVALID_STATE,
        "write during delayed terminate fails explicitly");
    ok &= check(delayed_stop_session->resize(QSizeF(800.0, 600.0), {30, 100}).code ==
        term::Terminal_session_result_code::INVALID_STATE,
        "resize during delayed terminate fails explicitly");
    ok &= check(!delayed_stop_session->exit_status().has_value(),
        "delayed terminate has no exit status before backend exit");
    ok &= check(notification_count(
        *delayed_stop_session,
        term::Terminal_session_notification_kind::PROCESS_EXITED) == 0U,
        "delayed terminate has no process-exited notification before backend exit");
    ok &= check(delayed_stop_session->terminate().code ==
        term::Terminal_session_result_code::INVALID_STATE,
        "second delayed terminate fails explicitly before backend exit");
    ok &= check(delayed_stop_backend->terminate_count == 1,
        "second delayed terminate does not reach backend");
    delayed_stop_backend->emit_exit({term::Terminal_exit_reason::TERMINATED, 0});
    ok &= check(delayed_stop_session->process_state() == term::Terminal_process_state::EXITED,
        "delayed backend exit completes terminate");
    ok &= check(delayed_stop_session->exit_status().has_value() &&
        delayed_stop_session->exit_status()->reason ==
            term::Terminal_exit_reason::TERMINATED,
        "delayed terminate records backend exit status");
    ok &= check(notification_count(
        *delayed_stop_session,
        term::Terminal_session_notification_kind::PROCESS_EXITED) == 1U,
        "delayed terminate emits one process-exited notification after exit");

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_start_callback_ordering_and_output();
    ok &= test_backend_output_capture_file();
    ok &= test_text_area_resize_request_updates_session_grid_in_sequence();
    ok &= test_text_area_resize_retry_publishes_geometry_metadata();
    ok &= test_backend_output_capture_records_callback_overflow_bytes();
    ok &= test_backend_output_updates_latest_render_snapshot();
    ok &= test_selection_snapshot_and_visible_text();
    ok &= test_selection_snapshot_during_blocked_publication();
    ok &= test_blocked_detach_resize_snapshot_metadata_coherence();
    ok &= test_selection_phase2_internal_state_and_lease();
    ok &= test_selection_phase2_replacement_empty_cancel_and_eviction();
    ok &= test_selection_phase2_model_semantic_flags();
    ok &= test_selection_phase2_session_lease_basis_advances();
    ok &= test_selection_phase3_line_lease_provenance_capture();
    ok &= test_selection_phase3_line_lease_negative_source_paths();
    ok &= test_selection_phase5_visual_lease_span_compatibility();
    ok &= test_selection_spans_preserve_after_same_viewport_idempotent_selected_row_rewrite();
    ok &= test_selection_spans_preserve_during_scrollback_growth_with_retained_lines();
    ok &= test_selection_spans_detach_when_selected_row_mutates();
    ok &= test_selection_spans_detach_when_retained_row_moves();
    ok &= test_selection_spans_preserve_after_unchanged_synchronized_output_release();
    ok &= test_selection_spans_detach_when_synchronized_release_mutates_selected_row();
    ok &= test_selection_spans_detach_when_synchronized_release_moves_retained_row();
    ok &= test_selection_spans_fail_closed_at_phase4c_boundaries();
    ok &= test_selection_spans_detach_when_resize_invalidates_selected_columns();
    ok &= test_selection_unicode_cluster_payloads();
    ok &= test_output_activity_notifications_are_session_level();
    ok &= test_synchronized_output_defers_content_until_release();
    ok &= test_viewport_scroll_public_session_path();
    ok &= test_resize_preserves_primary_scrollback();
    ok &= test_resize_repaint_clear_keeps_primary_scrollback_when_recovery_is_enabled();
    ok &= test_cursor_home_line_repaint_does_not_synthesize_primary_scrollback();
    ok &= test_cursor_home_text_then_el_repaint_synthesizes_primary_scrollback();
    ok &= test_parser_notifications_reach_session_notifications();
    ok &= test_bell_policy_coalesces_with_deterministic_clock();
    ok &= test_parser_state_crosses_backend_output_chunks();
    ok &= test_backend_output_replies_use_write_path();
    ok &= test_mixed_output_query_ordering();
    ok &= test_terminal_canvas_fixture_script_through_session();
    ok &= test_generated_reply_write_failure_reports_backend_error();
    ok &= test_generated_reply_byte_enqueue_failure_reports_backend_error();
    ok &= test_generated_reply_command_enqueue_failure_reports_backend_error();
    ok &= test_reentrant_start_callbacks_preserve_order();
    ok &= test_callback_during_write_is_serialized();
    ok &= test_destructor_ignores_late_backend_callbacks();
    ok &= test_worker_thread_callback_is_delivered();
    ok &= test_deferred_callback_ingress_merges_adjacent_output();
    ok &= test_budgeted_backend_callback_drain_yields_inside_coalesced_output();
    ok &= test_budgeted_backend_callback_drain_holds_output_command_backpressure();
    ok &= test_deferred_callback_ingress_pauses_backend_at_high_water();
    ok &= test_deferred_callback_ingress_overflow_is_bounded();
    ok &= test_deferred_callback_ingress_error_count_is_bounded();
    ok &= test_pending_notification_limit_preserves_recent_critical_events();
    ok &= test_invalid_launch_config();
    ok &= test_write_and_reply_path();
    ok &= test_output_backpressure_and_overflow();
    ok &= test_write_limits_and_failure();
    ok &= test_paste_policy_modes_and_drain();
    ok &= test_focus_reporting_mode_writes_and_drain();
    ok &= test_mouse_reporting_mode_writes_and_viewport();
    ok &= test_paste_queue_atomicity_and_failures();
    ok &= test_resize_transactions();
    ok &= test_metrics_driven_resize_controller();
    ok &= test_metrics_driven_resize_interleaves_with_output();
    ok &= test_exit_failed_start_and_double_stop();
    return ok ? 0 : 1;
}
