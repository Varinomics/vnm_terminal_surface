#include "vnm_terminal/internal/terminal_history_row_record_codec.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QString>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

constexpr std::size_t k_retained_history_ring_capacity_bytes = 64U * 1024U * 1024U;
constexpr std::uint64_t k_target_retained_rows = 205000U;
constexpr std::uint32_t k_149_column_record_bytes_with_ring_overhead = 305U;
constexpr std::uint64_t k_min_149_column_single_style_retained_rows = 80000U;
constexpr std::uint64_t k_min_149_column_hyperlink_heavy_retained_rows = 9000U;
constexpr int k_capacity_gate_columns = 149;

constexpr std::array<std::uint64_t, 5U> k_stress_levels = {
    20000U,
    50000U,
    100000U,
    200000U,
    205000U,
};

constexpr std::array<int, 7U> k_width_sweep_columns = {
    80,
    120,
    149,
    171,
    200,
    250,
    4096,
};

struct encoded_record_projection_t
{
    term::Terminal_history_row_record_codec_status
                                   status = term::Terminal_history_row_record_codec_status::OK;
    term::Terminal_history_ring_status
                                   ring_status = term::Terminal_history_ring_status::OK;
    std::size_t                    capacity_bytes = 0U;
    std::uint32_t                  record_bytes = 0U;
};

term::Terminal_history_row_cell make_ascii_cell(
    char                        text,
    term::Terminal_style_id     style_id = term::k_default_terminal_style_id,
    term::Terminal_hyperlink_id hyperlink_id = term::k_no_terminal_hyperlink_id)
{
    term::Terminal_history_row_cell cell;
    cell.text = QString(QChar(QLatin1Char(text)));
    cell.display_width = 1;
    cell.occupied = true;
    cell.style_id = style_id;
    cell.hyperlink_id = hyperlink_id;
    return cell;
}

term::Terminal_text_style capacity_style()
{
    term::Terminal_text_style style = term::make_default_terminal_text_style();
    style.foreground = term::make_rgb_terminal_color_ref(0xff4080c0U);
    return style;
}

term::Terminal_history_row_record make_full_width_ascii_record(int columns)
{
    term::Terminal_history_row_record record;
    record.provenance.source =
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE;
    record.metadata.source_width = columns;
    record.metadata.style_lifetime =
        term::Terminal_retained_row_style_lifetime::SESSION_LIFETIME_STYLE_ID;
    record.metadata.wrap_state =
        term::Terminal_retained_row_wrap_state::HARD_BOUNDARY;

    record.cells.reserve(static_cast<std::size_t>(columns));
    for (int column = 0; column < columns; ++column) {
        record.cells.push_back(make_ascii_cell(static_cast<char>('a' + (column % 26))));
    }
    return record;
}

term::Terminal_history_row_record make_full_width_single_style_record(int columns)
{
    term::Terminal_history_row_record record;
    record.provenance.source =
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE;
    record.metadata.source_width = columns;
    record.metadata.style_lifetime =
        term::Terminal_retained_row_style_lifetime::SESSION_LIFETIME_STYLE_ID;
    record.metadata.wrap_state =
        term::Terminal_retained_row_wrap_state::HARD_BOUNDARY;
    record.style_table.push_back(capacity_style());

    record.cells.reserve(static_cast<std::size_t>(columns));
    for (int column = 0; column < columns; ++column) {
        record.cells.push_back(make_ascii_cell(
            static_cast<char>('a' + (column % 26)),
            1U));
    }
    return record;
}

term::Terminal_history_row_record make_full_width_hyperlink_heavy_record(int columns)
{
    term::Terminal_history_row_record record;
    record.provenance.source =
        term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE;
    record.metadata.source_width = columns;
    record.metadata.style_lifetime =
        term::Terminal_retained_row_style_lifetime::SESSION_LIFETIME_STYLE_ID;
    record.metadata.wrap_state =
        term::Terminal_retained_row_wrap_state::HARD_BOUNDARY;

    record.cells.reserve(static_cast<std::size_t>(columns));
    for (int column = 0; column < columns; ++column) {
        const term::Terminal_hyperlink_id row_ref =
            static_cast<term::Terminal_hyperlink_id>(column + 1);
        record.cells.push_back(make_ascii_cell(
            static_cast<char>('a' + (column % 26)),
            term::k_default_terminal_style_id,
            row_ref));
        record.hyperlink_identity_keys.emplace(
            row_ref,
            QByteArrayLiteral("uri:https://example.test/capacity/") +
                QByteArray::number(column));
    }
    return record;
}

void set_row_provenance(
    term::Terminal_history_row_record& record,
    std::uint64_t                      row_sequence)
{
    record.provenance.retained_line_id = row_sequence;
    record.provenance.content_generation = row_sequence + 1000000U;
    record.provenance.content_stamp_ms = static_cast<qint64>(row_sequence);
}

