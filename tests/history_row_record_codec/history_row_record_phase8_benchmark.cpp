#include "vnm_terminal/internal/terminal_history_row_record_codec.h"
#include "vnm_terminal/internal/terminal_history_row_traversal.h"
#include "vnm_terminal/internal/terminal_screen_model.h"

#include <QByteArray>
#include <QString>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

constexpr std::size_t k_phase8_retained_history_ring_capacity_bytes =
    64U * 1024U * 1024U;
constexpr int k_phase8_columns = 120;
constexpr int k_phase8_row_count = 2000;

constexpr std::uint64_t k_codec_header_bytes = 116U;
constexpr std::uint64_t k_codec_footer_bytes = 32U;
constexpr std::uint64_t k_dense_cell_base_bytes = 24U;
constexpr std::uint64_t k_hyperlink_base_bytes = 12U;
constexpr std::uint64_t k_projected_run_base_bytes = 32U;

enum class Phase8_row_pattern
{
    BLANK,
    DENSE_ASCII,
    REPEATED_ASCII,
    HYPERLINK_HEAVY,
};

struct benchmark_payload_t
{
    bool                         ok = true;
    std::uint64_t                records = 0U;
    std::uint64_t                bytes = 0U;
    std::uint64_t                checksum = 0U;
};

struct benchmark_measurement_t
{
    std::string                  name;
    std::uint64_t                operations = 0U;
    std::uint64_t                elapsed_ns = 0U;
    std::uint64_t                records = 0U;
    std::uint64_t                bytes = 0U;
    std::uint64_t                checksum = 0U;
    bool                         ok = true;
};

struct row_size_projection_t
{
    std::string                  scenario;
    std::uint64_t                cells = 0U;
    std::uint64_t                hyperlinks = 0U;
    std::uint64_t                dense_record_bytes = 0U;
    std::uint64_t                projected_run_length_record_bytes = 0U;
};

bool report_failure(const char* message)
{
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

template <typename Fn>
benchmark_measurement_t measure_case(
    std::string     name,
    std::uint64_t   operations,
    Fn              fn)
{
    const auto started = std::chrono::steady_clock::now();
    const benchmark_payload_t payload = fn();
    const auto finished = std::chrono::steady_clock::now();

    benchmark_measurement_t measurement;
    measurement.name       = std::move(name);
    measurement.operations = operations;
    measurement.elapsed_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            finished - started).count());
    measurement.records  = payload.records;
    measurement.bytes    = payload.bytes;
    measurement.checksum = payload.checksum;
    measurement.ok       = payload.ok;
    return measurement;
}

term::Terminal_history_row_cell make_cell(
    QString                 text,
    term::Terminal_style_id style_id = term::k_default_terminal_style_id,
    std::uint64_t           hyperlink_id = 0U)
{
    term::Terminal_history_row_cell cell;
    cell.text         = std::move(text);
    cell.occupied     = true;
    cell.style_id     = style_id;
    cell.hyperlink_id = hyperlink_id;
    return cell;
}

term::Terminal_history_row_record make_record(
    std::uint64_t       row_sequence,
    int                 columns,
    Phase8_row_pattern  pattern)
{
    term::Terminal_history_row_record record;
    record.provenance.retained_line_id   = row_sequence;
    record.provenance.content_generation = row_sequence + 1000U;
    record.provenance.source =
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE;
    record.metadata.source_width = columns;
    record.metadata.style_lifetime =
        term::Terminal_retained_row_style_lifetime::SESSION_LIFETIME_STYLE_ID;
    record.metadata.wrap_state =
        term::Terminal_retained_row_wrap_state::HARD_BOUNDARY;

    if (pattern == Phase8_row_pattern::BLANK) {
        record.cells.resize(static_cast<std::size_t>(columns));
        return record;
    }

    record.cells.reserve(static_cast<std::size_t>(columns));
    for (int column = 0; column < columns; ++column) {
        if (pattern == Phase8_row_pattern::REPEATED_ASCII) {
            record.cells.push_back(make_cell(QStringLiteral("x")));
            continue;
        }

        if (pattern == Phase8_row_pattern::HYPERLINK_HEAVY) {
            const std::uint64_t hyperlink_id =
                (row_sequence * 1000U) + static_cast<std::uint64_t>(column + 1);
            record.cells.push_back(make_cell(QStringLiteral("L"), 1U, hyperlink_id));
            record.hyperlink_identity_keys.emplace(
                hyperlink_id,
                QByteArrayLiteral("uri:https://example.test/phase8/") +
                    QByteArray::number(static_cast<qlonglong>(row_sequence)) +
                    QByteArrayLiteral("/") +
                    QByteArray::number(column));
            continue;
        }

        const char text = static_cast<char>('A' + ((row_sequence + column) % 26U));
        record.cells.push_back(make_cell(QString(QChar(QLatin1Char(text)))));
    }

    return record;
}

