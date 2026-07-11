#include "vnm_terminal/internal/terminal_history_row_record_codec.h"
#include "vnm_terminal/internal/terminal_screen_model.h"

#include <QByteArray>
#include <QString>
#include <algorithm>
#include <chrono>
#include <cstddef>
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

constexpr std::size_t k_retained_history_ring_capacity_bytes =
    64U * 1024U * 1024U;
constexpr std::size_t k_row_record_header_flags_offset = 20U;
constexpr std::uint32_t k_payload_kind_mask = 0x0fU;
constexpr std::uint32_t k_payload_kind_generic_compact = 0U;
constexpr std::uint32_t k_payload_kind_prefix_plain_ascii = 1U;
constexpr int k_columns = 120;
constexpr int k_row_count = 2000;

enum class Retained_history_row_pattern
{
    BLANK,
    ASCII_SEQUENCE,
    REPEATED_ASCII,
    HYPERLINK_HEAVY,
};

struct benchmark_payload_t
{
    bool                         ok = true;
    std::uint64_t                records = 0U;
    std::uint64_t                bytes = 0U;
    std::uint64_t                cells = 0U;
    std::uint64_t                hyperlinks = 0U;
    std::uint64_t                checksum = 0U;
};

struct benchmark_measurement_t
{
    std::string                  name;
    std::uint64_t                operations = 0U;
    std::uint64_t                elapsed_ns = 0U;
    std::uint64_t                records = 0U;
    std::uint64_t                bytes = 0U;
    std::uint64_t                cells = 0U;
    std::uint64_t                hyperlinks = 0U;
    std::uint64_t                checksum = 0U;
    bool                         ok = true;
};

struct row_size_projection_t
{
    std::string                  scenario;
    std::uint64_t                cells = 0U;
    std::uint64_t                hyperlinks = 0U;
    std::uint64_t                encoded_record_bytes = 0U;
    std::uint32_t                payload_kind = k_payload_kind_generic_compact;
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
    measurement.records    = payload.records;
    measurement.bytes      = payload.bytes;
    measurement.cells      = payload.cells;
    measurement.hyperlinks = payload.hyperlinks;
    measurement.checksum   = payload.checksum;
    measurement.ok         = payload.ok;
    return measurement;
}

std::uint64_t checksum_mix_byte(
    std::uint64_t checksum,
    std::uint8_t  byte)
{
    constexpr std::uint64_t k_fnv_prime = 1099511628211ULL;
    return (checksum ^ byte) * k_fnv_prime;
}

std::uint64_t checksum_mix_u64(
    std::uint64_t checksum,
    std::uint64_t value)
{
    for (int byte_index = 0; byte_index < 8; ++byte_index) {
        checksum = checksum_mix_byte(
            checksum,
            static_cast<std::uint8_t>((value >> (byte_index * 8)) & 0xffU));
    }
    return checksum;
}

std::uint64_t checksum_mix_text(
    std::uint64_t  checksum,
    const QString& text)
{
    checksum = checksum_mix_u64(
        checksum,
        static_cast<std::uint64_t>(text.size()));
    for (qsizetype index = 0; index < text.size(); ++index) {
        checksum = checksum_mix_u64(
            checksum,
            static_cast<std::uint64_t>(text.at(index).unicode()));
    }
    return checksum;
}

std::uint64_t record_content_checksum(
    const term::Terminal_history_row_record& record)
{
    constexpr std::uint64_t k_fnv_offset = 1469598103934665603ULL;
    std::uint64_t checksum = checksum_mix_u64(
        k_fnv_offset,
        static_cast<std::uint64_t>(record.cells.size()));
    for (const term::Terminal_history_row_cell& cell : record.cells) {
        checksum = checksum_mix_text(checksum, cell.text);
        checksum = checksum_mix_u64(checksum, static_cast<std::uint64_t>(
            std::max(cell.display_width, 0)));
        checksum = checksum_mix_u64(checksum, cell.wide_continuation ? 1U : 0U);
        checksum = checksum_mix_u64(checksum, cell.occupied ? 1U : 0U);
        checksum = checksum_mix_u64(checksum, cell.style_id);
        checksum = checksum_mix_u64(checksum, cell.hyperlink_id);
    }
    return checksum;
}