term::terminal_history_row_record_identity_t make_identity(
    std::uint64_t                   row_sequence,
    term::terminal_history_handle_t previous_handle)
{
    return {
        29U,
        row_sequence,
        previous_handle.byte_sequence,
        previous_handle.row_sequence,
    };
}

term::Terminal_history_row_record_append_result append_record(
    term::Terminal_history_ring&            ring,
    term::Terminal_history_row_record&      record,
    std::uint64_t                           row_sequence,
    term::terminal_history_handle_t         previous_handle)
{
    set_row_provenance(record, row_sequence);
    return term::encode_terminal_history_row_record_to_ring(
        ring,
        record,
        make_identity(row_sequence, previous_handle));
}

encoded_record_projection_t encoded_record_projection(
    term::Terminal_history_row_record record)
{
    encoded_record_projection_t projection;
    term::Terminal_history_ring ring({
        k_retained_history_ring_capacity_bytes,
        0U,
    });
    projection.ring_status = ring.status();
    projection.capacity_bytes = ring.capacity_bytes();
    if (!ring.ok()) {
        return projection;
    }

    const term::Terminal_history_row_record_append_result append =
        append_record(ring, record, 1U, {});
    projection.status = append.status;
    if (append.status == term::Terminal_history_row_record_codec_status::OK) {
        projection.record_bytes = append.commit.record_bytes;
    }
    return projection;
}

std::uint64_t retained_row_floor(
    std::size_t   capacity_bytes,
    std::uint32_t record_bytes)
{
    return record_bytes == 0U
        ? 0U
        : static_cast<std::uint64_t>(capacity_bytes / record_bytes);
}

std::uint64_t retained_row_floor(const encoded_record_projection_t& projection)
{
    return retained_row_floor(projection.capacity_bytes, projection.record_bytes);
}

std::uint64_t max_record_bytes_for_target(std::size_t capacity_bytes)
{
    return static_cast<std::uint64_t>(capacity_bytes) / k_target_retained_rows;
}

std::uint64_t max_full_width_ascii_columns_for_target(std::size_t capacity_bytes)
{
    const encoded_record_projection_t one_column =
        encoded_record_projection(make_full_width_ascii_record(1));
    const encoded_record_projection_t two_columns =
        encoded_record_projection(make_full_width_ascii_record(2));
    if (one_column.status != term::Terminal_history_row_record_codec_status::OK ||
        two_columns.status != term::Terminal_history_row_record_codec_status::OK ||
        two_columns.record_bytes <= one_column.record_bytes)
    {
        return 0U;
    }

    const std::uint64_t bytes_per_column =
        static_cast<std::uint64_t>(two_columns.record_bytes - one_column.record_bytes);
    const std::uint64_t fixed_record_bytes =
        static_cast<std::uint64_t>(one_column.record_bytes) - bytes_per_column;
    const std::uint64_t max_record_bytes =
        max_record_bytes_for_target(capacity_bytes);
    if (max_record_bytes <= fixed_record_bytes) {
        return 0U;
    }

    return (max_record_bytes - fixed_record_bytes) / bytes_per_column;
}

bool run_width_sweep()
{
    bool ok = true;

    term::Terminal_history_ring limits_ring({
        k_retained_history_ring_capacity_bytes,
        0U,
    });
    ok &= check(limits_ring.status() == term::Terminal_history_ring_status::OK,
        "ASCII width sweep limits ring initializes");
    if (!limits_ring.ok()) {
        return ok;
    }

    const std::size_t capacity_bytes = limits_ring.capacity_bytes();
    const std::uint64_t ascii_target_max_columns =
        max_full_width_ascii_columns_for_target(capacity_bytes);

    std::cout << "ascii_width_sweep capacity_bytes="
              << capacity_bytes
              << " target_rows=" << k_target_retained_rows
              << " target_max_columns=" << ascii_target_max_columns << '\n';

    for (int columns : k_width_sweep_columns) {
        const encoded_record_projection_t projection =
            encoded_record_projection(make_full_width_ascii_record(columns));
        const std::uint64_t retained_floor = retained_row_floor(projection);
        const bool reaches_target = retained_floor >= k_target_retained_rows;
        const bool expected_reaches_target =
            static_cast<std::uint64_t>(columns) <= ascii_target_max_columns;

        std::cout << "ascii_width_sweep columns=" << columns
                  << " record_bytes_with_ring_overhead=" << projection.record_bytes
                  << " retained_row_floor=" << retained_floor
                  << " ascii_reaches_205000=" << (reaches_target ? "yes" : "no")
                  << '\n';

        ok &= check(projection.status == term::Terminal_history_row_record_codec_status::OK &&
                projection.record_bytes > 0U,
            "width sweep full-width ASCII row encodes for " + std::to_string(columns) +
            " columns");

        if (columns == k_capacity_gate_columns) {
            ok &= check(projection.record_bytes <=
                    k_149_column_record_bytes_with_ring_overhead,
                "149-column width sweep record bytes include the 40-byte ring overhead "
                "and stay <= 305 bytes");
        }

        ok &= check(reaches_target == expected_reaches_target,
            "width sweep target reach follows the derived full-width ASCII boundary for " +
            std::to_string(columns) + " columns");
    }

    return ok;
}