term::terminal_history_row_record_identity_t make_identity(
    std::uint64_t                   row_sequence,
    term::terminal_history_handle_t previous_handle)
{
    return {
        11U,
        row_sequence,
        previous_handle.byte_sequence,
        previous_handle.row_sequence,
    };
}

term::Terminal_history_row_record_append_result append_record(
    term::Terminal_history_ring&    ring,
    std::uint64_t                   row_sequence,
    Phase8_row_pattern              pattern,
    term::terminal_history_handle_t previous_handle)
{
    return term::encode_terminal_history_row_record_to_ring(
        ring,
        make_record(row_sequence, k_phase8_columns, pattern),
        make_identity(row_sequence, previous_handle));
}

bool append_fixture_rows(
    term::Terminal_history_ring&                    ring,
    std::vector<term::terminal_history_handle_t>&    handles,
    int                                             row_count,
    Phase8_row_pattern                              pattern,
    std::uint64_t&                                  bytes)
{
    handles.clear();
    handles.reserve(static_cast<std::size_t>(row_count));
    bytes = 0U;

    term::terminal_history_handle_t previous_handle;
    for (int row = 0; row < row_count; ++row) {
        const std::uint64_t row_sequence = static_cast<std::uint64_t>(row + 1);
        const term::Terminal_history_row_record_append_result append =
            append_record(ring, row_sequence, pattern, previous_handle);
        if (append.status != term::Terminal_history_row_record_codec_status::OK) {
            return report_failure("Phase 8 fixture row append failed");
        }

        handles.push_back(append.history_handle);
        previous_handle = append.history_handle;
        bytes += append.commit.record_bytes;
    }

    return true;
}

std::uint64_t dense_record_bytes(const term::Terminal_history_row_record& record)
{
    std::uint64_t bytes =
        term::terminal_history_ring_record_overhead_bytes() +
        k_codec_header_bytes +
        k_codec_footer_bytes;

    for (const term::Terminal_history_row_cell& cell : record.cells) {
        bytes += k_dense_cell_base_bytes;
        bytes += static_cast<std::uint64_t>(cell.text.toUtf8().size());
    }

    for (const auto& hyperlink : record.hyperlink_identity_keys) {
        bytes += k_hyperlink_base_bytes;
        bytes += static_cast<std::uint64_t>(hyperlink.second.size());
    }

    return bytes;
}

bool cells_share_projected_run(
    const term::Terminal_history_row_cell& left,
    const term::Terminal_history_row_cell& right)
{
    return
        left.text              == right.text              &&
        left.display_width     == right.display_width     &&
        left.wide_continuation == right.wide_continuation &&
        left.occupied          == right.occupied          &&
        left.style_id          == right.style_id          &&
        left.hyperlink_id      == right.hyperlink_id;
}

std::uint64_t projected_run_length_record_bytes(
    const term::Terminal_history_row_record& record)
{
    std::uint64_t bytes =
        term::terminal_history_ring_record_overhead_bytes() +
        k_codec_header_bytes +
        k_codec_footer_bytes;

    std::size_t index = 0U;
    while (index < record.cells.size()) {
        const term::Terminal_history_row_cell& first = record.cells[index];
        bytes += k_projected_run_base_bytes;
        bytes += static_cast<std::uint64_t>(first.text.toUtf8().size());

        ++index;
        while (index < record.cells.size() &&
            cells_share_projected_run(first, record.cells[index]))
        {
            ++index;
        }
    }

    for (const auto& hyperlink : record.hyperlink_identity_keys) {
        bytes += k_hyperlink_base_bytes;
        bytes += static_cast<std::uint64_t>(hyperlink.second.size());
    }

    return bytes;
}

