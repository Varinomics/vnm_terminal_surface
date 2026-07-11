#include "vnm_terminal/internal/terminal_history_row_record_codec.h"
#include "vnm_terminal/internal/terminal_screen_model.h"
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
constexpr std::uint32_t k_149_column_record_bytes_with_ring_overhead = 289U;
constexpr std::uint64_t k_min_149_column_single_style_retained_rows = 80000U;
constexpr std::uint64_t k_min_149_column_hyperlink_heavy_retained_rows = 9000U;
constexpr int k_capacity_gate_columns = 149;
constexpr int k_model_eviction_rows = 25000;

constexpr std::array<std::uint64_t, 5U> k_stress_levels = {
    20000U,
    50000U,
    100000U,
    200000U,
    205000U,
};

constexpr std::array<int, 8U> k_width_sweep_columns = {
    80,
    120,
    149,
    187,
    188,
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
    record.metadata.style_reference =
        term::Terminal_retained_row_style_reference::ROW_LOCAL_RESOLVED_STYLE;
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
    record.metadata.style_reference =
        term::Terminal_retained_row_style_reference::ROW_LOCAL_RESOLVED_STYLE;
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
    record.metadata.style_reference =
        term::Terminal_retained_row_style_reference::ROW_LOCAL_RESOLVED_STYLE;
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
    std::uint64_t row_sequence)
{
    return {
        29U,
        row_sequence,
    };
}