term::Terminal_history_row_cell make_cell(
    QString                 text,
    term::Terminal_style_id style_id = term::k_default_terminal_style_id,
    term::Terminal_hyperlink_id hyperlink_id = term::k_no_terminal_hyperlink_id)
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
    Retained_history_row_pattern  pattern)
{
    term::Terminal_history_row_record record;
    record.provenance.retained_line_id   = row_sequence;
    record.provenance.content_generation = row_sequence + 1000U;
    record.provenance.source =
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE;
    record.metadata.source_width = columns;
    record.metadata.style_reference =
        term::Terminal_retained_row_style_reference::ROW_LOCAL_RESOLVED_STYLE;
    record.metadata.wrap_state =
        term::Terminal_retained_row_wrap_state::HARD_BOUNDARY;

    if (pattern == Retained_history_row_pattern::BLANK) {
        record.cells.resize(static_cast<std::size_t>(columns));
        return record;
    }

    record.cells.reserve(static_cast<std::size_t>(columns));
    for (int column = 0; column < columns; ++column) {
        if (pattern == Retained_history_row_pattern::REPEATED_ASCII) {
            record.cells.push_back(make_cell(QStringLiteral("x")));
            continue;
        }

        if (pattern == Retained_history_row_pattern::HYPERLINK_HEAVY) {
            const term::Terminal_hyperlink_id row_ref =
                static_cast<term::Terminal_hyperlink_id>(column + 1);
            record.cells.push_back(make_cell(
                QStringLiteral("L"),
                term::k_default_terminal_style_id,
                row_ref));
            record.hyperlink_identity_keys.emplace(
                row_ref,
                QByteArrayLiteral("uri:https://example.test/retained-history/") +
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
    std::uint64_t row_sequence)
{
    return {
        11U,
        row_sequence,
    };
}

term::Terminal_history_row_record_append_result append_record(
    term::Terminal_history_ring&    ring,
    std::uint64_t                   row_sequence,
    Retained_history_row_pattern              pattern)
{
    return term::encode_terminal_history_row_record_to_ring(
        ring,
        make_record(row_sequence, k_columns, pattern),
        make_identity(row_sequence));
}

bool append_fixture_rows(
    term::Terminal_history_ring&                    ring,
    std::vector<term::terminal_history_handle_t>&    handles,
    int                                             row_count,
    Retained_history_row_pattern                              pattern,
    std::uint64_t&                                  bytes)
{
    handles.clear();
    handles.reserve(static_cast<std::size_t>(row_count));
    bytes = 0U;

    for (int row = 0; row < row_count; ++row) {
        const std::uint64_t row_sequence = static_cast<std::uint64_t>(row + 1);
        const term::Terminal_history_row_record_append_result append =
            append_record(ring, row_sequence, pattern);
        if (append.status != term::Terminal_history_row_record_codec_status::OK) {
            return report_failure("fixture row append failed");
        }

        handles.push_back(append.history_handle);
        bytes += append.commit.record_bytes;
    }

    return true;
}

std::uint32_t row_record_payload_kind(std::span<const std::byte> payload)
{
    std::uint32_t flags = 0U;
    for (std::size_t i = 0U; i < 4U; ++i) {
        flags |= static_cast<std::uint32_t>(
            payload[k_row_record_header_flags_offset + i]) << (i * 8U);
    }
    return flags & k_payload_kind_mask;
}

const char* payload_kind_name(std::uint32_t payload_kind)
{
    switch (payload_kind) {
        case k_payload_kind_generic_compact:
            return "generic_compact";
        case k_payload_kind_prefix_plain_ascii:
            return "prefix_plain_ascii";
        default:
            return "reserved";
    }
}

row_size_projection_t encoded_record_projection(
    std::string                              scenario,
    const term::Terminal_history_row_record& record)
{
    term::Terminal_history_ring ring({
        k_retained_history_ring_capacity_bytes,
        0U,
    });
    if (!ring.ok()) {
        return {std::move(scenario)};
    }

    const term::Terminal_history_row_record_append_result append =
        term::encode_terminal_history_row_record_to_ring(
            ring,
            record,
            make_identity(record.provenance.retained_line_id));
    if (append.status != term::Terminal_history_row_record_codec_status::OK) {
        return {std::move(scenario)};
    }

    const term::Terminal_history_ring_read_scope read =
        ring.read_record(append.commit.byte_sequence);
    if (read.status() != term::Terminal_history_ring_status::OK) {
        return {std::move(scenario)};
    }

    return {
        std::move(scenario),
        static_cast<std::uint64_t>(record.cells.size()),
        static_cast<std::uint64_t>(record.hyperlink_identity_keys.size()),
        append.commit.record_bytes,
        row_record_payload_kind(read.payload()),
    };
}

std::vector<row_size_projection_t> row_size_projections()
{
    const std::vector<std::pair<std::string, Retained_history_row_pattern>> scenarios = {
        {"blank_120_columns",          Retained_history_row_pattern::BLANK},
        {"ascii_120_columns",          Retained_history_row_pattern::ASCII_SEQUENCE},
        {"repeated_ascii_120_columns", Retained_history_row_pattern::REPEATED_ASCII},
        {"hyperlink_120_columns",      Retained_history_row_pattern::HYPERLINK_HEAVY},
    };

    std::vector<row_size_projection_t> projections;
    projections.reserve(scenarios.size());
    for (const auto& scenario : scenarios) {
        const term::Terminal_history_row_record record =
            make_record(1U, k_columns, scenario.second);
        projections.push_back(encoded_record_projection(scenario.first, record));
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
        QByteArray text = QByteArrayLiteral("retained-row-") + QByteArray::number(row);
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
        const QByteArray row_id = QByteArrayLiteral("retained-") + QByteArray::number(row);
        stream += QByteArrayLiteral("\x1b]8;id=");
        stream += row_id;
        stream += QByteArrayLiteral(";https://example.test/retained-history/");
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
        "append_ascii_rows",
        k_row_count,
        [] {
            benchmark_payload_t payload;
            term::Terminal_history_ring ring({
                k_retained_history_ring_capacity_bytes,
                0U,
            });
            if (!ring.ok()) {
                payload.ok = report_failure("append benchmark ring failed");
                return payload;
            }

            for (int row = 0; row < k_row_count; ++row) {
                const std::uint64_t row_sequence = static_cast<std::uint64_t>(row + 1);
                const term::Terminal_history_row_record_append_result append =
                    append_record(
                        ring,
                        row_sequence,
                        Retained_history_row_pattern::ASCII_SEQUENCE);
                if (append.status != term::Terminal_history_row_record_codec_status::OK) {
                    payload.ok = report_failure("append benchmark append failed");
                    return payload;
                }

                payload.bytes += append.commit.record_bytes;
                payload.checksum += append.history_handle.row_sequence;
            }

            payload.records = k_row_count;
            return payload;
        });
}

benchmark_measurement_t benchmark_materialization()
{
    term::Terminal_history_ring ring({
        k_retained_history_ring_capacity_bytes,
        0U,
    });
    std::vector<term::terminal_history_handle_t> handles;
    std::uint64_t fixture_bytes = 0U;
    const bool fixture_ok = ring.ok() &&
        append_fixture_rows(
            ring,
            handles,
            k_row_count,
            Retained_history_row_pattern::ASCII_SEQUENCE,
            fixture_bytes);

    return measure_case(
        "materialization_ascii_rows",
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
                        "materialization benchmark decode failed");
                    return payload;
                }

                payload.cells += static_cast<std::uint64_t>(decoded.record.cells.size());
                payload.checksum = checksum_mix_u64(
                    payload.checksum,
                    record_content_checksum(decoded.record));
                payload.bytes += handle.record_bytes;
            }

            payload.records = static_cast<std::uint64_t>(handles.size());
            return payload;
        });
}