std::vector<row_size_projection_t> row_size_projections()
{
    const std::vector<std::pair<std::string, Phase8_row_pattern>> scenarios = {
        {"blank_120_columns",           Phase8_row_pattern::BLANK},
        {"dense_ascii_120_columns",     Phase8_row_pattern::DENSE_ASCII},
        {"repeated_ascii_120_columns",  Phase8_row_pattern::REPEATED_ASCII},
        {"hyperlink_120_columns",       Phase8_row_pattern::HYPERLINK_HEAVY},
    };

    std::vector<row_size_projection_t> projections;
    projections.reserve(scenarios.size());
    for (const auto& scenario : scenarios) {
        const term::Terminal_history_row_record record =
            make_record(1U, k_phase8_columns, scenario.second);
        projections.push_back({
            scenario.first,
            static_cast<std::uint64_t>(record.cells.size()),
            static_cast<std::uint64_t>(record.hyperlink_identity_keys.size()),
            dense_record_bytes(record),
            projected_run_length_record_bytes(record),
        });
    }
    return projections;
}

term::Terminal_render_snapshot_request request_for_model(
    const term::Terminal_screen_model& model,
    std::uint64_t                      sequence,
    int                                offset_from_tail)
{
    term::Terminal_viewport_state viewport;
    viewport.active_buffer = model.active_buffer_id();
    viewport.visible_rows  = model.grid_size().rows;
    viewport.scrollback_rows =
        model.active_buffer_id() == term::Terminal_buffer_id::PRIMARY
            ? model.scrollback_size()
            : 0;
    viewport.offset_from_tail = offset_from_tail;
    viewport.follow_tail      = offset_from_tail == 0;

    term::Terminal_render_snapshot_request request;
    request.sequence = sequence;
    request.viewport = viewport;
    request.viewport_changed = true;
    return request;
}

QByteArray plain_output_stream(int row_count, int columns)
{
    QByteArray stream;
    for (int row = 0; row < row_count; ++row) {
        QByteArray text = QByteArrayLiteral("phase8-row-") + QByteArray::number(row);
        while (text.size() < columns - 1) {
            text += QByteArrayLiteral("x");
        }
        stream += text.left(columns - 1);
        stream += QByteArrayLiteral("\r\n");
    }
    return stream;
}

QByteArray hyperlink_output_stream(int row_count, int columns)
{
    QByteArray stream;
    for (int row = 0; row < row_count; ++row) {
        const QByteArray row_id = QByteArrayLiteral("phase8-") + QByteArray::number(row);
        stream += QByteArrayLiteral("\x1b]8;id=");
        stream += row_id;
        stream += QByteArrayLiteral(";https://example.test/phase8/");
        stream += QByteArray::number(row);
        stream += QByteArrayLiteral("\x1b\\");
        for (int column = 0; column < columns - 1; ++column) {
            stream += static_cast<char>('a' + ((row + column) % 26));
        }
        stream += QByteArrayLiteral("\x1b]8;;\x1b\\\r\n");
    }
    return stream;
}

bool result_has_diagnostic(const term::Terminal_screen_model_result& result)
{
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) == term::Parser_action_kind::DIAGNOSTIC) {
            return true;
        }
    }

    return false;
}

benchmark_measurement_t benchmark_append()
{
    return measure_case(
        "append_dense_rows",
        k_phase8_row_count,
        [] {
            benchmark_payload_t payload;
            term::Terminal_history_ring ring({
                k_phase8_retained_history_ring_capacity_bytes,
                0U,
            });
            if (!ring.ok()) {
                payload.ok = report_failure("Phase 8 append benchmark ring failed");
                return payload;
            }

            term::terminal_history_handle_t previous_handle;
            for (int row = 0; row < k_phase8_row_count; ++row) {
                const std::uint64_t row_sequence = static_cast<std::uint64_t>(row + 1);
                const term::Terminal_history_row_record_append_result append =
                    append_record(
                        ring,
                        row_sequence,
                        Phase8_row_pattern::DENSE_ASCII,
                        previous_handle);
                if (append.status != term::Terminal_history_row_record_codec_status::OK) {
                    payload.ok = report_failure("Phase 8 append benchmark append failed");
                    return payload;
                }

                previous_handle = append.history_handle;
                payload.bytes += append.commit.record_bytes;
                payload.checksum += append.history_handle.row_sequence;
            }

            payload.records = k_phase8_row_count;
            return payload;
        });
}