term::Terminal_history_row_record_append_result append_record(
    term::Terminal_history_ring&            ring,
    term::Terminal_history_row_record&      record,
    std::uint64_t                           row_sequence)
{
    set_row_provenance(record, row_sequence);
    return term::encode_terminal_history_row_record_to_ring(
        ring,
        record,
        make_identity(row_sequence));
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
        append_record(ring, record, 1U);
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
    const term::terminal_history_prefix_plain_ascii_retention_estimate_t width_bound =
        term::make_terminal_history_prefix_plain_ascii_retention_estimate(
            capacity_bytes,
            k_capacity_gate_columns);
    const std::uint64_t ascii_target_max_columns = width_bound.max_columns_at_target_rows;

    std::cout << "ascii_width_sweep capacity_bytes="
              << capacity_bytes
              << " target_rows=" << term::k_terminal_history_retention_target_rows
              << " target_max_columns=" << ascii_target_max_columns << '\n';

    for (int columns : k_width_sweep_columns) {
        const encoded_record_projection_t projection =
            encoded_record_projection(make_full_width_ascii_record(columns));
        const std::uint64_t retained_floor = retained_row_floor(projection);
        const bool reaches_target =
            retained_floor >= term::k_terminal_history_retention_target_rows;
        const bool expected_reaches_target =
            static_cast<std::uint64_t>(columns) <= ascii_target_max_columns;
        const term::terminal_history_prefix_plain_ascii_retention_estimate_t estimate =
            term::make_terminal_history_prefix_plain_ascii_retention_estimate(
                capacity_bytes,
                columns);

        std::cout << "ascii_width_sweep columns=" << columns
                  << " record_bytes_with_ring_overhead=" << projection.record_bytes
                  << " retained_row_floor=" << retained_floor
                  << " ascii_reaches_205000=" << (reaches_target ? "yes" : "no")
                  << '\n';

        ok &= check(projection.status == term::Terminal_history_row_record_codec_status::OK &&
                projection.record_bytes > 0U,
            "width sweep full-width ASCII row encodes for " + std::to_string(columns) +
            " columns");
        ok &= check(estimate.record_bytes == projection.record_bytes &&
                estimate.retained_rows == retained_floor,
            "codec estimate matches actual committed bytes for " +
            std::to_string(columns) + " columns");

        if (columns == k_capacity_gate_columns) {
            ok &= check(projection.record_bytes <=
                    k_149_column_record_bytes_with_ring_overhead,
                "149-column width sweep record bytes include the 40-byte ring overhead "
                "and stay <= 289 bytes");
            ok &= check(
                estimate.contract_version ==
                    term::k_terminal_history_retention_estimate_contract_version &&
                estimate.source_width_columns == 149U &&
                estimate.record_bytes == 289U &&
                estimate.retained_rows == 232210U &&
                estimate.target_rows == 205000U &&
                estimate.max_columns_at_target_rows == 187U,
                "149-column estimate pins the versioned retained-capacity contract tuple");
        }

        if (columns == 187) {
            ok &= check(reaches_target,
                "actual committed 187-column records retain at least 205000 rows");
        }
        if (columns == 188) {
            ok &= check(!reaches_target,
                "actual committed 188-column records retain fewer than 205000 rows");
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
        const bool reaches_ascii_target =
            retained_floor >= term::k_terminal_history_retention_target_rows;

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

QString snapshot_text_at_offset(
    term::Terminal_screen_model& model,
    int                          offset_from_tail)
{
    term::Terminal_render_snapshot_request request;
    request.sequence                  = 1U;
    request.viewport.active_buffer    = term::Terminal_buffer_id::PRIMARY;
    request.viewport.visible_rows     = 1;
    request.viewport.scrollback_rows  = model.scrollback_size();
    request.viewport.offset_from_tail = offset_from_tail;
    request.viewport.follow_tail      = false;

    const term::Terminal_render_snapshot snapshot = model.render_snapshot(request);
    QString text;
    for (const term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row == 0 && !cell.wide_continuation) {
            cell.text.append_to(text);
        }
    }
    return text;
}

bool run_model_sustained_cap_eviction_stress()
{
    bool ok = true;

    term::Terminal_screen_model_config config;
    config.grid_size        = {1, 1};
    config.scrollback_limit = static_cast<int>(
        term::k_terminal_history_retention_target_rows);
    term::Terminal_screen_model model(config);

    model.ingest(QByteArrayLiteral("x\r\n").repeated(config.scrollback_limit));
    ok &= check(model.scrollback_size() == config.scrollback_limit,
        "model sustained-output fixture reaches the configured retained-row cap");

    const std::optional<term::terminal_history_handle_t> oldest_before =
        model.retained_history_handle_at_logical_row(term::Terminal_buffer_id::PRIMARY, 0);
    const std::optional<term::terminal_history_handle_t> expected_oldest_after =
        model.retained_history_handle_at_logical_row(
            term::Terminal_buffer_id::PRIMARY,
            k_model_eviction_rows);
    if (!oldest_before.has_value() || !expected_oldest_after.has_value()) {
        return check(false, "model sustained-output fixture captures exact eviction handles");
    }

    model.ingest(QByteArrayLiteral("y\r\n").repeated(k_model_eviction_rows));

    const term::terminal_retained_history_diagnostics_t diagnostics =
        model.retained_history_diagnostics();
    const std::optional<term::terminal_history_handle_t> oldest_after =
        model.retained_history_handle_at_logical_row(term::Terminal_buffer_id::PRIMARY, 0);
    const std::optional<term::terminal_history_handle_t> newest_after =
        model.retained_history_handle_at_logical_row(
            term::Terminal_buffer_id::PRIMARY,
            config.scrollback_limit - 1);

    if (!oldest_after.has_value() || !newest_after.has_value()) {
        return check(false, "model sustained output exposes exact retained boundary handles");
    }

    ok &= check(diagnostics.retained_rows ==
            term::k_terminal_history_retention_target_rows &&
            diagnostics.payload_kind_generic_compact_rows == 0U &&
            diagnostics.payload_kind_prefix_plain_ascii_rows == diagnostics.retained_rows &&
            oldest_after->record_bytes == newest_after->record_bytes &&
            diagnostics.retained_record_bytes ==
                diagnostics.retained_rows * oldest_after->record_bytes &&
            diagnostics.average_retained_row_bytes == oldest_after->record_bytes,
        "model sustained output reports exact retained storage after 25000 replacements");
    ok &= check(
        diagnostics.prefix_plain_ascii_estimate.contract_version ==
            term::k_terminal_history_retention_estimate_contract_version &&
        diagnostics.prefix_plain_ascii_estimate.source_width_columns == 1U &&
        diagnostics.prefix_plain_ascii_estimate.record_bytes == 141U &&
        diagnostics.prefix_plain_ascii_estimate.retained_rows == 475949U &&
        diagnostics.prefix_plain_ascii_estimate.target_rows == 205000U &&
        diagnostics.prefix_plain_ascii_estimate.max_columns_at_target_rows == 187U,
        "model supplies the live ring budget and current width to the codec estimate");
    ok &= check(oldest_after == expected_oldest_after &&
            newest_after->row_sequence ==
                oldest_after->row_sequence +
                    term::k_terminal_history_retention_target_rows - 1U,
        "model sustained output retains the exact oldest handle and contiguous sequence span");
    ok &= check(
        model.retained_line_lookup(term::Terminal_buffer_id::PRIMARY, *oldest_before).
                resolution_status == term::Terminal_history_resolution_status::STALE_ROW_SEQUENCE,
        "model sustained output rejects the exact evicted oldest handle");
    ok &= check(snapshot_text_at_offset(model, config.scrollback_limit) == QStringLiteral("x") &&
            snapshot_text_at_offset(model, 1) == QStringLiteral("y"),
        "model sustained output preserves exact oldest and newest retained content");

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
    term::terminal_history_handle_t first_handle;
    std::uint32_t record_bytes = 0U;
    std::size_t checkpoint_index = 0U;

    for (std::uint64_t row_sequence = 1U;
         row_sequence <= k_stress_levels.back();
         ++row_sequence)
    {
        const term::Terminal_history_row_record_append_result append =
            append_record(ring, record, row_sequence);
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
                "overhead and stay <= 289 bytes");
        }

        if (checkpoint_index < k_stress_levels.size() &&
            row_sequence == k_stress_levels[checkpoint_index])
        {
            ok &= check(ring.oldest_live_byte_sequence() == first_handle.byte_sequence,
                "149-column capacity stress retains at least " +
                std::to_string(row_sequence) + " rows");

            std::cout << "stress_149 rows_appended=" << row_sequence
                      << " retained_rows=" << row_sequence
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
    ok &= run_model_sustained_cap_eviction_stress();
    return ok ? 0 : 1;
}