bool run_styled_and_hyperlink_capacity_floor_report()
{
    bool ok = true;

    const auto check_case = [&](const std::string& scenario,
                                term::Terminal_history_row_record record,
                                std::uint64_t min_retained_floor) {
        const encoded_record_projection_t projection =
            encoded_record_projection(record);
        const std::uint64_t retained_floor = retained_row_floor(projection);
        const bool reaches_ascii_target = retained_floor >= k_target_retained_rows;

        std::cout << "styled_hyperlink_ascii_floor scenario=" << scenario
                  << " record_bytes_with_ring_overhead=" << projection.record_bytes
                  << " retained_row_floor=" << retained_floor
                  << " reaches_ascii_205000=" << (reaches_ascii_target ? "yes" : "no")
                  << '\n';

        ok &= check(projection.status == term::Terminal_history_row_record_codec_status::OK &&
                projection.record_bytes > 0U,
            scenario + " retained floor fixture encodes");
        ok &= check(retained_floor >= min_retained_floor,
            scenario + " retained floor stays above its explicit capacity floor");
        ok &= check(!reaches_ascii_target,
            scenario + " retained floor is reported separately from the ASCII 205000 gate");
    };

    check_case(
        "single_style_149_columns",
        make_full_width_single_style_record(k_capacity_gate_columns),
        k_min_149_column_single_style_retained_rows);
    check_case(
        "hyperlink_heavy_149_columns",
        make_full_width_hyperlink_heavy_record(k_capacity_gate_columns),
        k_min_149_column_hyperlink_heavy_retained_rows);

    return ok;
}

bool run_149_column_stress()
{
    bool ok = true;

    term::Terminal_history_ring ring({
        k_retained_history_ring_capacity_bytes,
        0U,
    });
    ok &= check(ring.status() == term::Terminal_history_ring_status::OK,
        "149-column retained-history capacity stress ring initializes");
    if (!ring.ok()) {
        return ok;
    }

    term::Terminal_history_row_record record =
        make_full_width_ascii_record(k_capacity_gate_columns);
    term::terminal_history_handle_t previous_handle;
    term::terminal_history_handle_t first_handle;
    std::uint32_t record_bytes = 0U;
    std::size_t checkpoint_index = 0U;

    for (std::uint64_t row_sequence = 1U;
         row_sequence <= k_stress_levels.back();
         ++row_sequence)
    {
        const term::Terminal_history_row_record_append_result append =
            append_record(ring, record, row_sequence, previous_handle);
        if (append.status != term::Terminal_history_row_record_codec_status::OK) {
            std::cerr << "FAIL: 149-column capacity stress append failed at row "
                      << row_sequence << " status="
                      << static_cast<int>(append.status) << '\n';
            return false;
        }

        if (row_sequence == 1U) {
            first_handle = append.history_handle;
            record_bytes = append.commit.record_bytes;
            ok &= check(record_bytes <= k_149_column_record_bytes_with_ring_overhead,
                "149-column capacity stress record bytes include the 40-byte ring "
                "overhead and stay <= 305 bytes");
        }

        previous_handle = append.history_handle;

        if (checkpoint_index < k_stress_levels.size() &&
            row_sequence == k_stress_levels[checkpoint_index])
        {
            const term::Terminal_history_ring_record_index_result live =
                ring.live_record_descriptors();
            ok &= check(live.status == term::Terminal_history_ring_status::OK,
                "149-column capacity stress live-record index rebuilds at " +
                std::to_string(row_sequence) + " rows");
            ok &= check(live.records.size() >= row_sequence,
                "149-column capacity stress retains at least " +
                std::to_string(row_sequence) + " rows");

            std::cout << "stress_149 rows_appended=" << row_sequence
                      << " retained_rows=" << live.records.size()
                      << " record_bytes_with_ring_overhead=" << record_bytes
                      << " retained_row_floor="
                      << retained_row_floor(ring.capacity_bytes(), record_bytes)
                      << '\n';
            ++checkpoint_index;
        }
    }

    ok &= check(ring.read_record(first_handle.byte_sequence).status() ==
            term::Terminal_history_ring_status::OK,
        "149-column capacity stress keeps the first row live through 205000 rows");

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= run_width_sweep();
    ok &= run_styled_and_hyperlink_capacity_floor_report();
    ok &= run_149_column_stress();
    return ok ? 0 : 1;
}