benchmark_measurement_t benchmark_materialization()
{
    term::Terminal_history_ring ring({
        k_phase8_retained_history_ring_capacity_bytes,
        0U,
    });
    std::vector<term::terminal_history_handle_t> handles;
    std::uint64_t fixture_bytes = 0U;
    const bool fixture_ok = ring.ok() &&
        append_fixture_rows(
            ring,
            handles,
            k_phase8_row_count,
            Phase8_row_pattern::DENSE_ASCII,
            fixture_bytes);

    return measure_case(
        "materialization_dense_rows",
        handles.size(),
        [&] {
            benchmark_payload_t payload;
            payload.ok = fixture_ok;
            if (!payload.ok) {
                return payload;
            }

            for (term::terminal_history_handle_t handle : handles) {
                const term::Terminal_history_ring_read_scope read =
                    ring.read_record(handle.byte_sequence);
                const term::Terminal_history_row_record_decode_result decoded =
                    term::decode_terminal_history_row_record(read, handle);
                if (decoded.status != term::Terminal_history_row_record_codec_status::OK) {
                    payload.ok = report_failure(
                        "Phase 8 materialization benchmark decode failed");
                    return payload;
                }

                payload.checksum += decoded.record.cells.size();
                payload.bytes += handle.record_bytes;
            }

            payload.records = static_cast<std::uint64_t>(handles.size());
            return payload;
        });
}

benchmark_measurement_t benchmark_traversal()
{
    term::Terminal_history_ring ring({
        k_phase8_retained_history_ring_capacity_bytes,
        0U,
    });
    std::vector<term::terminal_history_handle_t> handles;
    std::uint64_t fixture_bytes = 0U;
    const bool fixture_ok = ring.ok() &&
        append_fixture_rows(
            ring,
            handles,
            k_phase8_row_count,
            Phase8_row_pattern::DENSE_ASCII,
            fixture_bytes);

    term::Terminal_history_row_traversal traversal(ring);
    const term::Terminal_history_row_traversal_rebuild_result rebuild =
        traversal.rebuild_directory();
    const bool rebuild_ok =
        rebuild.status == term::Terminal_history_row_traversal_status::OK;

    return measure_case(
        "traversal_forward_rows",
        handles.size(),
        [&] {
            benchmark_payload_t payload;
            payload.ok = fixture_ok && rebuild_ok;
            if (!payload.ok) {
                return payload;
            }

            term::Terminal_history_row_traversal_result current =
                traversal.oldest_live_row();
            if (current.status != term::Terminal_history_row_traversal_status::OK) {
                payload.ok = report_failure("Phase 8 traversal oldest row failed");
                return payload;
            }

            payload.records = 1U;
            payload.bytes += current.row.history_handle.record_bytes;
            payload.checksum += current.row.history_handle.row_sequence;

            while (true) {
                term::Terminal_history_row_traversal_result next =
                    traversal.next_row_after(current.row.history_handle);
                if (next.status == term::Terminal_history_row_traversal_status::NOT_FOUND) {
                    break;
                }
                if (next.status != term::Terminal_history_row_traversal_status::OK) {
                    payload.ok = report_failure("Phase 8 traversal next row failed");
                    return payload;
                }

                current = std::move(next);
                ++payload.records;
                payload.bytes += current.row.history_handle.record_bytes;
                payload.checksum += current.row.history_handle.row_sequence;
            }

            return payload;
        });
}