benchmark_measurement_t benchmark_resize_projection()
{
    constexpr int resize_rounds = 16;
    const std::vector<int> widths = {80, 120, 160, 96};

    term::Terminal_screen_model model({
        term::terminal_grid_size_t{24, k_columns},
        768,
        8,
    });
    const term::Terminal_screen_model_result ingest =
        model.ingest(plain_output_stream(640, k_columns));
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
                        payload.ok = report_failure("resize projection did not reflow");
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
                            "resize projection snapshot failed validation");
                        return payload;
                    }

                    ++payload.records;
                    payload.checksum += snapshot.cells.size();
                    payload.cells += static_cast<std::uint64_t>(snapshot.cells.size());
                }
            }

            return payload;
        });
}

benchmark_measurement_t benchmark_selection_extraction()
{
    constexpr int selection_repetitions = 200;

    term::Terminal_screen_model model({
        term::terminal_grid_size_t{24, k_columns},
        768,
        8,
    });
    const term::Terminal_screen_model_result ingest =
        model.ingest(plain_output_stream(640, k_columns));

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
                    payload.ok = report_failure("selection extraction failed");
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
                payload.ok = report_failure("hyperlink-heavy output ingest failed");
                return payload;
            }

            const int offset = std::min(model.scrollback_size(), model.grid_size().rows);
            const term::Terminal_render_snapshot snapshot =
                model.render_snapshot(request_for_model(model, 800U, offset));
            if (term::validate_render_snapshot(snapshot).status !=
                term::Terminal_render_snapshot_status::OK)
            {
                payload.ok = report_failure(
                    "hyperlink-heavy output snapshot failed validation");
                return payload;
            }

            payload.records = hyperlink_rows;
            payload.cells = static_cast<std::uint64_t>(snapshot.cells.size());
            payload.hyperlinks = static_cast<std::uint64_t>(snapshot.hyperlinks.size());
            payload.checksum = checksum_mix_u64(
                checksum_mix_u64(0U, payload.cells),
                payload.hyperlinks);
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
        std::cerr << "FAIL: unable to write benchmark report: " << path << '\n';
        return false;
    }

    output << "{\n";
    output << "  \"schema\": \"vnm_terminal_retained_history_benchmark_report\",\n";
    output << "  \"schema_version\": 1,\n";
    output << "  \"retained_row_codec\": \"compact_row_record_with_prefix_plain_ascii\",\n";
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
        output << "      \"encoded_record_bytes\": "
               << projection.encoded_record_bytes << ",\n";
        output << "      \"payload_kind\": ";
        write_json_string(output, payload_kind_name(projection.payload_kind));
        output << "\n";
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
        output << "      \"cells\": " << measurement.cells << ",\n";
        output << "      \"hyperlinks\": " << measurement.hyperlinks << ",\n";
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
    std::cout << "Retained history benchmark summary\n";
    std::cout << "retained_row_codec=compact_row_record_with_prefix_plain_ascii\n";
    std::cout << "ring_capacity_bytes=" << limits_ring.capacity_bytes()
              << " max_record_bytes=" << limits_ring.max_record_bytes()
              << " max_payload_bytes=" << limits_ring.max_payload_bytes() << '\n';
    for (const row_size_projection_t& projection : projections) {
        std::cout << "projection " << projection.scenario
                  << " encoded_record_bytes=" << projection.encoded_record_bytes
                  << " payload_kind=" << payload_kind_name(projection.payload_kind)
                  << '\n';
    }
    for (const benchmark_measurement_t& measurement : measurements) {
        std::cout << "benchmark " << measurement.name
                  << " operations=" << measurement.operations
                  << " elapsed_ns=" << measurement.elapsed_ns
                  << " ns_per_operation=" << std::fixed << std::setprecision(2)
                  << ns_per_operation(measurement)
                  << " records=" << measurement.records
                  << " bytes=" << measurement.bytes
                  << " cells=" << measurement.cells
                  << " hyperlinks=" << measurement.hyperlinks
                  << " checksum=" << measurement.checksum
                  << " ok=" << (measurement.ok ? "true" : "false") << '\n';
        std::cout.unsetf(std::ios::floatfield);
    }
}

}

int main(int argc, char** argv)
{
    term::Terminal_history_ring limits_ring({
        k_retained_history_ring_capacity_bytes,
        0U,
    });
    if (!limits_ring.ok()) {
        report_failure("limits ring failed to initialize");
        return 1;
    }

    std::vector<benchmark_measurement_t> measurements;
    measurements.push_back(benchmark_append());
    measurements.push_back(benchmark_materialization());
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
