#include "vnm_terminal/internal/terminal_history_row_record_codec.h"
#include "helpers/test_check.h"

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

term::Terminal_history_row_cell make_ascii_cell(char text)
{
    term::Terminal_history_row_cell cell;
    cell.text = QString(QChar(QLatin1Char(text)));
    cell.display_width = 1;
    cell.occupied = true;
    return cell;
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

std::uint32_t encoded_record_bytes(term::Terminal_history_row_record record)
{
    term::Terminal_history_ring ring({
        k_retained_history_ring_capacity_bytes,
        0U,
    });
    if (!ring.ok()) {
        return 0U;
    }

    const term::Terminal_history_row_record_append_result append =
        append_record(ring, record, 1U, {});
    return append.status == term::Terminal_history_row_record_codec_status::OK
        ? append.commit.record_bytes
        : 0U;
}

std::uint64_t retained_row_floor(std::uint32_t record_bytes)
{
    return record_bytes == 0U
        ? 0U
        : k_retained_history_ring_capacity_bytes / record_bytes;
}

bool run_width_sweep()
{
    bool ok = true;

    std::cout << "width_sweep capacity_bytes="
              << k_retained_history_ring_capacity_bytes
              << " target_rows=" << k_target_retained_rows << '\n';

    for (int columns : k_width_sweep_columns) {
        const std::uint32_t record_bytes =
            encoded_record_bytes(make_full_width_ascii_record(columns));
        const std::uint64_t retained_floor = retained_row_floor(record_bytes);
        const bool reaches_target = retained_floor >= k_target_retained_rows;

        std::cout << "width_sweep columns=" << columns
                  << " record_bytes_with_ring_overhead=" << record_bytes
                  << " retained_row_floor=" << retained_floor
                  << " reaches_205000=" << (reaches_target ? "yes" : "no")
                  << '\n';

        ok &= check(record_bytes > 0U,
            "width sweep full-width ASCII row encodes for " + std::to_string(columns) +
            " columns");

        if (columns == k_capacity_gate_columns) {
            ok &= check(record_bytes <= k_149_column_record_bytes_with_ring_overhead,
                "149-column width sweep record bytes include the 40-byte ring overhead "
                "and stay <= 305 bytes");
        }

        if (columns <= 171) {
            ok &= check(reaches_target,
                "width sweep reports at least 205000 retained rows for " +
                std::to_string(columns) + " columns");
        }
        else {
            ok &= check(!reaches_target,
                "width sweep does not claim 205000 retained rows for " +
                std::to_string(columns) + " columns");
        }
    }

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
                      << retained_row_floor(record_bytes)
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
    ok &= run_149_column_stress();
    return ok ? 0 : 1;
}