benchmark_measurement_t benchmark_cache_rebuild()
{
    constexpr int rebuild_count = 10;

    term::Terminal_history_ring ring({
        k_phase8_retained_history_ring_capacity_bytes,
        0U,
    });
    std::vector<term::terminal_history_handle_t> handles;
    std::uint64_t fixture_bytes = 0U;
    const bool fixture_ok = ring.ok() &&
        append_fixture_rows(
            ring,
            handles,
            k_phase8_row_count,
            Phase8_row_pattern::DENSE_ASCII,
            fixture_bytes);

    term::Terminal_history_row_traversal traversal(ring);
    return measure_case(
        "cache_rebuild_row_directory",
        static_cast<std::uint64_t>(handles.size() * rebuild_count),
        [&] {
            benchmark_payload_t payload;
            payload.ok = fixture_ok;
            if (!payload.ok) {
                return payload;
            }

            for (int rebuild_index = 0; rebuild_index < rebuild_count; ++rebuild_index) {
                traversal.discard_directory_cache();
                const term::Terminal_history_row_traversal_rebuild_result rebuild =
                    traversal.rebuild_directory();
                if (rebuild.status != term::Terminal_history_row_traversal_status::OK) {
                    payload.ok = report_failure("Phase 8 cache rebuild failed");
                    return payload;
                }

                payload.records += static_cast<std::uint64_t>(rebuild.row_count);
                payload.checksum += static_cast<std::uint64_t>(rebuild.row_count);
            }

            payload.bytes = fixture_bytes * rebuild_count;
            return payload;
        });
}

benchmark_measurement_t benchmark_resize_projection()
{
    constexpr int resize_rounds = 16;
    const std::vector<int> widths = {80, 120, 160, 96};

    term::Terminal_screen_model model({
        term::terminal_grid_size_t{24, k_phase8_columns},
        768,
        8,
    });
    const term::Terminal_screen_model_result ingest =
        model.ingest(plain_output_stream(640, k_phase8_columns));
    const bool fixture_ok = !result_has_diagnostic(ingest) && model.scrollback_size() > 0;

    return measure_case(
        "resize_projection_retained_rows",
        static_cast<std::uint64_t>(resize_rounds * widths.size()),
        [&] {
            benchmark_payload_t payload;
            payload.ok = fixture_ok;
            if (!payload.ok) {
                return payload;
            }

            for (int round = 0; round < resize_rounds; ++round) {
                for (int width : widths) {
                    const term::Terminal_screen_model_result resize =
                        model.resize({24, width});
                    if (!resize.grid_reflow_changed) {
                        payload.ok = report_failure("Phase 8 resize projection did not reflow");
                        return payload;
                    }

                    const int offset =
                        std::min(model.scrollback_size(), model.grid_size().rows);
                    const term::Terminal_render_snapshot snapshot =
                        model.render_snapshot(request_for_model(
                            model,
                            static_cast<std::uint64_t>(round + width),
                            offset));
                    if (term::validate_render_snapshot(snapshot).status !=
                        term::Terminal_render_snapshot_status::OK)
                    {
                        payload.ok = report_failure(
                            "Phase 8 resize projection snapshot failed validation");
                        return payload;
                    }

                    ++payload.records;
                    payload.checksum += snapshot.cells.size();
                    payload.bytes += static_cast<std::uint64_t>(snapshot.cells.size());
                }
            }

            return payload;
        });
}

benchmark_measurement_t benchmark_selection_extraction()
{
    constexpr int selection_repetitions = 200;

    term::Terminal_screen_model model({
        term::terminal_grid_size_t{24, k_phase8_columns},
        768,
        8,
    });
    const term::Terminal_screen_model_result ingest =
        model.ingest(plain_output_stream(640, k_phase8_columns));

    const int first_row = std::max(0, model.scrollback_size() / 3);
    const int last_row = std::min(
        first_row + 72,
        model.scrollback_size() + model.grid_size().rows - 1);
    const term::Terminal_selection_range selection = {
        {first_row, 0},
        {last_row, 90},
        term::Terminal_selection_mode::NORMAL,
    };
    const std::vector<term::terminal_selection_line_lease_t> leases =
        model.selection_line_leases(term::Terminal_buffer_id::PRIMARY, selection);
    const bool fixture_ok = !result_has_diagnostic(ingest) && !leases.empty();

    return measure_case(
        "selection_extraction_retained_rows",
        static_cast<std::uint64_t>(selection_repetitions),
        [&] {
            benchmark_payload_t payload;
            payload.ok = fixture_ok;
            if (!payload.ok) {
                return payload;
            }

            for (int repeat = 0; repeat < selection_repetitions; ++repeat) {
                const term::Terminal_selection_result selected =
                    model.selected_text(
                        term::Terminal_buffer_id::PRIMARY,
                        selection,
                        std::span<const term::terminal_selection_line_lease_t>(leases));
                if (selected.code != term::Terminal_selection_result_code::OK) {
                    payload.ok = report_failure("Phase 8 selection extraction failed");
                    return payload;
                }

                ++payload.records;
                payload.checksum += static_cast<std::uint64_t>(selected.text.size());
                payload.bytes += static_cast<std::uint64_t>(
                    selected.text.toUtf8().size());
            }

            return payload;
        });
}

benchmark_measurement_t benchmark_hyperlink_heavy_output()
{
    constexpr int hyperlink_rows = 220;
    constexpr int hyperlink_columns = 80;

    return measure_case(
        "hyperlink_heavy_output_and_snapshot",
        hyperlink_rows,
        [] {
            benchmark_payload_t payload;
            term::Terminal_screen_model model({
                term::terminal_grid_size_t{24, hyperlink_columns},
                256,
                8,
            });

            const term::Terminal_screen_model_result ingest =
                model.ingest(hyperlink_output_stream(hyperlink_rows, hyperlink_columns));
            if (result_has_diagnostic(ingest) || model.scrollback_size() == 0) {
                payload.ok = report_failure("Phase 8 hyperlink-heavy output ingest failed");
                return payload;
            }

            const int offset = std::min(model.scrollback_size(), model.grid_size().rows);
            const term::Terminal_render_snapshot snapshot =
                model.render_snapshot(request_for_model(model, 800U, offset));
            if (term::validate_render_snapshot(snapshot).status !=
                term::Terminal_render_snapshot_status::OK)
            {
                payload.ok = report_failure(
                    "Phase 8 hyperlink-heavy output snapshot failed validation");
                return payload;
            }

            payload.records = hyperlink_rows;
            payload.bytes = static_cast<std::uint64_t>(snapshot.hyperlinks.size());
            payload.checksum =
                static_cast<std::uint64_t>(snapshot.cells.size()) +
                static_cast<std::uint64_t>(snapshot.hyperlinks.size());
            return payload;
        });
}

double ns_per_operation(const benchmark_measurement_t& measurement)
{
    if (measurement.operations == 0U) {
        return 0.0;
    }

    return static_cast<double>(measurement.elapsed_ns) /
        static_cast<double>(measurement.operations);
}

void write_json_string(std::ostream& output, const std::string& text)
{
    output << '"';
    for (char ch : text) {
        switch (ch) {
        case '\\':
            output << "\\\\";
            break;
        case '"':
            output << "\\\"";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            output << ch;
            break;
        }
    }
    output << '"';
}

bool write_report(
    const std::string&                          path,
    const std::vector<benchmark_measurement_t>& measurements,
    const std::vector<row_size_projection_t>&   projections,
    const term::Terminal_history_ring&          limits_ring)
{
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        std::cerr << "FAIL: unable to write Phase 8 benchmark report: " << path << '\n';
        return false;
    }

    output << "{\n";
    output << "  \"phase\": \"8\",\n";
    output << "  \"generated_date\": \"2026-05-30\",\n";
    output << "  \"configuration\": \"MSVC x64 focused benchmark target\",\n";
    output << "  \"representation_decision\": \"keep_dense_encoding\",\n";
    output << "  \"ring_limits\": {\n";
    output << "    \"retained_history_ring_capacity_bytes\": "
           << limits_ring.capacity_bytes() << ",\n";
    output << "    \"max_record_bytes\": " << limits_ring.max_record_bytes() << ",\n";
    output << "    \"max_payload_bytes\": " << limits_ring.max_payload_bytes() << ",\n";
    output << "    \"screen_model_max_rows\": "
           << term::k_terminal_screen_model_max_rows << ",\n";
    output << "    \"screen_model_max_columns\": "
           << term::k_terminal_screen_model_max_columns << ",\n";
    output << "    \"screen_model_max_cells\": "
           << term::k_terminal_screen_model_max_cells << "\n";
    output << "  },\n";
    output << "  \"row_size_projections\": [\n";
    for (std::size_t i = 0U; i < projections.size(); ++i) {
        const row_size_projection_t& projection = projections[i];
        output << "    {\n";
        output << "      \"scenario\": ";
        write_json_string(output, projection.scenario);
        output << ",\n";
        output << "      \"cells\": " << projection.cells << ",\n";
        output << "      \"hyperlinks\": " << projection.hyperlinks << ",\n";
        output << "      \"dense_record_bytes\": "
               << projection.dense_record_bytes << ",\n";
        output << "      \"projected_run_length_record_bytes\": "
               << projection.projected_run_length_record_bytes << "\n";
        output << "    }" << (i + 1U == projections.size() ? "\n" : ",\n");
    }
    output << "  ],\n";
    output << "  \"benchmarks\": [\n";
    for (std::size_t i = 0U; i < measurements.size(); ++i) {
        const benchmark_measurement_t& measurement = measurements[i];
        output << "    {\n";
        output << "      \"name\": ";
        write_json_string(output, measurement.name);
        output << ",\n";
        output << "      \"operations\": " << measurement.operations << ",\n";
        output << "      \"elapsed_ns\": " << measurement.elapsed_ns << ",\n";
        output << "      \"ns_per_operation\": " << std::fixed << std::setprecision(2)
               << ns_per_operation(measurement) << ",\n";
        output.unsetf(std::ios::floatfield);
        output << "      \"records\": " << measurement.records << ",\n";
        output << "      \"bytes\": " << measurement.bytes << ",\n";
        output << "      \"checksum\": " << measurement.checksum << ",\n";
        output << "      \"ok\": " << (measurement.ok ? "true" : "false") << "\n";
        output << "    }" << (i + 1U == measurements.size() ? "\n" : ",\n");
    }
    output << "  ]\n";
    output << "}\n";
    return true;
}

void print_summary(
    const std::vector<benchmark_measurement_t>& measurements,
    const std::vector<row_size_projection_t>&   projections,
    const term::Terminal_history_ring&          limits_ring)
{
    std::cout << "Phase 8 benchmark summary\n";
    std::cout << "representation_decision=keep_dense_encoding\n";
    std::cout << "ring_capacity_bytes=" << limits_ring.capacity_bytes()
              << " max_record_bytes=" << limits_ring.max_record_bytes()
              << " max_payload_bytes=" << limits_ring.max_payload_bytes() << '\n';
    for (const row_size_projection_t& projection : projections) {
        std::cout << "projection " << projection.scenario
                  << " dense_record_bytes=" << projection.dense_record_bytes
                  << " projected_run_length_record_bytes="
                  << projection.projected_run_length_record_bytes << '\n';
    }
    for (const benchmark_measurement_t& measurement : measurements) {
        std::cout << "benchmark " << measurement.name
                  << " operations=" << measurement.operations
                  << " elapsed_ns=" << measurement.elapsed_ns
                  << " ns_per_operation=" << std::fixed << std::setprecision(2)
                  << ns_per_operation(measurement)
                  << " records=" << measurement.records
                  << " bytes=" << measurement.bytes
                  << " checksum=" << measurement.checksum
                  << " ok=" << (measurement.ok ? "true" : "false") << '\n';
        std::cout.unsetf(std::ios::floatfield);
    }
}

}

int main(int argc, char** argv)
{
    term::Terminal_history_ring limits_ring({
        k_phase8_retained_history_ring_capacity_bytes,
        0U,
    });
    if (!limits_ring.ok()) {
        report_failure("Phase 8 limits ring failed to initialize");
        return 1;
    }

    std::vector<benchmark_measurement_t> measurements;
    measurements.push_back(benchmark_append());
    measurements.push_back(benchmark_materialization());
    measurements.push_back(benchmark_traversal());
    measurements.push_back(benchmark_cache_rebuild());
    measurements.push_back(benchmark_resize_projection());
    measurements.push_back(benchmark_selection_extraction());
    measurements.push_back(benchmark_hyperlink_heavy_output());

    const std::vector<row_size_projection_t> projections = row_size_projections();
    print_summary(measurements, projections, limits_ring);

    bool ok = true;
    for (const benchmark_measurement_t& measurement : measurements) {
        ok = ok && measurement.ok;
    }

    if (argc > 1) {
        ok = write_report(argv[1], measurements, projections, limits_ring) && ok;
    }

    return ok ? 0 : 1;
}
