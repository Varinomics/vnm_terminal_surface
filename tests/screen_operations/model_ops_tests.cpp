#include "vnm_terminal/internal/terminal_screen_model.h"
#include "vnm_terminal/internal/terminal_repaint_recovery.h"
#include "helpers/primary_backing_observation.h"
#include "helpers/primary_backing_test_config.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QString>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;
using vnm_terminal::test_helpers::Primary_backing_observation_classification;
using vnm_terminal::test_helpers::Primary_backing_boundary;
using vnm_terminal::test_helpers::Scrollback_delta_observer;
using vnm_terminal::test_helpers::Scrollback_delta_operation_annotation;
using vnm_terminal::test_helpers::recovery_disabled_primary_backing_screen_model_config;
using vnm_terminal::test_helpers::scrollback_rows_delta;

[[noreturn]] void fail_required_value(const char* message)
{
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

QByteArray bytes_from_hex(const char* hex)
{
    return QByteArray::fromHex(QByteArray(hex));
}

term::Terminal_screen_model make_model(
    int    rows = 4,
    int    columns = 8,
    int    scrollback_limit = 8)
{
    return term::Terminal_screen_model({
        term::terminal_grid_size_t{rows, columns},
        scrollback_limit,
        4,
    });
}

term::Terminal_screen_model make_recovery_disabled_primary_backing_model(
    int    rows,
    int    columns,
    int    scrollback_limit)
{
    term::Terminal_screen_model_config config;
    config.grid_size        = term::terminal_grid_size_t{rows, columns};
    config.scrollback_limit = scrollback_limit;
    config.tab_width        = 4;
    return term::Terminal_screen_model(
        recovery_disabled_primary_backing_screen_model_config(config));
}

term::Terminal_screen_model make_recovery_enabled_primary_repaint_model(
    int    rows,
    int    columns,
    int    scrollback_limit)
{
    term::Terminal_screen_model_config config;
    config.grid_size                                = term::terminal_grid_size_t{rows, columns};
    config.scrollback_limit                         = scrollback_limit;
    config.tab_width                                = 4;
    config.recover_scrollback_from_primary_repaints = true;
    return term::Terminal_screen_model(config);
}

QByteArray visible_row_write_stream(
    std::initializer_list<QByteArray>  rows,
    bool                               cursor_hidden)
{
    QByteArray stream;
    if (cursor_hidden) {
        stream += QByteArrayLiteral("\x1b[?25l");
    }

    int row_number = 1;
    for (const QByteArray& row : rows) {
        stream += QByteArrayLiteral("\x1b[");
        stream += QByteArray::number(row_number);
        stream += QByteArrayLiteral(";1H");
        stream += row;
        stream += QByteArrayLiteral("\x1b[K");
        ++row_number;
    }

    if (cursor_hidden) {
        stream += QByteArrayLiteral("\x1b[?25h");
    }
    return stream;
}

QByteArray styled_hyperlink_row_bytes(
    const QByteArray& text,
    const QByteArray& uri)
{
    QByteArray row;
    row += QByteArrayLiteral("\x1b[43m");
    row += QByteArrayLiteral("\x1b]8;id=phase3;");
    row += uri;
    row += QByteArrayLiteral("\x1b\\");
    row += text;
    row += QByteArrayLiteral("\x1b]8;;\x1b\\");
    row += QByteArrayLiteral("\x1b[0m");
    return row;
}

int diagnostic_count(const term::Terminal_screen_model_result& result)
{
    int count = 0;
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) == term::Parser_action_kind::DIAGNOSTIC) {
            ++count;
        }
    }
    return count;
}

const term::Parser_payload_diagnostic& first_diagnostic(
    const term::Terminal_screen_model_result& result)
{
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) == term::Parser_action_kind::DIAGNOSTIC) {
            return std::get<term::Parser_payload_diagnostic>(action.payload);
        }
    }

    fail_required_value("expected diagnostic action");
}

bool dirty_rows_equal(
    const term::Terminal_screen_model_result&  result,
    std::initializer_list<int>                 rows,
    const char*                                label)
{
    return check(result.dirty_rows == std::vector<int>(rows), label);
}

bool dirty_rows_equal(
    const term::Terminal_screen_model_result&  result,
    const std::vector<int>&                    rows,
    const char*                                label)
{
    return check(result.dirty_rows == rows, label);
}

bool primary_backing_delta_matches(
    const term::terminal_backing_delta_t& delta,
    term::Terminal_backing_delta_kind     kind,
    int                                   scrollback_rows_before,
    int                                   scrollback_rows_after,
    int                                   appended_scrollback_rows,
    int                                   evicted_scrollback_rows,
    int                                   discarded_scrollback_rows = 0)
{
    return delta.kind                     == kind                       &&
        delta.buffer_id                   == term::Terminal_buffer_id::PRIMARY &&
        delta.scrollback_rows_before      == scrollback_rows_before     &&
        delta.scrollback_rows_after       == scrollback_rows_after      &&
        delta.appended_scrollback_rows    == appended_scrollback_rows   &&
        delta.evicted_scrollback_rows     == evicted_scrollback_rows    &&
        delta.discarded_scrollback_rows   == discarded_scrollback_rows;
}

bool single_primary_backing_delta_equal(
    const std::vector<term::terminal_backing_delta_t>& deltas,
    term::Terminal_backing_delta_kind                  kind,
    int                                                scrollback_rows_before,
    int                                                scrollback_rows_after,
    int                                                appended_scrollback_rows,
    int                                                evicted_scrollback_rows,
    const char*                                        label)
{
    return check(deltas.size() == 1U &&
            primary_backing_delta_matches(
                deltas.front(),
                kind,
                scrollback_rows_before,
                scrollback_rows_after,
                appended_scrollback_rows,
                evicted_scrollback_rows),
        label);
}

bool grid_size_equal(
    term::terminal_grid_size_t left,
    term::terminal_grid_size_t right)
{
    return left.rows == right.rows && left.columns == right.columns;
}

bool single_active_grid_backing_delta_equal(
    const std::vector<term::terminal_backing_delta_t>& deltas,
    term::Terminal_backing_delta_kind                  kind,
    term::terminal_grid_size_t                         grid_size_before,
    term::terminal_grid_size_t                         grid_size_after,
    const char*                                        label)
{
    return check(deltas.size() == 1U                        &&
            deltas.front().kind == kind                      &&
            deltas.front().buffer_id == term::Terminal_buffer_id::PRIMARY &&
            deltas.front().active_buffer_before ==
                term::Terminal_buffer_id::PRIMARY           &&
            deltas.front().active_buffer_after ==
                term::Terminal_buffer_id::PRIMARY           &&
            grid_size_equal(deltas.front().grid_size_before, grid_size_before) &&
            grid_size_equal(deltas.front().grid_size_after,  grid_size_after),
        label);
}

bool active_grid_backing_delta_matches(
    const term::terminal_backing_delta_t& delta,
    term::Terminal_backing_delta_kind     kind,
    term::Terminal_buffer_id              active_buffer,
    term::terminal_grid_size_t            grid_size_before,
    term::terminal_grid_size_t            grid_size_after)
{
    return delta.kind                 == kind          &&
        delta.buffer_id               == active_buffer &&
        delta.active_buffer_before    == active_buffer &&
        delta.active_buffer_after     == active_buffer &&
        grid_size_equal(delta.grid_size_before, grid_size_before) &&
        grid_size_equal(delta.grid_size_after,  grid_size_after);
}

bool single_mode_transition_backing_delta_equal(
    const std::vector<term::terminal_backing_delta_t>& deltas,
    term::Terminal_buffer_id                           active_buffer_before,
    term::Terminal_buffer_id                           active_buffer_after,
    const char*                                        label)
{
    return check(deltas.size() == 1U &&
            deltas.front().kind == term::Terminal_backing_delta_kind::MODE_TRANSITIONED &&
            deltas.front().active_buffer_before == active_buffer_before &&
            deltas.front().active_buffer_after  == active_buffer_after,
        label);
}

int appended_scrollback_rows_in_backing_deltas(
    const std::vector<term::terminal_backing_delta_t>& deltas)
{
    int rows = 0;
    for (const term::terminal_backing_delta_t& delta : deltas) {
        rows += delta.appended_scrollback_rows;
    }
    return rows;
}

int evicted_scrollback_rows_in_backing_deltas(
    const std::vector<term::terminal_backing_delta_t>& deltas)
{
    int rows = 0;
    for (const term::terminal_backing_delta_t& delta : deltas) {
        rows += delta.evicted_scrollback_rows;
    }
    return rows;
}

int discarded_scrollback_rows_in_backing_deltas(
    const std::vector<term::terminal_backing_delta_t>& deltas)
{
    int rows = 0;
    for (const term::terminal_backing_delta_t& delta : deltas) {
        rows += delta.discarded_scrollback_rows;
    }
    return rows;
}

std::vector<int> row_range(int row_count)
{
    std::vector<int> rows;
    rows.reserve(static_cast<std::size_t>(row_count));
    for (int row = 0; row < row_count; ++row) {
        rows.push_back(row);
    }
    return rows;
}

bool snapshot_valid(const term::Terminal_screen_model& model, std::uint64_t sequence)
{
    return term::validate_render_snapshot(model.render_snapshot(sequence)).status ==
        term::Terminal_render_snapshot_status::OK;
}

term::Terminal_render_snapshot_request request_for_model(
    const term::Terminal_screen_model&     model,
    std::uint64_t                          sequence,
    int                                    offset_from_tail = 0)
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
    return request;
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

bool snapshot_has_hyperlink_uri(
    const term::Terminal_render_snapshot& snapshot,
    const QByteArray&                     uri)
{
    return std::any_of(
        snapshot.hyperlinks.begin(),
        snapshot.hyperlinks.end(),
        [&](const term::Terminal_render_hyperlink_metadata& hyperlink) {
            return hyperlink.uri == uri;
        });
}

std::size_t snapshot_hyperlink_uri_count(
    const term::Terminal_render_snapshot& snapshot,
    std::span<const QByteArray>           uris)
{
    std::size_t count = 0U;
    for (const QByteArray& uri : uris) {
        if (snapshot_has_hyperlink_uri(snapshot, uri)) {
            ++count;
        }
    }
    return count;
}

bool contains_id(
    const std::vector<std::uint64_t>& ids,
    std::uint64_t                     target)
{
    for (std::uint64_t id : ids) {
        if (id == target) {
            return true;
        }
    }
    return false;
}

std::uint64_t max_retained_line_id(const std::vector<std::uint64_t>& ids)
{
    std::uint64_t max_id = 0U;
    for (std::uint64_t id : ids) {
        max_id = std::max(max_id, id);
    }
    return max_id;
}

term::Terminal_retained_line_provenance primary_retained_line_provenance(
    const term::Terminal_screen_model& model,
    int                                logical_row)
{
    return model.retained_line_provenance_for_testing(
        term::Terminal_buffer_id::PRIMARY,
        logical_row);
}

term::Terminal_retained_line_provenance alternate_retained_line_provenance(
    const term::Terminal_screen_model& model,
    int                                logical_row)
{
    return model.retained_line_provenance_for_testing(
        term::Terminal_buffer_id::ALTERNATE,
        logical_row);
}

std::uint64_t primary_retained_line_id(
    const term::Terminal_screen_model& model,
    int                                logical_row)
{
    return primary_retained_line_provenance(model, logical_row).retained_line_id;
}

std::uint64_t alternate_retained_line_id(
    const term::Terminal_screen_model& model,
    int                                logical_row)
{
    return alternate_retained_line_provenance(model, logical_row).retained_line_id;
}

std::uint64_t primary_retained_line_generation(
    const term::Terminal_screen_model& model,
    int                                logical_row)
{
    return primary_retained_line_provenance(model, logical_row).content_generation;
}

std::vector<std::uint64_t> primary_retained_line_ids(
    const term::Terminal_screen_model& model)
{
    std::vector<std::uint64_t> ids;
    const int retained_row_count = model.scrollback_size() + model.grid_size().rows;
    ids.reserve(static_cast<std::size_t>(retained_row_count));
    for (int logical_row = 0; logical_row < retained_row_count; ++logical_row) {
        ids.push_back(primary_retained_line_id(model, logical_row));
    }
    return ids;
}

struct primary_backing_model_summary_t
{
    term::terminal_grid_size_t      grid_size;
    term::terminal_grid_position_t  cursor;
    term::Terminal_buffer_id        active_buffer = term::Terminal_buffer_id::PRIMARY;
    int                             scrollback_rows = 0;
    std::vector<QString>            visible_rows;
    std::vector<std::uint64_t>      primary_retained_line_ids;
};

primary_backing_model_summary_t primary_backing_model_summary(
    const term::Terminal_screen_model& model)
{
    primary_backing_model_summary_t summary;
    summary.grid_size                 = model.grid_size();
    summary.cursor                    = model.cursor_position();
    summary.active_buffer             = model.active_buffer_id();
    summary.scrollback_rows           = model.scrollback_size();
    summary.primary_retained_line_ids = primary_retained_line_ids(model);
    summary.visible_rows.reserve(static_cast<std::size_t>(summary.grid_size.rows));
    for (int row = 0; row < summary.grid_size.rows; ++row) {
        summary.visible_rows.push_back(model.row_text(row));
    }
    return summary;
}

bool primary_backing_model_summaries_equal(
    const primary_backing_model_summary_t& left,
    const primary_backing_model_summary_t& right)
{
    return
        left.grid_size.rows             == right.grid_size.rows             &&
        left.grid_size.columns          == right.grid_size.columns          &&
        left.cursor.row                 == right.cursor.row                 &&
        left.cursor.column              == right.cursor.column              &&
        left.active_buffer              == right.active_buffer              &&
        left.scrollback_rows            == right.scrollback_rows            &&
        left.visible_rows               == right.visible_rows               &&
        left.primary_retained_line_ids  == right.primary_retained_line_ids;
}

bool check_primary_retained_line_ids_unique(
    const term::Terminal_screen_model& model,
    const char*                        label)
{
    bool ok = true;
    std::vector<std::uint64_t> ids;
    const int retained_row_count = model.scrollback_size() + model.grid_size().rows;
    ids.reserve(static_cast<std::size_t>(retained_row_count));
    for (int logical_row = 0; logical_row < retained_row_count; ++logical_row) {
        const term::Terminal_retained_line_provenance provenance =
            primary_retained_line_provenance(model, logical_row);
        ok &= check(provenance.retained_line_id != 0U, label);
        ok &= check(!contains_id(ids, provenance.retained_line_id), label);
        ids.push_back(provenance.retained_line_id);
    }
    return ok;
}

bool check_alternate_screen_retained_line_ids_unique(
    const term::Terminal_screen_model& model,
    const char*                        label)
{
    bool ok = true;
    std::vector<std::uint64_t> ids;
    ids.reserve(static_cast<std::size_t>(model.grid_size().rows));
    for (int logical_row = 0; logical_row < model.grid_size().rows; ++logical_row) {
        const term::Terminal_retained_line_provenance provenance =
            alternate_retained_line_provenance(model, logical_row);
        ok &= check(provenance.retained_line_id != 0U, label);
        ok &= check(!contains_id(ids, provenance.retained_line_id), label);
        ids.push_back(provenance.retained_line_id);
    }
    return ok;
}

std::vector<term::Terminal_reply> replies_in(
    const term::Terminal_screen_model_result& result)
{
    std::vector<term::Terminal_reply> replies;
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) == term::Parser_action_kind::TERMINAL_REPLY) {
            replies.push_back(std::get<term::Terminal_reply>(action.payload));
        }
    }
    return replies;
}

std::vector<term::Parser_notification> notifications_in(
    const term::Terminal_screen_model_result& result)
{
    std::vector<term::Parser_notification> notifications;
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) == term::Parser_action_kind::NOTIFICATION) {
            notifications.push_back(std::get<term::Parser_notification>(action.payload));
        }
    }
    return notifications;
}

const term::Terminal_reply& reply_at(
    const std::vector<term::Terminal_reply>&   replies,
    std::size_t                                index)
{
    if (index >= replies.size()) {
        fail_required_value("expected terminal reply");
    }

    return replies[index];
}

const term::Terminal_render_cell& cell_at(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row,
    int                                    column)
{
    for (const term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row == row && cell.position.column == column) {
            return cell;
        }
    }

    fail_required_value("expected render cell");
}

const term::Terminal_render_cell* cell_at_or_null(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row,
    int                                    column)
{
    for (const term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row == row && cell.position.column == column) {
            return &cell;
        }
    }

    return nullptr;
}

bool check_cell_background_palette(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row,
    int                                    column,
    std::uint16_t                          palette_index,
    const char*                            label)
{
    bool ok = true;
    const term::Terminal_render_cell* cell = cell_at_or_null(snapshot, row, column);
    ok &= check(cell != nullptr, label);
    if (cell == nullptr) {
        return false;
    }

    ok &= check(static_cast<std::size_t>(cell->style_id) < snapshot.styles.size(), label);
    if (static_cast<std::size_t>(cell->style_id) >= snapshot.styles.size()) {
        return false;
    }

    const term::Terminal_text_style& style =
        snapshot.styles[static_cast<std::size_t>(cell->style_id)];
    ok &= check(
        style.background.kind == term::Terminal_color_ref_kind::PALETTE_INDEX &&
            style.background.palette_index == palette_index,
        label);
    return ok;
}

bool check_row_background_palette(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row,
    int                                    first_column,
    int                                    column_count,
    std::uint16_t                          palette_index,
    const char*                            label)
{
    bool ok = true;
    for (int column = first_column; column < first_column + column_count; ++column) {
        ok &= check_cell_background_palette(snapshot, row, column, palette_index, label);
    }
    return ok;
}

bool test_cursor_addressing_and_split_csi()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(3, 5);
    term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("A\x1b[2;3HB"));
    ok &= check(diagnostic_count(result) == 0, "CUP has no diagnostics");
    ok &= check(model.row_text(0) == QStringLiteral("A"), "CUP leaves first row");
    ok &= check(model.row_text(1) == QStringLiteral("  B"), "CUP writes addressed row");
    ok &= check(model.cursor_position().row == 1 && model.cursor_position().column == 3,
        "CUP cursor advances after write");

    result  = model.ingest(QByteArray("\x1b[", 2));
    ok     &= check(diagnostic_count(result) == 0, "split CSI prefix waits");
    ok     &= check(model.cursor_position().row == 1 && model.cursor_position().column == 3,
        "split CSI prefix does not move cursor");

    result  = model.ingest(QByteArrayLiteral("3;99HC"));
    ok     &= check(diagnostic_count(result) == 0, "split CUP completes");
    ok     &= check(model.row_text(2) == QStringLiteral("    C"), "CUP clamps column");
    ok     &= check(model.cursor_position().row == 2 && model.cursor_position().column == 4,
        "CUP clamps cursor position");
    ok     &= check(snapshot_valid(model, 10U), "CUP snapshot validates");

    term::Terminal_screen_model relative_model = make_model(3, 5);
    result  = relative_model.ingest(QByteArrayLiteral("\x1b[2;2H\x1b[CX\x1b[2DY\x1b[BZ\x1b[2AC"));
    ok     &= check(diagnostic_count(result) == 0, "relative cursor movement has no diagnostics");
    ok     &= check(relative_model.row_text(0) == QStringLiteral("   C"),
        "relative CUU/CUF/CUB writes first row");
    ok     &= check(relative_model.row_text(1) == QStringLiteral(" YX"),
        "relative CUF/CUB writes middle row");
    ok     &= check(relative_model.row_text(2) == QStringLiteral("  Z"),
        "relative CUD writes lower row");
    ok     &= check(relative_model.cursor_position().row == 0 &&
        relative_model.cursor_position().column == 4,
        "relative cursor movement leaves cursor after final write");

    term::Terminal_screen_model scroll_region_relative_model = make_model(5, 5);
    result  = scroll_region_relative_model.ingest(
        QByteArrayLiteral("\x1b[2;4r\x1b[2;3H\x1b[10AX\x1b[4;1H\x1b[10BY"));
    ok     &= check(diagnostic_count(result) == 0,
        "scroll-region relative cursor movement has no diagnostics");
    ok     &= check(scroll_region_relative_model.row_text(1) == QStringLiteral("  X"),
        "CUU stops at the scroll-region top margin");
    ok     &= check(scroll_region_relative_model.row_text(3) == QStringLiteral("Y"),
        "CUD stops at the scroll-region bottom margin");

    term::Terminal_screen_model outside_region_relative_model = make_model(5, 5);
    result  = outside_region_relative_model.ingest(
        QByteArrayLiteral("\x1b[2;4r\x1b[1;1H\x1b[10BZ"));
    ok     &= check(diagnostic_count(result) == 0,
        "outside-scroll-region relative cursor movement has no diagnostics");
    ok     &= check(outside_region_relative_model.row_text(4) == QStringLiteral("Z"),
        "CUD outside the scroll region still clamps to the screen");

    term::Terminal_screen_model horizontal_model = make_model(2, 5);
    result  = horizontal_model.ingest(QByteArrayLiteral("abcde\x1b[2;3H\x1b[GZ\x1b[999GY"));
    ok     &= check(diagnostic_count(result) == 0, "CHA has no diagnostics");
    ok     &= check(horizontal_model.row_text(0) == QStringLiteral("abcde"),
        "CHA preserves previous row content");
    ok     &= check(horizontal_model.row_text(1) == QStringLiteral("Z   Y"),
        "CHA addresses absolute columns on the current row");
    ok     &= check(horizontal_model.cursor_position().row == 1 &&
        horizontal_model.cursor_position().column == 4,
        "CHA clamps the target column");

    term::Terminal_screen_model hvp_model = make_model(3, 5);
    result  = hvp_model.ingest(QByteArrayLiteral("\x1b[2;2fZ"));
    ok     &= check(diagnostic_count(result) == 0, "HVP has no diagnostics");
    ok     &= check(hvp_model.row_text(1) == QStringLiteral(" Z"), "HVP writes addressed row");
    ok     &= dirty_rows_equal(result, {0, 1}, "HVP dirties old and new cursor rows");
    ok     &= check(snapshot_valid(hvp_model, 11U), "HVP snapshot validates");

    term::Terminal_screen_model direct_model = make_model(3, 5);
    direct_model.apply_action(
        term::make_csi_dispatch_action({2, 3}, QByteArrayLiteral("H")));
    direct_model.ingest(QByteArrayLiteral("Q"));
    ok &= check(direct_model.row_text(1) == QStringLiteral("  Q"),
        "direct CSI action carries parameters");

    term::Terminal_screen_model c0_model = make_model(3, 5);
    result  = c0_model.ingest(QByteArrayLiteral("A\x1b[\n"));
    ok     &= check(diagnostic_count(result) == 0, "split CSI with C0 waits after executing C0");
    result  = c0_model.ingest(QByteArrayLiteral("qB"));
    ok     &= check(diagnostic_count(result) == 1, "CSI with C0 emits unsupported final diagnostic");
    ok     &= check(c0_model.row_text(0) == QStringLiteral("A"),
        "CSI C0 preserves text before line feed");
    ok     &= check(c0_model.row_text(1) == QStringLiteral(" B"),
        "CSI C0 executes line feed while collecting CSI");
    ok     &= check(snapshot_valid(c0_model, 12U), "CSI C0 snapshot validates");

    return ok;
}

bool test_scrollback_growth_observer_seam()
{
    bool ok = true;

    term::Terminal_screen_model model =
        make_recovery_disabled_primary_backing_model(2, 4, 4);
    Scrollback_delta_observer observer(false);

    term::Terminal_screen_model helper_contract_model = make_model(2, 4, 4);
    helper_contract_model.ingest(QByteArrayLiteral("aa\r\nbb\r\ncc"));
    Scrollback_delta_observer recovery_observer(true);
    const auto helper_contract = recovery_observer.observe(
        helper_contract_model,
        Primary_backing_boundary::SCROLLBACK_CLEAR,
        Scrollback_delta_operation_annotation::CLEAR_SCROLLBACK,
        [&] {
            return helper_contract_model.ingest(QByteArrayLiteral("\x1b[3J"));
        });
    ok &= check(helper_contract.boundary_annotation == Primary_backing_boundary::SCROLLBACK_CLEAR &&
        helper_contract.operation_annotation ==
            Scrollback_delta_operation_annotation::CLEAR_SCROLLBACK &&
        helper_contract.classification_annotation ==
            Primary_backing_observation_classification::TEST_ONLY &&
        helper_contract.recovery_enabled_annotation,
        "observer seam preserves caller-supplied helper annotations");

    auto check_allowed_growth_source = [&](const QByteArray& bytes,
        int expected_cursor_column,
        const char* label) {
        bool source_ok = true;

        term::Terminal_screen_model source_model =
            make_recovery_disabled_primary_backing_model(2, 4, 4);
        Scrollback_delta_observer source_observer(false);

        source_model.ingest(QByteArrayLiteral("aa\r\nbb"));
        const std::vector<std::uint64_t> pre_source_ids =
            primary_retained_line_ids(source_model);
        const std::uint64_t pre_max_line_id = max_retained_line_id(pre_source_ids);

        source_ok &= check(source_model.scrollback_size() == 0, label);
        source_ok &= check(pre_source_ids.size() == 2U, label);

        const auto observation = source_observer.observe(
            source_model,
            Primary_backing_boundary::INGEST,
            Scrollback_delta_operation_annotation::TERMINAL_SCROLL,
            [&] {
                return source_model.ingest(bytes);
            });
        const std::vector<std::uint64_t> post_source_ids =
            primary_retained_line_ids(source_model);

        source_ok &= check(scrollback_rows_delta(observation) == 1, label);
        source_ok &= check(observation.result_scrollback_rows == observation.scrollback_rows_after,
            label);
        source_ok &= single_primary_backing_delta_equal(
            observation.backing_deltas,
            term::Terminal_backing_delta_kind::PRIMARY_HISTORY_APPENDED,
            0,
            1,
            1,
            0,
            label);
        source_ok &= check(!observation.recovery_enabled_annotation, label);
        source_ok &= check(source_model.scrollback_size() == 1, label);
        source_ok &= check(source_model.row_text(0) == QStringLiteral("bb"), label);
        source_ok &= check(source_model.row_text(1).isEmpty(), label);
        source_ok &= check(source_model.cursor_position().row == 1 &&
            source_model.cursor_position().column == expected_cursor_column,
            label);
        source_ok &= check(post_source_ids.size() == pre_source_ids.size() + 1U, label);
        if (pre_source_ids.size() == 2U && post_source_ids.size() == 3U) {
            source_ok &= check(post_source_ids[0] == pre_source_ids[0], label);
            source_ok &= check(post_source_ids[1] == pre_source_ids[1], label);
            source_ok &= check(post_source_ids[2] > pre_max_line_id, label);
        }
        source_ok &= check_primary_retained_line_ids_unique(source_model, label);

        return source_ok;
    };

    ok &= check_allowed_growth_source(
        QByteArrayLiteral("\n"),
        2,
        "LF is an allowed retained-history growth source");
    ok &= check_allowed_growth_source(
        QByteArrayLiteral("\x1b" "D"),
        2,
        "IND is an allowed retained-history growth source");
    ok &= check_allowed_growth_source(
        QByteArrayLiteral("\x1b" "E"),
        0,
        "NEL is an allowed retained-history growth source");
    ok &= check_allowed_growth_source(
        QByteArrayLiteral("\r\n"),
        0,
        "CRLF overflow is an allowed retained-history growth source");

    const auto terminal_scroll = observer.observe(
        model,
        Primary_backing_boundary::INGEST,
        Scrollback_delta_operation_annotation::TERMINAL_SCROLL,
        [&] {
            return model.ingest(QByteArrayLiteral("aa\r\nbb\r\ncc"));
        });
    ok &= check(scrollback_rows_delta(terminal_scroll) == 1,
        "terminal scroll observation records retained-history growth");
    ok &= check(terminal_scroll.result_scrollback_rows == terminal_scroll.scrollback_rows_after,
        "terminal scroll observation records the model result row count");
    ok &= single_primary_backing_delta_equal(
        terminal_scroll.backing_deltas,
        term::Terminal_backing_delta_kind::PRIMARY_HISTORY_APPENDED,
        0,
        1,
        1,
        0,
        "terminal scroll observation records the backing append delta");
    ok &= check(terminal_scroll.active_buffer_before == term::Terminal_buffer_id::PRIMARY &&
        terminal_scroll.active_buffer_after == term::Terminal_buffer_id::PRIMARY,
        "terminal scroll observation records primary buffer ownership");
    ok &= check(model.row_text(0) == QStringLiteral("bb"),
        "terminal scroll leaves the first visible row");
    ok &= check(model.row_text(1) == QStringLiteral("cc"),
        "terminal scroll leaves the second visible row");

    const auto repaint = observer.observe(
        model,
        Primary_backing_boundary::INGEST,
        Scrollback_delta_operation_annotation::REPAINT,
        [&] {
            return model.ingest(QByteArrayLiteral("\x1b[1;1Hdd\x1b[K\x1b[2;1Hee\x1b[K"));
        });
    ok &= check(scrollback_rows_delta(repaint) == 0,
        "repaint-shaped observation does not record retained-history growth");
    ok &= check(repaint.result_scrollback_rows == terminal_scroll.scrollback_rows_after,
        "repaint-shaped observation preserves retained-history count");
    ok &= check(repaint.backing_deltas.empty(),
        "repaint-shaped observation has no backing storage delta");
    ok &= check(model.row_text(0) == QStringLiteral("dd"),
        "repaint-shaped observation rewrites the first visible row");
    ok &= check(model.row_text(1) == QStringLiteral("ee"),
        "repaint-shaped observation rewrites the second visible row");

    const auto clear = observer.observe(
        model,
        Primary_backing_boundary::SCROLLBACK_CLEAR,
        Scrollback_delta_operation_annotation::CLEAR_SCROLLBACK,
        [&] {
            return model.ingest(QByteArrayLiteral("\x1b[3J"));
        });
    ok &= check(scrollback_rows_delta(clear) == -1,
        "scrollback clear observation records retained-history removal");
    ok &= check(clear.scrollback_rows_after == 0,
        "scrollback clear observation records the cleared retained-history count");
    ok &= check(clear.evicted_scrollback_rows == clear.scrollback_rows_before,
        "scrollback clear observation records evicted retained-history rows");
    ok &= single_primary_backing_delta_equal(
        clear.backing_deltas,
        term::Terminal_backing_delta_kind::PRIMARY_HISTORY_CLEARED,
        clear.scrollback_rows_before,
        0,
        0,
        clear.scrollback_rows_before,
        "scrollback clear observation records the backing clear delta");

    const auto empty_clear = observer.observe(
        model,
        Primary_backing_boundary::SCROLLBACK_CLEAR,
        Scrollback_delta_operation_annotation::CLEAR_SCROLLBACK,
        [&] {
            return model.ingest(QByteArrayLiteral("\x1b[3J"));
        });
    ok &= single_primary_backing_delta_equal(
        empty_clear.backing_deltas,
        term::Terminal_backing_delta_kind::BACKING_UNCHANGED,
        0,
        0,
        0,
        0,
        "empty scrollback clear records an explicit backing no-op delta");

    const term::Terminal_screen_model_result erase_visible_result =
        model.ingest(QByteArrayLiteral("\x1b[2J"));
    ok &= check(erase_visible_result.backing_deltas.empty(),
        "ED2 visible clear has no backing storage delta");

    const term::Terminal_screen_model_result resize_result =
        model.resize(term::terminal_grid_size_t{3, 4});
    ok &= single_active_grid_backing_delta_equal(
        resize_result.backing_deltas,
        term::Terminal_backing_delta_kind::ACTIVE_GRID_RESIZED,
        term::terminal_grid_size_t{2, 4},
        term::terminal_grid_size_t{3, 4},
        "row-count resize records an active-grid backing delta");

    const term::Terminal_screen_model_result reflow_result =
        model.resize(term::terminal_grid_size_t{3, 6});
    ok &= single_active_grid_backing_delta_equal(
        reflow_result.backing_deltas,
        term::Terminal_backing_delta_kind::COLUMN_REFLOWED,
        term::terminal_grid_size_t{3, 4},
        term::terminal_grid_size_t{3, 6},
        "column-count resize records a column-reflow backing delta");

    const term::Terminal_screen_model_result alternate_enter =
        model.ingest(QByteArrayLiteral("\x1b[?47h"));
    ok &= single_mode_transition_backing_delta_equal(
        alternate_enter.backing_deltas,
        term::Terminal_buffer_id::PRIMARY,
        term::Terminal_buffer_id::ALTERNATE,
        "alternate-screen enter records a mode-transition backing delta");

    const term::Terminal_screen_model_result alternate_leave =
        model.ingest(QByteArrayLiteral("\x1b[?47l"));
    ok &= single_mode_transition_backing_delta_equal(
        alternate_leave.backing_deltas,
        term::Terminal_buffer_id::ALTERNATE,
        term::Terminal_buffer_id::PRIMARY,
        "alternate-screen leave records a mode-transition backing delta");

    return ok;
}

bool test_phase_r_primary_repaint_recovery_accepts_distinct_shift()
{
    bool ok = true;

    term::Terminal_screen_model model =
        make_recovery_enabled_primary_repaint_model(4, 8, 8);
    model.ingest(visible_row_write_stream({
        QByteArrayLiteral("aa"),
        QByteArrayLiteral("bb"),
        QByteArrayLiteral("cc"),
        QByteArrayLiteral("dd"),
    }, false));

    const term::Terminal_screen_model_result result =
        model.ingest(visible_row_write_stream({
            QByteArrayLiteral("bb"),
            QByteArrayLiteral("cc"),
            QByteArrayLiteral("dd"),
            QByteArrayLiteral("ee"),
        }, true));

    ok &= check(model.scrollback_size() == 1,
        "Phase R recovery accepts one provable shifted repaint row");
    ok &= check(result.scrollback_rows == 1,
        "Phase R recovery result reports recovered retained row");
    ok &= check(result.backing_deltas.size() == 1U &&
            primary_backing_delta_matches(
                result.backing_deltas[0],
                term::Terminal_backing_delta_kind::PRIMARY_HISTORY_APPENDED,
                0,
                1,
                1,
                0),
        "Phase R recovery emits a primary-history append delta");
    ok &= check(result.recovery_proposals.size() == 1U &&
            result.recovery_proposals[0].reason ==
                term::Terminal_recovery_proposal_reason::PRIMARY_REPAINT_SHIFTED_VISIBLE_ROWS &&
            result.recovery_proposals[0].status ==
                term::Terminal_recovery_proposal_status::ACCEPTED &&
            result.recovery_proposals[0].provenance_source ==
                term::Terminal_retained_line_provenance_source::RECOVERED_PRIMARY_REPAINT &&
            result.recovery_proposals[0].candidate_visible_rows == 4 &&
            result.recovery_proposals[0].recovered_row_count == 1 &&
            result.recovery_proposals[0].visible_row_identity_ambiguous,
        "Phase R recovery records accepted proposal metadata");
    ok &= check(model.row_text(0) == QStringLiteral("bb") &&
            model.row_text(1) == QStringLiteral("cc") &&
            model.row_text(2) == QStringLiteral("dd") &&
            model.row_text(3) == QStringLiteral("ee"),
        "Phase R recovery keeps the repainted active grid");
    ok &= check(primary_retained_line_provenance(model, 0).source ==
            term::Terminal_retained_line_provenance_source::RECOVERED_PRIMARY_REPAINT,
        "Phase R recovery stamps recovered retained-row provenance");
    ok &= check(primary_retained_line_provenance(model, 1).source ==
            term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        "Phase R recovery keeps repainted active rows on terminal-storage provenance");

    const term::Terminal_render_snapshot scrollback_snapshot =
        model.render_snapshot(request_for_model(model, 80U, 1));
    ok &= check(snapshot_row_text(scrollback_snapshot, 0) == QStringLiteral("aa") &&
            snapshot_row_text(scrollback_snapshot, 1) == QStringLiteral("bb") &&
            snapshot_row_text(scrollback_snapshot, 2) == QStringLiteral("cc") &&
            snapshot_row_text(scrollback_snapshot, 3) == QStringLiteral("dd"),
        "Phase R recovery exposes the recovered row through primary backing");

    const term::Terminal_screen_model_result non_recovery_result =
        model.ingest(QByteArrayLiteral("z"));
    ok &= check(non_recovery_result.recovery_proposals.empty(),
        "Phase R recovery proposal metadata clears on later non-recovery ingest");

    return ok;
}

bool test_flat_ring_phase3_retained_record_producer_contract()
{
    bool ok = true;

    const QByteArray uri =
        QByteArrayLiteral("https://phase3.varinomics.example/retained");
    const QByteArray styled_row =
        styled_hyperlink_row_bytes(QByteArrayLiteral("aa"), uri);

    term::Terminal_screen_model normal_model =
        make_recovery_disabled_primary_backing_model(4, 8, 8);
    normal_model.ingest(visible_row_write_stream({
        styled_row,
        QByteArrayLiteral("bb"),
        QByteArrayLiteral("cc"),
        QByteArrayLiteral("dd"),
    }, false));
    const term::Terminal_screen_model_result normal_result =
        normal_model.ingest(QByteArrayLiteral("\r\nee"));

    term::Terminal_screen_model recovered_model =
        make_recovery_enabled_primary_repaint_model(4, 8, 8);
    recovered_model.ingest(visible_row_write_stream({
        styled_row,
        QByteArrayLiteral("bb"),
        QByteArrayLiteral("cc"),
        QByteArrayLiteral("dd"),
    }, false));
    const term::Terminal_screen_model_result recovered_result =
        recovered_model.ingest(visible_row_write_stream({
            QByteArrayLiteral("bb"),
            QByteArrayLiteral("cc"),
            QByteArrayLiteral("dd"),
            QByteArrayLiteral("ee"),
        }, true));

    ok &= check(normal_model.scrollback_size() == 1 &&
            normal_result.recovery_proposals.empty(),
        "flat-ring Phase 3 normal scrollout seals one retained row without recovery");
    ok &= check(recovered_model.scrollback_size() == 1 &&
            recovered_result.recovery_proposals.size() == 1U,
        "flat-ring Phase 3 accepted recovery seals one retained row");

    const term::Terminal_render_snapshot normal_snapshot =
        normal_model.render_snapshot(request_for_model(normal_model, 90U, 1));
    const term::Terminal_render_snapshot recovered_snapshot =
        recovered_model.render_snapshot(request_for_model(recovered_model, 91U, 1));
    ok &= check(snapshot_row_text(normal_snapshot, 0) == QStringLiteral("aa") &&
            snapshot_row_text(recovered_snapshot, 0) == QStringLiteral("aa"),
        "flat-ring Phase 3 normal and recovered retained records carry canonical content");
    ok &= check(snapshot_has_hyperlink_uri(normal_snapshot, uri) &&
            snapshot_has_hyperlink_uri(recovered_snapshot, uri),
        "flat-ring Phase 3 normal and recovered retained records materialize row-local hyperlinks");
    ok &= check_cell_background_palette(
        normal_snapshot,
        0,
        0,
        3U,
        "flat-ring Phase 3 normal retained record keeps session style id");
    ok &= check_cell_background_palette(
        recovered_snapshot,
        0,
        0,
        3U,
        "flat-ring Phase 3 recovered retained record keeps session style id");

    const term::Terminal_retained_line_provenance normal_provenance =
        normal_model.retained_line_provenance_for_testing(term::Terminal_buffer_id::PRIMARY, 0);
    const term::Terminal_retained_line_provenance recovered_provenance =
        recovered_model.retained_line_provenance_for_testing(term::Terminal_buffer_id::PRIMARY, 0);
    ok &= check(normal_provenance.retained_line_id != 0U &&
            normal_provenance.source ==
                term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        "flat-ring Phase 3 normal retained record keeps terminal-storage provenance");
    ok &= check(recovered_provenance.retained_line_id != 0U &&
            recovered_provenance.source ==
                term::Terminal_retained_line_provenance_source::RECOVERED_PRIMARY_REPAINT,
        "flat-ring Phase 3 recovered retained record keeps recovered provenance");

    const std::optional<term::terminal_retained_row_record_metadata_t> normal_metadata =
        normal_model.retained_row_record_metadata_for_testing(
            term::Terminal_buffer_id::PRIMARY,
            0);
    const std::optional<term::terminal_retained_row_record_metadata_t> recovered_metadata =
        recovered_model.retained_row_record_metadata_for_testing(
            term::Terminal_buffer_id::PRIMARY,
            0);
    ok &= check(normal_metadata.has_value() &&
            recovered_metadata.has_value() &&
            normal_metadata->source_width == 8 &&
            recovered_metadata->source_width == 8,
        "flat-ring Phase 3 producer records retained source width");
    ok &= check(normal_metadata.has_value() &&
            recovered_metadata.has_value() &&
            normal_metadata->style_lifetime ==
                term::Terminal_retained_row_style_lifetime::SESSION_LIFETIME_STYLE_ID &&
            recovered_metadata->style_lifetime ==
                term::Terminal_retained_row_style_lifetime::SESSION_LIFETIME_STYLE_ID,
        "flat-ring Phase 3 producer records session-lifetime style policy");
    ok &= check(normal_metadata.has_value() &&
            recovered_metadata.has_value() &&
            normal_metadata->wrap_state ==
                term::Terminal_retained_row_wrap_state::HARD_BOUNDARY &&
            recovered_metadata->wrap_state ==
                term::Terminal_retained_row_wrap_state::HARD_BOUNDARY,
        "flat-ring Phase 3 producer records current hard-boundary wrap metadata");

    return ok;
}

bool test_flat_ring_phase7_recovery_shared_producer_boundary()
{
    bool ok = true;

    const QByteArray uri =
        QByteArrayLiteral("https://phase7.varinomics.example/recovered-ring");
    const QByteArray styled_row =
        styled_hyperlink_row_bytes(QByteArrayLiteral("aa"), uri);

    term::Terminal_screen_model recovered_model =
        make_recovery_enabled_primary_repaint_model(4, 8, 8);
    recovered_model.ingest(visible_row_write_stream({
        styled_row,
        QByteArrayLiteral("bb"),
        QByteArrayLiteral("cc"),
        QByteArrayLiteral("dd"),
    }, false));
    const term::Terminal_screen_model_result recovered_result =
        recovered_model.ingest(visible_row_write_stream({
            QByteArrayLiteral("bb"),
            QByteArrayLiteral("cc"),
            QByteArrayLiteral("dd"),
            QByteArrayLiteral("ee"),
        }, true));

    ok &= check(recovered_model.scrollback_size() == 1 &&
            recovered_result.backing_deltas.size() == 1U &&
            primary_backing_delta_matches(
                recovered_result.backing_deltas[0],
                term::Terminal_backing_delta_kind::PRIMARY_HISTORY_APPENDED,
                0,
                1,
                1,
                0) &&
            recovered_result.recovery_proposals.size() == 1U &&
            recovered_result.recovery_proposals[0].provenance_source ==
                term::Terminal_retained_line_provenance_source::RECOVERED_PRIMARY_REPAINT,
        "flat-ring Phase 7 recovery appends one retained row through the primary-history delta path");

    const std::optional<term::terminal_history_handle_t> recovered_handle =
        recovered_model.retained_history_handle_at_logical_row(
            term::Terminal_buffer_id::PRIMARY,
            0);
    const term::Terminal_retained_line_lookup_result recovered_lookup =
        recovered_handle.has_value()
            ? recovered_model.retained_line_lookup(
                  term::Terminal_buffer_id::PRIMARY,
                  *recovered_handle)
            : term::Terminal_retained_line_lookup_result{};
    ok &= check(recovered_handle.has_value() &&
            recovered_handle->record_bytes != 0U &&
            recovered_lookup.resolution_status ==
                term::Terminal_history_resolution_status::OK &&
            recovered_lookup.exact_match &&
            recovered_lookup.exact_logical_row == 0,
        "flat-ring Phase 7 recovered row resolves through an authoritative ring handle");

    const term::Terminal_retained_line_provenance recovered_provenance =
        primary_retained_line_provenance(recovered_model, 0);
    ok &= check(recovered_provenance.retained_line_id != 0U &&
            recovered_provenance.source ==
                term::Terminal_retained_line_provenance_source::RECOVERED_PRIMARY_REPAINT,
        "flat-ring Phase 7 recovered ring materialization carries recovered provenance");

    const std::optional<term::terminal_retained_row_record_metadata_t> recovered_metadata =
        recovered_model.retained_row_record_metadata_for_testing(
            term::Terminal_buffer_id::PRIMARY,
            0);
    ok &= check(recovered_metadata.has_value() &&
            recovered_metadata->source_width == 8 &&
            recovered_metadata->style_lifetime ==
                term::Terminal_retained_row_style_lifetime::SESSION_LIFETIME_STYLE_ID &&
            recovered_metadata->wrap_state ==
                term::Terminal_retained_row_wrap_state::HARD_BOUNDARY,
        "flat-ring Phase 7 recovered row carries shared-producer metadata in ring storage");

    const term::Terminal_render_snapshot recovered_snapshot =
        recovered_model.render_snapshot(request_for_model(recovered_model, 92U, 1));
    ok &= check(snapshot_row_text(recovered_snapshot, 0) == QStringLiteral("aa") &&
            snapshot_has_hyperlink_uri(recovered_snapshot, uri),
        "flat-ring Phase 7 recovered row materializes shared-producer content and hyperlinks");
    ok &= check_cell_background_palette(
        recovered_snapshot,
        0,
        0,
        3U,
        "flat-ring Phase 7 recovered row materializes shared-producer style ids");

    term::Terminal_screen_model disabled_model =
        make_recovery_disabled_primary_backing_model(4, 8, 8);
    disabled_model.ingest(visible_row_write_stream({
        styled_row,
        QByteArrayLiteral("bb"),
        QByteArrayLiteral("cc"),
        QByteArrayLiteral("dd"),
    }, false));
    const term::Terminal_screen_model_result disabled_result =
        disabled_model.ingest(QByteArrayLiteral("\r\nee"));

    ok &= check(disabled_model.scrollback_size() == 1 &&
            disabled_result.recovery_proposals.empty() &&
            disabled_result.backing_deltas.size() == 1U &&
            primary_backing_delta_matches(
                disabled_result.backing_deltas[0],
                term::Terminal_backing_delta_kind::PRIMARY_HISTORY_APPENDED,
                0,
                1,
                1,
                0),
        "flat-ring Phase 7 recovery-disabled normal scrollout appends through normal history");

    const std::optional<term::terminal_history_handle_t> disabled_handle =
        disabled_model.retained_history_handle_at_logical_row(
            term::Terminal_buffer_id::PRIMARY,
            0);
    const term::Terminal_retained_line_lookup_result disabled_lookup =
        disabled_handle.has_value()
            ? disabled_model.retained_line_lookup(
                  term::Terminal_buffer_id::PRIMARY,
                  *disabled_handle)
            : term::Terminal_retained_line_lookup_result{};
    const term::Terminal_retained_line_provenance disabled_provenance =
        primary_retained_line_provenance(disabled_model, 0);
    const std::optional<term::terminal_retained_row_record_metadata_t> disabled_metadata =
        disabled_model.retained_row_record_metadata_for_testing(
            term::Terminal_buffer_id::PRIMARY,
            0);
    ok &= check(disabled_handle.has_value() &&
            disabled_handle->record_bytes != 0U &&
            disabled_lookup.resolution_status ==
                term::Terminal_history_resolution_status::OK &&
            disabled_lookup.exact_match &&
            disabled_lookup.exact_logical_row == 0 &&
            disabled_provenance.retained_line_id != 0U &&
            disabled_provenance.source ==
                term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE &&
            disabled_metadata.has_value() &&
            disabled_metadata->source_width == 8,
        "flat-ring Phase 7 recovery-disabled normal scrollout remains terminal-storage ring history");

    return ok;
}

bool test_flat_ring_phase5a_resize_projects_retained_history_without_mutation()
{
    bool ok = true;

    term::Terminal_screen_model model =
        make_recovery_disabled_primary_backing_model(2, 8, 8);
    model.ingest(
        QByteArrayLiteral("\x1b[1;1H123456") +
        bytes_from_hex("e7958c") +
        QByteArrayLiteral("\x1b[2;1Htail"));
    const term::Terminal_screen_model_result scroll_result =
        model.ingest(QByteArrayLiteral("\r\nnext"));
    ok &= check(model.scrollback_size() == 1 &&
            scroll_result.viewport_changed,
        "flat-ring Phase 5A fixture creates one retained row");

    const term::Terminal_retained_line_provenance before_provenance =
        model.retained_line_provenance_for_testing(term::Terminal_buffer_id::PRIMARY, 0);
    const std::optional<term::terminal_retained_row_record_metadata_t> before_metadata =
        model.retained_row_record_metadata_for_testing(term::Terminal_buffer_id::PRIMARY, 0);
    const term::Terminal_selection_range prefix_range = {
        {0, 0},
        {0, 6},
        term::Terminal_selection_mode::NORMAL,
    };
    const std::vector<term::terminal_selection_line_lease_t> prefix_leases =
        model.selection_line_leases(term::Terminal_buffer_id::PRIMARY, prefix_range);
    ok &= check(before_provenance.retained_line_id != 0U &&
            before_metadata.has_value() &&
            before_metadata->source_width == 8 &&
            prefix_leases.size() == 1U,
        "flat-ring Phase 5A fixture captures source-width retained row and lease");
    if (!before_metadata.has_value()) {
        return ok;
    }

    const term::Terminal_screen_model_result narrow_resize = model.resize({3, 7});
    const term::Terminal_retained_line_provenance narrow_provenance =
        model.retained_line_provenance_for_testing(term::Terminal_buffer_id::PRIMARY, 0);
    const std::optional<term::terminal_retained_row_record_metadata_t> narrow_metadata =
        model.retained_row_record_metadata_for_testing(term::Terminal_buffer_id::PRIMARY, 0);
    const term::Terminal_render_snapshot narrow_snapshot =
        model.render_snapshot(request_for_model(model, 92U, model.scrollback_size()));
    const term::Terminal_selection_result prefix_after_resize =
        model.selected_text(
            term::Terminal_buffer_id::PRIMARY,
            prefix_range,
            std::span<const term::terminal_selection_line_lease_t>(
                prefix_leases.data(),
                prefix_leases.size()));
    ok &= check(narrow_resize.grid_reflow_changed &&
            !narrow_resize.terminal_content_changed,
        "flat-ring Phase 5A resize advances geometry without reporting content mutation");
    ok &= check(narrow_metadata.has_value() &&
            narrow_provenance.content_generation ==
                before_provenance.content_generation &&
            narrow_metadata->source_width == before_metadata->source_width,
        "flat-ring Phase 5A narrow resize leaves retained record content immutable");
    ok &= check(term::validate_render_snapshot(narrow_snapshot).status ==
            term::Terminal_render_snapshot_status::OK &&
            narrow_snapshot.grid_size.rows == 3 &&
            narrow_snapshot.grid_size.columns == 7 &&
            snapshot_row_text(narrow_snapshot, 0) == QStringLiteral("123456"),
        "flat-ring Phase 5A narrow projection repairs wide cell geometry at the edge");
    ok &= check(prefix_after_resize.code == term::Terminal_selection_result_code::OK &&
            prefix_after_resize.text == QStringLiteral("123456"),
        "flat-ring Phase 5A retained selection lease resolves after geometry-only resize");

    const term::Terminal_screen_model_result wide_resize = model.resize({2, 10});
    const term::Terminal_retained_line_provenance wide_provenance =
        model.retained_line_provenance_for_testing(term::Terminal_buffer_id::PRIMARY, 0);
    const std::optional<term::terminal_retained_row_record_metadata_t> wide_metadata =
        model.retained_row_record_metadata_for_testing(term::Terminal_buffer_id::PRIMARY, 0);
    const term::Terminal_render_snapshot wide_snapshot =
        model.render_snapshot(request_for_model(model, 93U, model.scrollback_size()));
    const term::Terminal_selection_range trailing_blank_range = {
        {0, 8},
        {0, 9},
        term::Terminal_selection_mode::NORMAL,
    };
    const std::vector<term::terminal_selection_line_lease_t> trailing_blank_leases =
        model.selection_line_leases(
            term::Terminal_buffer_id::PRIMARY,
            trailing_blank_range);
    const term::Terminal_selection_result trailing_blank =
        model.selected_text(
            term::Terminal_buffer_id::PRIMARY,
            trailing_blank_range,
            std::span<const term::terminal_selection_line_lease_t>(
                trailing_blank_leases.data(),
                trailing_blank_leases.size()));
    ok &= check(wide_resize.grid_reflow_changed &&
            !wide_resize.terminal_content_changed,
        "flat-ring Phase 5A width growth remains geometry-only");
    ok &= check(wide_metadata.has_value() &&
            wide_provenance.content_generation ==
                before_provenance.content_generation &&
            wide_metadata->source_width == before_metadata->source_width,
        "flat-ring Phase 5A width growth leaves retained record content immutable");
    ok &= check(term::validate_render_snapshot(wide_snapshot).status ==
            term::Terminal_render_snapshot_status::OK &&
            wide_snapshot.grid_size.rows == 2 &&
            wide_snapshot.grid_size.columns == 10 &&
            snapshot_row_text(wide_snapshot, 0) == QStringLiteral("123456") +
                QString::fromUtf8(bytes_from_hex("e7958c")),
        "flat-ring Phase 5A width growth projects retained content into wider geometry");
    ok &= check(trailing_blank.code == term::Terminal_selection_result_code::OK &&
            trailing_blank.text == QStringLiteral(" "),
        "flat-ring Phase 5A selection projection exposes widened trailing blanks");

    return ok;
}

bool test_flat_ring_phase5b_retained_hyperlink_metadata_authority()
{
    bool ok = true;

    term::Terminal_screen_model model =
        make_recovery_disabled_primary_backing_model(3, 16, 8);
    std::vector<QByteArray> retained_uris;
    for (int index = 0; index < 7; ++index) {
        const QByteArray uri =
            QByteArrayLiteral("https://phase5b.varinomics.example/retained/") +
            QByteArray::number(index);
        const QByteArray label = QByteArrayLiteral("L") + QByteArray::number(index);
        retained_uris.push_back(uri);
        model.ingest(styled_hyperlink_row_bytes(label, uri) + QByteArrayLiteral("\r\n"));
    }

    const int scrollback_before_active_clear = model.scrollback_size();
    ok &= check(scrollback_before_active_clear >= 4,
        "flat-ring Phase 5B fixture creates hyperlink-heavy retained history");

    const term::Terminal_render_snapshot retained_snapshot =
        model.render_snapshot(
            request_for_model(model, 94U, scrollback_before_active_clear));
    const int visible_retained_rows =
        std::min(scrollback_before_active_clear, model.grid_size().rows);
    ok &= check(term::validate_render_snapshot(retained_snapshot).status ==
            term::Terminal_render_snapshot_status::OK &&
            snapshot_hyperlink_uri_count(
                retained_snapshot,
                std::span<const QByteArray>(
                    retained_uris.data(),
                    static_cast<std::size_t>(visible_retained_rows))) ==
                static_cast<std::size_t>(visible_retained_rows),
        "flat-ring Phase 5B hyperlink-heavy retained snapshot materializes row-local metadata");

    model.ingest(QByteArrayLiteral("\x1b]8;;\x1b\\\x1b[2Jplain"));
    const term::Terminal_render_snapshot after_active_clear_snapshot =
        model.render_snapshot(
            request_for_model(model, 95U, model.scrollback_size()));
    ok &= check(term::validate_render_snapshot(after_active_clear_snapshot).status ==
            term::Terminal_render_snapshot_status::OK &&
            snapshot_has_hyperlink_uri(after_active_clear_snapshot, retained_uris.front()),
        "flat-ring Phase 5B retained hyperlinks survive after active hyperlinks are gone");

    model.set_scrollback_limit(2);
    const int first_surviving_uri =
        scrollback_before_active_clear - model.scrollback_size();
    const term::Terminal_render_snapshot after_eviction_snapshot =
        model.render_snapshot(request_for_model(model, 96U, model.scrollback_size()));
    ok &= check(model.scrollback_size() == 2 &&
            term::validate_render_snapshot(after_eviction_snapshot).status ==
                term::Terminal_render_snapshot_status::OK &&
            snapshot_hyperlink_uri_count(
                after_eviction_snapshot,
                std::span<const QByteArray>(
                    retained_uris.data() + first_surviving_uri,
                    static_cast<std::size_t>(model.scrollback_size()))) ==
                static_cast<std::size_t>(model.scrollback_size()),
        "flat-ring Phase 5B eviction leaves surviving retained hyperlinks self-contained");

    model.ingest(QByteArrayLiteral("\x1b[3J"));
    const term::Terminal_render_snapshot after_clear_snapshot =
        model.render_snapshot(request_for_model(model, 97U));
    ok &= check(model.scrollback_size() == 0 &&
            term::validate_render_snapshot(after_clear_snapshot).status ==
                term::Terminal_render_snapshot_status::OK &&
            snapshot_has_hyperlink_uri(
                after_eviction_snapshot,
                retained_uris[static_cast<std::size_t>(first_surviving_uri)]),
        "flat-ring Phase 5B clear needs no retained hyperlink pre-reclaim cleanup for correctness");

    return ok;
}

bool test_phase_r_repaint_recovery_shift_helper_matches_policy()
{
    bool ok = true;

    term::terminal_repaint_recovery_shift_input_t input;
    input.candidate_active          = true;
    input.primary_buffer_active     = true;
    input.scrollback_rows_unchanged = true;
    input.candidate_rows = {
        QStringLiteral("aa"),
        QStringLiteral("bb"),
        QStringLiteral("cc"),
        QStringLiteral("dd"),
    };
    input.current_rows = {
        QStringLiteral("bb"),
        QStringLiteral("cc"),
        QStringLiteral("dd"),
        QStringLiteral("ee"),
    };
    ok &= check(term::primary_repaint_recovery_shift_rows(input) == 1,
        "Phase R recovery helper accepts distinct shifted repaint");

    input.candidate_rows = {
        QStringLiteral("aa"),
        QStringLiteral("aa"),
        QStringLiteral("aa"),
        QStringLiteral("aa"),
    };
    input.current_rows = {
        QStringLiteral("aa"),
        QStringLiteral("aa"),
        QStringLiteral("aa"),
        QStringLiteral("zz"),
    };
    ok &= check(term::primary_repaint_recovery_shift_rows(input) == 0,
        "Phase R recovery helper suppresses repeated-row ambiguous repaint");

    input.candidate_rows = {
        QString(),
        QStringLiteral("bb"),
        QStringLiteral("cc"),
        QStringLiteral("dd"),
    };
    input.current_rows = {
        QStringLiteral("bb"),
        QStringLiteral("cc"),
        QStringLiteral("dd"),
        QStringLiteral("ee"),
    };
    ok &= check(term::primary_repaint_recovery_shift_rows(input) == 0,
        "Phase R recovery helper suppresses blank-only displaced repaint");

    input.line_start_clear_before_text     = true;
    input.explicit_non_home_repaint_address = true;
    input.candidate_rows = {
        QStringLiteral("aa"),
        QStringLiteral("bb"),
        QStringLiteral("cc"),
        QStringLiteral("dd"),
    };
    ok &= check(term::primary_repaint_recovery_shift_rows(input) == 0,
        "Phase R recovery helper suppresses explicit non-home repaint");

    return ok;
}

bool test_phase_r_primary_repaint_recovery_suppresses_false_positives()
{
    bool ok = true;

    term::Terminal_screen_model repeated_model =
        make_recovery_enabled_primary_repaint_model(4, 8, 8);
    repeated_model.ingest(visible_row_write_stream({
        QByteArrayLiteral("aa"),
        QByteArrayLiteral("aa"),
        QByteArrayLiteral("aa"),
        QByteArrayLiteral("aa"),
    }, false));
    const term::Terminal_screen_model_result repeated_result =
        repeated_model.ingest(visible_row_write_stream({
            QByteArrayLiteral("aa"),
            QByteArrayLiteral("aa"),
            QByteArrayLiteral("aa"),
            QByteArrayLiteral("zz"),
        }, true));
    ok &= check(repeated_model.scrollback_size() == 0 &&
            repeated_result.backing_deltas.empty() &&
            repeated_result.recovery_proposals.empty(),
        "Phase R recovery suppresses repeated-row ambiguous repaint shifts");

    term::Terminal_screen_model blank_model =
        make_recovery_enabled_primary_repaint_model(4, 8, 8);
    blank_model.ingest(visible_row_write_stream({
        QByteArray(),
        QByteArrayLiteral("bb"),
        QByteArrayLiteral("cc"),
        QByteArrayLiteral("dd"),
    }, false));
    const term::Terminal_screen_model_result blank_result =
        blank_model.ingest(visible_row_write_stream({
            QByteArrayLiteral("bb"),
            QByteArrayLiteral("cc"),
            QByteArrayLiteral("dd"),
            QByteArrayLiteral("ee"),
        }, true));
    ok &= check(blank_model.scrollback_size() == 0 &&
            blank_result.backing_deltas.empty() &&
            blank_result.recovery_proposals.empty(),
        "Phase R recovery suppresses blank-only displaced repaint shifts");

    term::Terminal_screen_model resize_model =
        make_recovery_enabled_primary_repaint_model(4, 8, 8);
    resize_model.ingest(visible_row_write_stream({
        QByteArrayLiteral("aa"),
        QByteArrayLiteral("bb"),
        QByteArrayLiteral("cc"),
        QByteArrayLiteral("dd"),
    }, false));
    resize_model.resize(term::terminal_grid_size_t{5, 8});
    const term::Terminal_screen_model_result resize_result =
        resize_model.ingest(visible_row_write_stream({
            QByteArrayLiteral("bb"),
            QByteArrayLiteral("cc"),
            QByteArrayLiteral("dd"),
            QByteArrayLiteral("ee"),
            QByteArrayLiteral("ff"),
        }, true));
    ok &= check(resize_model.scrollback_size() == 0 &&
            resize_result.backing_deltas.empty() &&
            resize_result.recovery_proposals.empty(),
        "Phase R recovery suppresses resize-adjacent repaint shifts");

    return ok;
}

bool test_phase_r_primary_repaint_recovery_toggle_cancels_candidate()
{
    bool ok = true;

    term::Terminal_screen_model model =
        make_recovery_enabled_primary_repaint_model(4, 8, 8);
    model.ingest(visible_row_write_stream({
        QByteArrayLiteral("aa"),
        QByteArrayLiteral("bb"),
        QByteArrayLiteral("cc"),
        QByteArrayLiteral("dd"),
    }, false));

    model.ingest(QByteArrayLiteral("\x1b[?25l\x1b[1;1H\x1b[Kbb"));
    model.set_primary_repaint_recovery_enabled(false);

    const term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral(
            "\x1b[2;1H\x1b[K" "cc"
            "\x1b[3;1H\x1b[K" "dd"
            "\x1b[4;1H\x1b[K" "ee"
            "\x1b[?25h"));

    ok &= check(model.scrollback_size() == 0,
        "Phase R recovery toggle off cancels in-flight candidate");
    ok &= check(result.recovery_proposals.empty(),
        "Phase R recovery toggle off leaves no accepted proposal");
    ok &= check(result.backing_deltas.empty(),
        "Phase R recovery toggle off emits no recovered history delta");
    ok &= check(model.row_text(0) == QStringLiteral("bb") &&
            model.row_text(1) == QStringLiteral("cc") &&
            model.row_text(2) == QStringLiteral("dd") &&
            model.row_text(3) == QStringLiteral("ee"),
        "Phase R recovery toggle off keeps the repainted active grid");

    model.set_primary_repaint_recovery_enabled(true);
    model.ingest(visible_row_write_stream({
        QByteArrayLiteral("cc"),
        QByteArrayLiteral("dd"),
        QByteArrayLiteral("ee"),
        QByteArrayLiteral("ff"),
    }, true));
    ok &= check(model.scrollback_size() == 1,
        "Phase R recovery toggle on re-enables later recovery");

    return ok;
}

bool test_erase_operations_and_wide_damage()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(2, 6);
    term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("abcdef\x1b[2;1Hghijkl\x1b[1;4H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[J"));
    ok     &= check(diagnostic_count(result) == 0, "ED has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("abc"), "ED clears after cursor row");
    ok     &= check(model.row_text(1).isEmpty(), "ED clears following rows");
    ok     &= dirty_rows_equal(result, {0, 1}, "ED suffix dirty rows");
    ok     &= check(snapshot_valid(model, 12U), "ED suffix snapshot validates");

    model = make_model(1, 6);
    model.ingest(QByteArrayLiteral("abcdef\x1b[1;3H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[K"));
    ok     &= check(diagnostic_count(result) == 0, "EL has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("ab"), "EL clears row suffix");
    ok     &= dirty_rows_equal(result, {0}, "EL suffix dirty rows");
    ok     &= check(!result.viewport_changed, "EL suffix does not change viewport");
    ok     &= check(snapshot_valid(model, 14U), "EL suffix snapshot validates");

    model = make_model(2, 6);
    model.ingest(QByteArrayLiteral("abcdef\x1b[2;1Hghijkl\x1b[2;3H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[1J"));
    ok     &= check(diagnostic_count(result) == 0, "ED prefix has no diagnostics");
    ok     &= check(model.row_text(0).isEmpty(), "ED prefix clears earlier rows");
    ok     &= check(model.row_text(1) == QStringLiteral("   jkl"), "ED prefix clears through cursor");
    ok     &= dirty_rows_equal(result, {0, 1}, "ED prefix dirty rows");
    ok     &= check(!result.viewport_changed, "ED prefix does not change viewport");
    ok     &= check(snapshot_valid(model, 15U), "ED prefix snapshot validates");

    model = make_model(2, 6);
    model.ingest(QByteArrayLiteral("abcdef\x1b[2;1Hghijkl\x1b[1;4H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[2J"));
    ok     &= check(diagnostic_count(result) == 0, "ED full has no diagnostics");
    ok     &= check(model.row_text(0).isEmpty() && model.row_text(1).isEmpty(),
        "ED full clears visible screen");
    ok     &= dirty_rows_equal(result, {0, 1}, "ED full dirty rows");
    ok     &= check(!result.viewport_changed, "ED full does not change viewport");
    ok     &= check(snapshot_valid(model, 16U), "ED full snapshot validates");

    model = make_model(2, 4);
    model.ingest(QByteArrayLiteral("1\r\n2\r\n3"));
    ok     &= check(model.scrollback_size() > 0, "ED3 setup has scrollback");
    result  = model.ingest(QByteArrayLiteral("\x1b[3J"));
    ok     &= check(diagnostic_count(result) == 0, "ED3 has no diagnostics");
    ok     &= check(model.scrollback_size() == 0, "ED3 clears scrollback");
    ok     &= check(result.dirty_rows.empty(), "ED3 does not dirty visible rows");
    ok     &= check(result.viewport_changed, "ED3 reports viewport change");
    ok     &= check(snapshot_valid(model, 17U), "ED3 snapshot validates");

    model = make_model(1, 6);
    model.ingest(QByteArrayLiteral("abcdef\x1b[1;4H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[1K"));
    ok     &= check(diagnostic_count(result) == 0, "EL prefix has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("    ef"), "EL prefix clears through cursor");
    ok     &= dirty_rows_equal(result, {0}, "EL prefix dirty rows");
    ok     &= check(snapshot_valid(model, 18U), "EL prefix snapshot validates");

    model = make_model(1, 6);
    model.ingest(QByteArrayLiteral("abcdef\x1b[1;4H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[2K"));
    ok     &= check(diagnostic_count(result) == 0, "EL full has no diagnostics");
    ok     &= check(model.row_text(0).isEmpty(), "EL full clears row");
    ok     &= dirty_rows_equal(result, {0}, "EL full dirty rows");
    ok     &= check(snapshot_valid(model, 19U), "EL full snapshot validates");

    model = make_model(1, 6);
    model.ingest(QByteArrayLiteral("abcdef\x1b[1;3H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[2X"));
    ok     &= check(diagnostic_count(result) == 0, "ECH has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("ab  ef"),
        "ECH clears characters without shifting");
    ok     &= check(model.cursor_position().row == 0 && model.cursor_position().column == 2,
        "ECH does not move the cursor");
    ok     &= dirty_rows_equal(result, {0}, "ECH dirty rows");
    ok     &= check(snapshot_valid(model, 24U), "ECH snapshot validates");

    model   = make_model(1, 5);
    result  = model.ingest(bytes_from_hex("e4b880") + QByteArrayLiteral("AB"));
    ok     &= check(diagnostic_count(result) == 0, "wide setup has no diagnostics");
    result  = model.ingest(QByteArrayLiteral("\x1b[1;2H\x1b[K"));
    ok     &= check(diagnostic_count(result) == 0, "wide EL has no diagnostics");
    ok     &= check(model.row_text(0).isEmpty(), "EL touching continuation clears wide cell");
    ok     &= check(snapshot_valid(model, 13U), "wide erase snapshot validates");

    model = make_model(1, 5);
    model.ingest(bytes_from_hex("e4b880") + QByteArrayLiteral("AB\x1b[1;2H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[X"));
    ok     &= check(diagnostic_count(result) == 0, "wide ECH has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("  AB"),
        "ECH touching continuation clears the full wide cell");
    ok     &= check(snapshot_valid(model, 25U), "wide ECH snapshot validates");

    return ok;
}

bool test_scroll_region_and_origin_mode()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(4, 4);
    term::Terminal_screen_model_result result = model.ingest(
        QByteArrayLiteral("\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333\x1b[4;1H444"));
    ok &= check(diagnostic_count(result) == 0, "scroll setup has no diagnostics");

    model.ingest(QByteArrayLiteral("\x1b[2;3r\x1b[3;1H"));
    result  = model.ingest(QByteArrayLiteral("\n"));
    ok     &= check(diagnostic_count(result) == 0, "DECSTBM region scroll has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("111"), "region scroll preserves row above");
    ok     &= check(model.row_text(1) == QStringLiteral("333"), "region scroll moves region up");
    ok     &= check(model.row_text(2).isEmpty(), "region scroll clears bottom row");
    ok     &= check(model.row_text(3) == QStringLiteral("444"), "region scroll preserves row below");
    ok     &= check(model.scrollback_size() == 0, "partial region scroll does not append scrollback");
    ok     &= dirty_rows_equal(result, {1, 2}, "region scroll dirty rows");
    ok     &= check(snapshot_valid(model, 20U), "region scroll snapshot validates");

    result  = model.ingest(QByteArrayLiteral("\x1b[?6h\x1b[1;1HX"));
    ok     &= check(diagnostic_count(result) == 0, "DECOM enable has no diagnostics");
    ok     &= check(model.row_text(1) == QStringLiteral("X33"), "origin mode addresses inside region");
    ok     &= check(snapshot_valid(model, 21U), "origin mode snapshot validates");

    result  = model.ingest(QByteArrayLiteral("\x1b[?6;9999h\x1b[1;2HY"));
    ok     &= check(diagnostic_count(result) == 1, "mixed DECSET diagnoses unsupported mode");
    ok     &= check(model.row_text(1) == QStringLiteral("XY3"),
        "mixed DECSET still applies supported origin mode");
    ok     &= check(snapshot_valid(model, 23U), "mixed DECSET snapshot validates");

    result  = model.ingest(QByteArrayLiteral("\x1b[?6l\x1b[H Y"));
    ok     &= check(diagnostic_count(result) == 0, "DECOM reset has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral(" Y1"),
        "origin reset addresses absolute home");
    ok     &= check(snapshot_valid(model, 22U), "origin reset snapshot validates");

    return ok;
}

bool test_top_anchored_scroll_region_appends_scrollback()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(4, 4);
    term::Terminal_screen_model_result result = model.ingest(
        QByteArrayLiteral("\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333\x1b[4;1H444"));
    ok &= check(diagnostic_count(result) == 0,
        "top-anchored scroll region setup has no diagnostics");

    model.ingest(QByteArrayLiteral("\x1b[1;2r\x1b[2;1H"));
    result  = model.ingest(QByteArrayLiteral("\n"));
    ok     &= check(diagnostic_count(result) == 0,
        "top-anchored scroll region has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("222"),
        "top-anchored scroll region moves row up");
    ok     &= check(model.row_text(1).isEmpty(),
        "top-anchored scroll region clears region bottom row");
    ok     &= check(model.row_text(2) == QStringLiteral("333"),
        "top-anchored scroll region preserves first row below region");
    ok     &= check(model.row_text(3) == QStringLiteral("444"),
        "top-anchored scroll region preserves second row below region");
    ok     &= check(model.scrollback_size() == 1,
        "top-anchored scroll region appends scrollback");
    ok     &= check(result.viewport_changed,
        "top-anchored scroll region reports viewport changed");
    ok     &= check(result.scrollback_rows == 1,
        "top-anchored scroll region result reports scrollback row count");
    ok     &= check(result.evicted_scrollback_rows == 0,
        "top-anchored scroll region does not evict scrollback");
    const term::Terminal_render_snapshot scrollback_snapshot =
        model.render_snapshot(request_for_model(model, 22U, 1));
    ok &= check(term::validate_render_snapshot(scrollback_snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "top-anchored scrollback snapshot validates");
    ok &= check(snapshot_row_text(scrollback_snapshot, 0) == QStringLiteral("111"),
        "top-anchored scroll region appends the scrolled-out row");
    ok &= check(snapshot_row_text(scrollback_snapshot, 1) == QStringLiteral("222"),
        "top-anchored scrollback snapshot includes shifted active row");
    ok &= check(snapshot_row_text(scrollback_snapshot, 2).isEmpty(),
        "top-anchored scrollback snapshot includes cleared region bottom");
    ok &= check(snapshot_row_text(scrollback_snapshot, 3) == QStringLiteral("333"),
        "top-anchored scrollback snapshot includes row below region");
    ok &= dirty_rows_equal(result, {0, 1},
        "top-anchored scroll region dirty rows");
    ok &= check(snapshot_valid(model, 21U),
        "top-anchored scroll region snapshot validates");

    return ok;
}

bool test_alternate_top_anchored_scroll_region_does_not_append_scrollback()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(4, 4);
    model.ingest(QByteArrayLiteral("AAA\x1b[4;1H\n\x1b[1;1HPPP"));
    const int primary_scrollback_size = model.scrollback_size();
    const term::Terminal_render_snapshot primary_before =
        model.render_snapshot(request_for_model(model, 24U, 1));

    term::Terminal_screen_model_result result = model.ingest(
        QByteArrayLiteral("\x1b[?1049h"
            "\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333\x1b[4;1H444"));
    ok &= check(diagnostic_count(result) == 0,
        "alternate top-anchored setup has no diagnostics");

    model.ingest(QByteArrayLiteral("\x1b[1;2r\x1b[2;1H"));
    result  = model.ingest(QByteArrayLiteral("\n"));
    ok     &= check(diagnostic_count(result) == 0,
        "alternate top-anchored scroll region has no diagnostics");
    ok     &= check(model.active_buffer_id() == term::Terminal_buffer_id::ALTERNATE,
        "alternate top-anchored scroll stays in alternate buffer");
    ok     &= check(model.row_text(0) == QStringLiteral("222"),
        "alternate top-anchored scroll moves row up");
    ok     &= check(model.row_text(1).isEmpty(),
        "alternate top-anchored scroll clears region bottom row");
    ok     &= check(model.scrollback_size() == primary_scrollback_size,
        "alternate top-anchored scroll does not append primary scrollback");
    ok     &= check(!result.viewport_changed,
        "alternate top-anchored scroll does not report viewport changed");
    ok     &= check(result.evicted_scrollback_rows == 0,
        "alternate top-anchored scroll does not evict primary scrollback");
    ok     &= check(snapshot_valid(model, 23U),
        "alternate top-anchored scroll snapshot validates");

    result  = model.ingest(QByteArrayLiteral("\x1b[?1049l"));
    ok     &= check(diagnostic_count(result) == 0,
        "alternate top-anchored leave has no diagnostics");
    ok     &= check(model.active_buffer_id() == term::Terminal_buffer_id::PRIMARY,
        "alternate top-anchored leave returns to primary buffer");
    const term::Terminal_render_snapshot primary_after =
        model.render_snapshot(request_for_model(model, 25U, 1));
    ok &= check(snapshot_row_text(primary_after, 0) ==
        snapshot_row_text(primary_before, 0),
        "alternate top-anchored scroll preserves primary scrollback row");
    ok &= check(snapshot_row_text(primary_after, 1) ==
        snapshot_row_text(primary_before, 1),
        "alternate top-anchored scroll preserves primary visible row");

    return ok;
}

bool test_escape_index_controls()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(3, 5);
    term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("A\x1b" "DB\x1b" "EC"));
    ok &= check(diagnostic_count(result) == 0,
        "ESC IND and NEL have no diagnostics");
    ok &= check(model.row_text(0) == QStringLiteral("A"), "IND preserves current column");
    ok &= check(model.row_text(1) == QStringLiteral(" B"), "IND moves down without CR");
    ok &= check(model.row_text(2) == QStringLiteral("C"), "NEL moves to next line home");
    ok &= check(snapshot_valid(model, 26U), "IND/NEL snapshot validates");

    model = make_model(4, 5);
    model.ingest(
        QByteArrayLiteral("\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333\x1b[4;1H444"
            "\x1b[2;4r\x1b[2;1H"));
    result  = model.ingest(QByteArrayLiteral("\x1b" "M"));
    ok     &= check(diagnostic_count(result) == 0, "RI has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("111"), "RI preserves row above region");
    ok     &= check(model.row_text(1).isEmpty(), "RI blanks scroll-region top row");
    ok     &= check(model.row_text(2) == QStringLiteral("222"), "RI shifts top row down");
    ok     &= check(model.row_text(3) == QStringLiteral("333"), "RI drops bottom row");
    ok     &= check(model.scrollback_size() == 0, "RI does not append scrollback");
    ok     &= check(!result.viewport_changed, "RI does not report viewport change");
    ok     &= dirty_rows_equal(result, {1, 2, 3}, "RI dirty rows");
    ok     &= check(snapshot_valid(model, 27U), "RI snapshot validates");

    result  = model.ingest(QByteArrayLiteral("\x1b[4;1H\x1b" "MZ"));
    ok     &= check(diagnostic_count(result) == 0, "RI away from top margin has no diagnostics");
    ok     &= check(model.row_text(2) == QStringLiteral("Z22"),
        "RI away from top margin moves cursor up");
    ok     &= check(snapshot_valid(model, 28U), "RI cursor movement snapshot validates");

    return ok;
}

bool test_scroll_region_history_insert_after_reverse_index()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(5, 8);
    model.ingest(
        QByteArrayLiteral("\x1b[1;1Htop-one"
            "\x1b[2;1Htop-two"
            "\x1b[3;1Hview"
            "\x1b[4;1Hbelow"
            "\x1b[5;1Hprompt"));

    term::Terminal_screen_model_result result = model.ingest(
        QByteArrayLiteral("\x1b[4;5r\x1b[4;1H\x1b" "M"
            "\x1b[r"
            "\x1b[1;4r\x1b[3;1H\r\nHIST"
            "\x1b[r\x1b[5;1H"));

    ok &= check(diagnostic_count(result) == 0,
        "scroll-region first history insert has no diagnostics");
    ok &= check(model.scrollback_size() == 0,
        "scroll-region first history insert does not append before overflow");
    ok &= check(!result.viewport_changed,
        "scroll-region first history insert does not report viewport changed");
    ok &= check(model.row_text(3) == QStringLiteral("HIST"),
        "scroll-region first history insert writes inserted row");

    result = model.ingest(
        QByteArrayLiteral("\x1b[1;4r\x1b[4;1H\r\nNEXT"
            "\x1b[r\x1b[5;1H"));

    ok &= check(diagnostic_count(result) == 0,
        "scroll-region overflowing history insert has no diagnostics");
    ok &= check(model.scrollback_size() == 1,
        "scroll-region overflowing history insert appends scrollback");
    ok &= check(result.viewport_changed,
        "scroll-region overflowing history insert reports viewport changed");
    const term::Terminal_render_snapshot scrollback_snapshot =
        model.render_snapshot(request_for_model(model, 29U, 1));
    ok &= check(term::validate_render_snapshot(scrollback_snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "scroll-region overflowing history insert snapshot validates");
    ok &= check(snapshot_row_text(scrollback_snapshot, 0) == QStringLiteral("top-one"),
        "scroll-region overflowing history insert keeps scrolled row in scrollback");
    ok &= check(snapshot_row_text(scrollback_snapshot, 1) == QStringLiteral("top-two"),
        "scroll-region overflowing history insert shifts following row into viewport");
    ok &= check(snapshot_row_text(scrollback_snapshot, 4) == QStringLiteral("NEXT"),
        "scroll-region overflowing history insert writes inserted row");

    return ok;
}











bool test_scroll_region_zero_bottom_defaults_to_screen_bottom()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(4, 5);
    model.ingest(
        QByteArrayLiteral("\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333\x1b[4;1H444"));

    const term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("\x1b[1;0r\x1b[4;1H\r\n555"));
    ok &= check(diagnostic_count(result) == 0,
        "zero-bottom scroll region has no diagnostics");
    ok &= check(model.scrollback_size() == 1,
        "zero-bottom scroll region uses screen bottom");
    ok &= check(model.row_text(0) == QStringLiteral("222"),
        "zero-bottom scroll region scrolls full screen");
    ok &= check(model.row_text(3) == QStringLiteral("555"),
        "zero-bottom scroll region writes after scrolling");
    const term::Terminal_render_snapshot scrollback_snapshot =
        model.render_snapshot(request_for_model(model, 34U, 1));
    ok &= check(snapshot_row_text(scrollback_snapshot, 0) == QStringLiteral("111"),
        "zero-bottom scroll region preserves scrolled row");

    return ok;
}

bool test_scroll_up_down_sequences()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(4, 5);
    model.ingest(
        QByteArrayLiteral("\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333\x1b[4;1H444"
            "\x1b[1;3r"));
    term::Terminal_screen_model_result result = model.ingest(QByteArrayLiteral("\x1b[2S"));

    ok &= check(diagnostic_count(result) == 0, "SU has no diagnostics");
    ok &= check(model.scrollback_size() == 2,
        "top-anchored SU appends scrolled rows to primary scrollback");
    ok &= check(result.viewport_changed, "top-anchored SU reports viewport changed");
    ok &= check(model.row_text(0) == QStringLiteral("333"), "SU shifts rows up by count");
    ok &= check(model.row_text(1).isEmpty(), "SU blanks first vacated bottom row");
    ok &= check(model.row_text(2).isEmpty(), "SU blanks second vacated bottom row");
    ok &= check(model.row_text(3) == QStringLiteral("444"), "SU preserves row below region");
    ok &= dirty_rows_equal(result, {0, 1, 2}, "SU dirty rows");
    const term::Terminal_render_snapshot scrollback_snapshot =
        model.render_snapshot(request_for_model(model, 30U, 2));
    ok &= check(snapshot_row_text(scrollback_snapshot, 0) == QStringLiteral("111"),
        "SU first scrolled row is in scrollback");
    ok &= check(snapshot_row_text(scrollback_snapshot, 1) == QStringLiteral("222"),
        "SU second scrolled row is in scrollback");
    ok &= check(snapshot_valid(model, 31U), "SU snapshot validates");

    model = make_model(4, 5);
    model.ingest(
        QByteArrayLiteral("\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333\x1b[4;1H444"
            "\x1b[1;3r"));
    result = model.ingest(QByteArrayLiteral("\x1b[2T"));

    ok &= check(diagnostic_count(result) == 0, "SD has no diagnostics");
    ok &= check(model.scrollback_size() == 0, "SD does not append scrollback");
    ok &= check(!result.viewport_changed, "SD does not report viewport changed");
    ok &= check(model.row_text(0).isEmpty(), "SD blanks first vacated top row");
    ok &= check(model.row_text(1).isEmpty(), "SD blanks second vacated top row");
    ok &= check(model.row_text(2) == QStringLiteral("111"), "SD shifts rows down by count");
    ok &= check(model.row_text(3) == QStringLiteral("444"), "SD preserves row below region");
    ok &= dirty_rows_equal(result, {0, 1, 2}, "SD dirty rows");
    ok &= check(snapshot_valid(model, 32U), "SD snapshot validates");

    result  = model.ingest(QByteArrayLiteral("\x1b[1;2;3;4;5T"));
    ok     &= check(diagnostic_count(result) == 1,
        "XTHIMOUSE-shaped CSI T emits unsupported diagnostic");
    ok     &= check(first_diagnostic(result).code ==
        term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
        "XTHIMOUSE-shaped CSI T diagnostic is unsupported");
    ok     &= check(model.row_text(2) == QStringLiteral("111"),
        "unsupported multi-parameter CSI T does not mutate rows");

    return ok;
}

bool test_blank_fill_operations_use_current_style()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(1, 8);
    term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("\x1b[44mABCD\x1b[41m\x1b[1;2H\x1b[2@"));
    ok &= check(diagnostic_count(result) == 0, "styled ICH has no diagnostics");
    ok &= check(model.row_text(0) == QStringLiteral("A  BCD"),
        "styled ICH inserts blanks without losing text");
    term::Terminal_render_snapshot snapshot = model.render_snapshot(43U);
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled ICH snapshot validates");
    ok &= check_row_background_palette(
        snapshot,
        0,
        1,
        2,
        1U,
        "styled ICH inserted blanks keep current background");

    model     = make_model(1, 8);
    result    = model.ingest(
        QByteArrayLiteral("\x1b[44mABCDEFGH\x1b[41m\x1b[1;3H\x1b[2P"));
    ok       &= check(diagnostic_count(result) == 0, "styled DCH has no diagnostics");
    ok       &= check(model.row_text(0) == QStringLiteral("ABEFGH"),
        "styled DCH deletes cells and fills the right edge");
    snapshot  = model.render_snapshot(44U);
    ok       &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled DCH snapshot validates");
    ok       &= check_row_background_palette(
        snapshot,
        0,
        6,
        2,
        1U,
        "styled DCH right-edge blanks keep current background");

    model     = make_model(4, 5);
    result    = model.ingest(
        QByteArrayLiteral("\x1b[41m\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333"
            "\x1b[4;1H444\x1b[2;4r\x1b[2;1H\x1b[L"));
    ok       &= check(diagnostic_count(result) == 0, "styled IL has no diagnostics");
    ok       &= check(model.row_text(1).isEmpty(), "styled IL blanks the exposed row");
    snapshot  = model.render_snapshot(45U);
    ok       &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled IL snapshot validates");
    ok       &= check_row_background_palette(
        snapshot,
        1,
        0,
        5,
        1U,
        "styled IL exposed row keeps current background");

    model     = make_model(4, 5);
    result    = model.ingest(
        QByteArrayLiteral("\x1b[41m\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333"
            "\x1b[4;1H444\x1b[2;4r\x1b[2;1H\x1b[M"));
    ok       &= check(diagnostic_count(result) == 0, "styled DL has no diagnostics");
    ok       &= check(model.row_text(3).isEmpty(), "styled DL blanks the exposed bottom row");
    snapshot  = model.render_snapshot(46U);
    ok       &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled DL snapshot validates");
    ok       &= check_row_background_palette(
        snapshot,
        3,
        0,
        5,
        1U,
        "styled DL exposed row keeps current background");

    model     = make_model(3, 4);
    result    = model.ingest(
        QByteArrayLiteral("\x1b[41m\x1b[1;1H1111\x1b[2;1H2222"
            "\x1b[3;1H3333\x1b[S"));
    ok       &= check(diagnostic_count(result) == 0, "styled SU has no diagnostics");
    ok       &= check(model.row_text(2).isEmpty(), "styled SU blanks the exposed bottom row");
    snapshot  = model.render_snapshot(47U);
    ok       &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled SU snapshot validates");
    ok       &= check_row_background_palette(
        snapshot,
        2,
        0,
        4,
        1U,
        "styled SU exposed row keeps current background");

    model     = make_model(3, 4);
    result    = model.ingest(
        QByteArrayLiteral("\x1b[41m\x1b[1;1H1111\x1b[2;1H2222"
            "\x1b[3;1H3333\x1b[T"));
    ok       &= check(diagnostic_count(result) == 0, "styled SD has no diagnostics");
    ok       &= check(model.row_text(0).isEmpty(), "styled SD blanks the exposed top row");
    snapshot  = model.render_snapshot(48U);
    ok       &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled SD snapshot validates");
    ok       &= check_row_background_palette(
        snapshot,
        0,
        0,
        4,
        1U,
        "styled SD exposed row keeps current background");

    model     = make_model(2, 4);
    result    = model.ingest(
        QByteArrayLiteral("\x1b[41m\x1b[1;1H1111\x1b[2;1H2222"
            "\x1b[2;1H\x1b" "D"));
    ok       &= check(diagnostic_count(result) == 0, "styled IND scroll has no diagnostics");
    ok       &= check(model.row_text(1).isEmpty(), "styled IND scroll blanks the exposed row");
    snapshot  = model.render_snapshot(49U);
    ok       &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled IND scroll snapshot validates");
    ok       &= check_row_background_palette(
        snapshot,
        1,
        0,
        4,
        1U,
        "styled IND scroll exposed row keeps current background");

    model     = make_model(2, 4);
    result    = model.ingest(
        QByteArrayLiteral("\x1b[41m\x1b[1;1H1111\x1b[2;1H2222"
            "\x1b[1;1H\x1b" "M"));
    ok       &= check(diagnostic_count(result) == 0, "styled RI scroll has no diagnostics");
    ok       &= check(model.row_text(0).isEmpty(), "styled RI scroll blanks the exposed row");
    snapshot  = model.render_snapshot(50U);
    ok       &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled RI scroll snapshot validates");
    ok       &= check_row_background_palette(
        snapshot,
        0,
        0,
        4,
        1U,
        "styled RI scroll exposed row keeps current background");

    model     = make_model(1, 6);
    result    = model.ingest(
        QByteArrayLiteral("\x1b[41m") +
        bytes_from_hex("e4b880") +
        QByteArrayLiteral("AB\x1b[1;2H\x1b[@"));
    ok       &= check(diagnostic_count(result) == 0,
        "styled wide-boundary ICH has no diagnostics");
    ok       &= check(model.row_text(0) == QStringLiteral("   AB"),
        "styled wide-boundary ICH blanks the damaged wide cell");
    snapshot  = model.render_snapshot(51U);
    ok       &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled wide-boundary ICH snapshot validates");
    ok       &= check_row_background_palette(
        snapshot,
        0,
        0,
        3,
        1U,
        "styled wide-boundary ICH blanks keep current background");

    model     = make_model(1, 6);
    result    = model.ingest(
        QByteArrayLiteral("\x1b[41m") +
        bytes_from_hex("e4b880") +
        QByteArrayLiteral("AB\x1b[1;2H\x1b[P"));
    ok       &= check(diagnostic_count(result) == 0,
        "styled wide-boundary DCH has no diagnostics");
    ok       &= check(model.row_text(0) == QStringLiteral(" AB"),
        "styled wide-boundary DCH blanks the damaged wide cell");
    snapshot  = model.render_snapshot(52U);
    ok       &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled wide-boundary DCH snapshot validates");
    ok       &= check_cell_background_palette(
        snapshot,
        0,
        0,
        1U,
        "styled wide-boundary DCH blank keeps current background");

    return ok;
}

bool test_insert_delete_lines_cells_and_tabs()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(1, 8);
    term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("abcdef\x1b[1;3H\x1b[2@"));
    ok &= check(diagnostic_count(result) == 0, "ICH has no diagnostics");
    ok &= check(model.row_text(0) == QStringLiteral("ab  cdef"), "ICH shifts cells right");
    ok &= dirty_rows_equal(result, {0}, "ICH dirty rows");
    ok &= check(snapshot_valid(model, 30U), "ICH snapshot validates");

    model   = make_model(1, 8);
    result  = model.ingest(QByteArrayLiteral("abcdef\x1b[1;3H\x1b[2P"));
    ok     &= check(diagnostic_count(result) == 0, "DCH has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("abef"), "DCH shifts cells left");
    ok     &= dirty_rows_equal(result, {0}, "DCH dirty rows");
    ok     &= check(snapshot_valid(model, 31U), "DCH snapshot validates");

    model = make_model(1, 6);
    model.ingest(bytes_from_hex("e4b880") + QByteArrayLiteral("AB\x1b[1;1H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[@"));
    ok     &= check(diagnostic_count(result) == 0, "wide-preserving ICH has no diagnostics");
    ok     &= check(model.row_text(0) == QString::fromUtf8(" \xe4\xb8\x80" "AB"),
        "ICH before wide glyph preserves shifted wide cell");
    ok     &= check(snapshot_valid(model, 32U), "wide-preserving ICH snapshot validates");

    model = make_model(1, 6);
    model.ingest(bytes_from_hex("e4b880") + QByteArrayLiteral("AB\x1b[1;2H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[@"));
    ok     &= check(diagnostic_count(result) == 0, "wide ICH has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("   AB"), "ICH touching continuation clears wide cell");
    ok     &= check(snapshot_valid(model, 33U), "wide ICH snapshot validates");

    model = make_model(1, 6);
    model.ingest(bytes_from_hex("e4b880") + QByteArrayLiteral("AB\x1b[1;2H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[P"));
    ok     &= check(diagnostic_count(result) == 0, "wide DCH has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral(" AB"),
        "DCH touching continuation clears wide cell");
    ok     &= check(snapshot_valid(model, 34U), "wide DCH snapshot validates");

    model = make_model(1, 4);
    model.ingest(QByteArrayLiteral("AB") + bytes_from_hex("e4b880") + QByteArrayLiteral("\x1b[1;3H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[@"));
    ok     &= check(diagnostic_count(result) == 0, "right-edge wide ICH has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("AB"),
        "ICH clipping right-edge wide glyph clears full span");
    ok     &= check(snapshot_valid(model, 38U), "right-edge wide ICH snapshot validates");

    model = make_model(1, 6);
    model.ingest(bytes_from_hex("e4b880") + QByteArrayLiteral("AB\x1b[1;2H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[2@"));
    ok     &= check(diagnostic_count(result) == 0, "multi-count wide ICH has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("    AB"),
        "multi-count ICH touching continuation clears wide cell");
    ok     &= check(snapshot_valid(model, 39U), "multi-count wide ICH snapshot validates");

    model = make_model(1, 6);
    model.ingest(QByteArrayLiteral("AB") + bytes_from_hex("e4b880") +
        QByteArrayLiteral("C\x1b[1;2H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[P"));
    ok     &= check(diagnostic_count(result) == 0, "wide-shift DCH has no diagnostics");
    ok     &= check(model.row_text(0) == QString::fromUtf8("A\xe4\xb8\x80" "C"),
        "DCH shifts wide glyph left intact");
    {
        const term::Terminal_render_snapshot shifted = model.render_snapshot(40U);
        ok &= check(cell_at(shifted, 0, 1).display_width == 2 &&
            cell_at(shifted, 0, 2).wide_continuation,
            "DCH shifted wide glyph has continuation");
        ok &= check(term::validate_render_snapshot(shifted).status ==
            term::Terminal_render_snapshot_status::OK,
            "wide-shift DCH snapshot validates");
    }

    model = make_model(1, 6);
    model.ingest(QByteArrayLiteral("A") + bytes_from_hex("e4b880") +
        QByteArrayLiteral("BC\x1b[1;2H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[2P"));
    ok     &= check(diagnostic_count(result) == 0, "wide-span DCH has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("ABC"),
        "multi-count DCH deletes full wide span");
    ok     &= check(snapshot_valid(model, 41U), "wide-span DCH snapshot validates");

    model = make_model(4, 5);
    model.ingest(
        QByteArrayLiteral("\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333\x1b[4;1H444"
            "\x1b[2;4r\x1b[2;1H"));
    result  = model.ingest(QByteArrayLiteral("\x1b[L"));
    ok     &= check(diagnostic_count(result) == 0, "IL has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("111"), "IL preserves row above region");
    ok     &= check(model.row_text(1).isEmpty(), "IL blanks cursor row");
    ok     &= check(model.row_text(2) == QStringLiteral("222"), "IL shifts row down");
    ok     &= check(model.row_text(3) == QStringLiteral("333"), "IL drops bottom row");
    ok     &= dirty_rows_equal(result, {1, 2, 3}, "IL dirty rows");
    ok     &= check(snapshot_valid(model, 35U), "IL snapshot validates");

    result  = model.ingest(QByteArrayLiteral("\x1b[2;1H\x1b[M"));
    ok     &= check(diagnostic_count(result) == 0, "DL has no diagnostics");
    ok     &= check(model.row_text(1) == QStringLiteral("222"), "DL shifts row up");
    ok     &= check(model.row_text(3).isEmpty(), "DL blanks bottom row");
    ok     &= dirty_rows_equal(result, {1, 2, 3}, "DL dirty rows");
    ok     &= check(snapshot_valid(model, 36U), "DL snapshot validates");

    model   = make_model(1, 8);
    result  = model.ingest(QByteArrayLiteral("A\tB"));
    ok     &= check(diagnostic_count(result) == 0, "HT has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("A   B"), "HT advances to tab stop");
    ok     &= check(model.render_snapshot(37U).cells.size() == 2U,
        "HT does not materialize intermediate blank cells");

    model   = make_model(1, 8);
    result  = model.ingest(QByteArrayLiteral("A\x1b[3g\tB"));
    ok     &= check(diagnostic_count(result) == 0, "TBC all has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("A      B"),
        "TBC all makes HT use right edge");
    ok     &= check(snapshot_valid(model, 38U), "TBC snapshot validates");

    return ok;
}

bool test_retained_line_provenance_lifecycle()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(3, 4, 4);
    ok &= check_primary_retained_line_ids_unique(
        model,
        "initial retained line ids are allocated and unique");
    for (int logical_row = 0; logical_row < model.grid_size().rows; ++logical_row) {
        ok &= check(primary_retained_line_generation(model, logical_row) == 0U,
            "initial retained line generations are zero");
    }

    model.ingest(QByteArrayLiteral("111\r\n222\r\n333"));
    const std::vector<std::uint64_t> scroll_source_ids = primary_retained_line_ids(model);
    model.ingest(QByteArrayLiteral("\r\n444"));
    ok &= check(model.scrollback_size() == 1, "primary scroll creates one scrollback row");
    ok &= check(primary_retained_line_id(model, 0) == scroll_source_ids[0],
        "scrollback row keeps the scrolled screen row id");
    ok &= check(primary_retained_line_id(model, 1) == scroll_source_ids[1],
        "primary scroll moves retained row ids upward");
    ok &= check(primary_retained_line_id(model, 2) == scroll_source_ids[2],
        "primary scroll preserves the second moved row id");
    ok &= check(!contains_id(scroll_source_ids, primary_retained_line_id(model, 3)),
        "primary scroll allocates a fresh id for the new bottom row");
    ok &= check_primary_retained_line_ids_unique(
        model,
        "primary scroll retained line ids stay unique");

    term::Terminal_screen_model resize_model = make_model(2, 4, 4);
    resize_model.ingest(QByteArrayLiteral("aa\r\nbb"));
    const std::vector<std::uint64_t> pre_resize_ids =
        primary_retained_line_ids(resize_model);
    resize_model.resize({4, 6});
    ok &= check(primary_retained_line_id(resize_model, 0) == pre_resize_ids[0],
        "resize preserves existing top retained row id");
    ok &= check(primary_retained_line_id(resize_model, 1) == pre_resize_ids[1],
        "resize preserves existing second retained row id");
    ok &= check(primary_retained_line_id(resize_model, 2) > max_retained_line_id(pre_resize_ids) &&
        primary_retained_line_id(resize_model, 3) > primary_retained_line_id(resize_model, 2),
        "resize allocates monotonically fresh retained row ids for new rows");
    ok &= check_primary_retained_line_ids_unique(
        resize_model,
        "resize retained line ids stay unique");

    term::Terminal_screen_model streaming_eviction_model = make_model(2, 4, 1);
    streaming_eviction_model.ingest(QByteArrayLiteral("aa\r\nbb\r\ncc"));
    const std::uint64_t streaming_evicted_id =
        primary_retained_line_id(streaming_eviction_model, 0);
    streaming_eviction_model.ingest(QByteArrayLiteral("\r\ndd"));
    ok &= check(streaming_eviction_model.scrollback_size() == 1,
        "streaming eviction keeps the scrollback cap");
    ok &= check(
        !contains_id(primary_retained_line_ids(streaming_eviction_model), streaming_evicted_id),
        "streaming scrollback eviction retires the evicted id");
    ok &= check_primary_retained_line_ids_unique(
        streaming_eviction_model,
        "streaming eviction retained line ids stay unique");

    term::Terminal_screen_model manual_eviction_model = make_model(2, 4, 4);
    manual_eviction_model.ingest(QByteArrayLiteral("aa\r\nbb\r\ncc\r\ndd\r\nee"));
    const std::uint64_t manually_evicted_id =
        primary_retained_line_id(manual_eviction_model, 0);
    manual_eviction_model.set_scrollback_limit(1);
    ok &= check(manual_eviction_model.scrollback_size() == 1,
        "manual scrollback limit applies eviction");
    ok &= check(
        !contains_id(primary_retained_line_ids(manual_eviction_model), manually_evicted_id),
        "manual scrollback eviction retires the evicted id");
    ok &= check_primary_retained_line_ids_unique(
        manual_eviction_model,
        "manual eviction retained line ids stay unique");

    term::Terminal_screen_model zero_scrollback_model = make_model(2, 4, 0);
    zero_scrollback_model.ingest(QByteArrayLiteral("aa\r\nbb"));
    const std::uint64_t zero_discarded_id = primary_retained_line_id(zero_scrollback_model, 0);
    const std::uint64_t zero_moved_id     = primary_retained_line_id(zero_scrollback_model, 1);
    zero_scrollback_model.ingest(QByteArrayLiteral("\r\ncc"));
    ok &= check(zero_scrollback_model.scrollback_size() == 0,
        "zero scrollback retains no discarded rows");
    ok &= check(primary_retained_line_id(zero_scrollback_model, 0) == zero_moved_id,
        "zero scrollback preserves the row moved up");
    ok &= check(primary_retained_line_id(zero_scrollback_model, 1) != zero_discarded_id &&
        primary_retained_line_id(zero_scrollback_model, 1) != zero_moved_id,
        "zero scrollback reused storage receives a fresh id");
    ok &= check_primary_retained_line_ids_unique(
        zero_scrollback_model,
        "zero scrollback retained line ids stay unique");

    term::Terminal_screen_model reverse_index_model = make_model(3, 4, 4);
    reverse_index_model.ingest(
        QByteArrayLiteral("\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333"));
    const std::vector<std::uint64_t> reverse_index_ids =
        primary_retained_line_ids(reverse_index_model);
    reverse_index_model.ingest(QByteArrayLiteral("\x1b[1;1H\x1b" "M"));
    ok &= check(primary_retained_line_id(reverse_index_model, 0) >
            max_retained_line_id(reverse_index_ids),
        "reverse index allocates a fresh id for the exposed top row");
    ok &= check(primary_retained_line_id(reverse_index_model, 1) == reverse_index_ids[0],
        "reverse index moves the previous top row id down");
    ok &= check(primary_retained_line_id(reverse_index_model, 2) == reverse_index_ids[1],
        "reverse index moves retained row ids downward");
    ok &= check(!contains_id(primary_retained_line_ids(reverse_index_model), reverse_index_ids[2]),
        "reverse index retires the displaced bottom row id");
    ok &= check_primary_retained_line_ids_unique(
        reverse_index_model,
        "reverse index retained line ids stay unique");

    term::Terminal_screen_model scroll_down_model = make_model(3, 4, 4);
    scroll_down_model.ingest(
        QByteArrayLiteral("\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333"));
    const std::vector<std::uint64_t> scroll_down_ids =
        primary_retained_line_ids(scroll_down_model);
    scroll_down_model.ingest(QByteArrayLiteral("\x1b[T"));
    ok &= check(primary_retained_line_id(scroll_down_model, 0) >
            max_retained_line_id(scroll_down_ids),
        "SD/T allocates a fresh id for the exposed top row");
    ok &= check(primary_retained_line_id(scroll_down_model, 1) == scroll_down_ids[0],
        "SD/T moves the previous top row id down");
    ok &= check(primary_retained_line_id(scroll_down_model, 2) == scroll_down_ids[1],
        "SD/T moves retained row ids downward");
    ok &= check(!contains_id(primary_retained_line_ids(scroll_down_model), scroll_down_ids[2]),
        "SD/T retires the displaced bottom row id");
    ok &= check_primary_retained_line_ids_unique(
        scroll_down_model,
        "SD/T retained line ids stay unique");

    term::Terminal_screen_model region_model = make_model(4, 4, 4);
    region_model.ingest(
        QByteArrayLiteral("\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333\x1b[4;1H444"));
    const std::vector<std::uint64_t> region_ids = primary_retained_line_ids(region_model);
    region_model.ingest(QByteArrayLiteral("\x1b[2;3r\x1b[3;1H\n"));
    ok &= check(region_model.scrollback_size() == 0,
        "partial scroll region does not append scrollback");
    ok &= check(primary_retained_line_id(region_model, 0) == region_ids[0],
        "partial scroll region preserves the row above the region");
    ok &= check(primary_retained_line_id(region_model, 1) == region_ids[2],
        "partial scroll region preserves moved retained content");
    ok &= check(primary_retained_line_id(region_model, 2) != region_ids[1] &&
        primary_retained_line_id(region_model, 2) != region_ids[2],
        "partial scroll region replaces the exposed blank row id");
    ok &= check(primary_retained_line_id(region_model, 3) == region_ids[3],
        "partial scroll region preserves the row below the region");
    ok &= check(!contains_id(primary_retained_line_ids(region_model), region_ids[1]),
        "partial scroll region retires the displaced row id");
    ok &= check_primary_retained_line_ids_unique(
        region_model,
        "partial scroll region retained line ids stay unique");

    term::Terminal_screen_model insert_delete_lines_model = make_model(4, 4, 4);
    insert_delete_lines_model.ingest(
        QByteArrayLiteral("\x1b[1;1H111\x1b[2;1H222\x1b[3;1H333\x1b[4;1H444"
            "\x1b[2;4r\x1b[2;1H"));
    const std::vector<std::uint64_t> pre_insert_line_ids =
        primary_retained_line_ids(insert_delete_lines_model);
    insert_delete_lines_model.ingest(QByteArrayLiteral("\x1b[L"));
    const std::vector<std::uint64_t> post_insert_line_ids =
        primary_retained_line_ids(insert_delete_lines_model);
    ok &= check(post_insert_line_ids[0] == pre_insert_line_ids[0],
        "IL preserves the row above the scroll region");
    ok &= check(post_insert_line_ids[1] > max_retained_line_id(pre_insert_line_ids),
        "IL allocates a fresh id for the inserted row");
    ok &= check(post_insert_line_ids[2] == pre_insert_line_ids[1],
        "IL moves retained row ids down within the region");
    ok &= check(post_insert_line_ids[3] == pre_insert_line_ids[2],
        "IL preserves the second shifted retained row id");
    ok &= check(!contains_id(post_insert_line_ids, pre_insert_line_ids[3]),
        "IL retires the displaced bottom row id");

    insert_delete_lines_model.ingest(QByteArrayLiteral("\x1b[2;1H\x1b[M"));
    const std::vector<std::uint64_t> post_delete_line_ids =
        primary_retained_line_ids(insert_delete_lines_model);
    ok &= check(post_delete_line_ids[0] == post_insert_line_ids[0],
        "DL preserves the row above the scroll region");
    ok &= check(post_delete_line_ids[1] == post_insert_line_ids[2],
        "DL moves retained row ids up within the region");
    ok &= check(post_delete_line_ids[2] == post_insert_line_ids[3],
        "DL preserves the second shifted retained row id");
    ok &= check(post_delete_line_ids[3] > max_retained_line_id(post_insert_line_ids),
        "DL allocates a fresh id for the exposed bottom row");
    ok &= check(!contains_id(post_delete_line_ids, post_insert_line_ids[1]),
        "DL retires the deleted row id");
    ok &= check_primary_retained_line_ids_unique(
        insert_delete_lines_model,
        "IL/DL retained line ids stay unique");

    term::Terminal_screen_model clear_scrollback_model = make_model(2, 4, 4);
    clear_scrollback_model.ingest(QByteArrayLiteral("aa\r\nbb\r\ncc\r\ndd"));
    const int purged_scrollback_rows = clear_scrollback_model.scrollback_size();
    std::vector<std::uint64_t> purged_ids;
    purged_ids.reserve(static_cast<std::size_t>(purged_scrollback_rows));
    for (int logical_row = 0; logical_row < purged_scrollback_rows; ++logical_row) {
        purged_ids.push_back(primary_retained_line_id(clear_scrollback_model, logical_row));
    }
    const term::Terminal_screen_model_result clear_scrollback_result =
        clear_scrollback_model.ingest(QByteArrayLiteral("\x1b[3J"));
    ok &= check(clear_scrollback_model.scrollback_size() == 0,
        "ED3 clears primary scrollback");
    ok &= check(clear_scrollback_result.evicted_scrollback_rows == purged_scrollback_rows,
        "ED3 reports evicted primary scrollback rows");
    for (std::uint64_t purged_id : purged_ids) {
        ok &= check(!contains_id(primary_retained_line_ids(clear_scrollback_model), purged_id),
            "clear scrollback retires purged ids");
    }
    ok &= check_primary_retained_line_ids_unique(
        clear_scrollback_model,
        "clear scrollback retained line ids stay unique");

    term::Terminal_screen_model alternate_model = make_model(2, 4, 4);
    alternate_model.ingest(QByteArrayLiteral("P"));
    const std::uint64_t primary_top_id    = primary_retained_line_id(alternate_model, 0);
    const std::uint64_t primary_bottom_id = primary_retained_line_id(alternate_model, 1);
    alternate_model.ingest(QByteArrayLiteral("\x1b[?47h"));
    const std::uint64_t alternate_top_id    = alternate_retained_line_id(alternate_model, 0);
    const std::uint64_t alternate_bottom_id = alternate_retained_line_id(alternate_model, 1);
    ok &= check(alternate_top_id != primary_top_id && alternate_top_id != primary_bottom_id,
        "alternate retained line ids do not collide with primary ids");
    alternate_model.ingest(QByteArrayLiteral("ALT"));
    ok &= check(alternate_retained_line_id(alternate_model, 0) == alternate_top_id,
        "alternate writes preserve the active alternate row id");
    alternate_model.ingest(QByteArrayLiteral("\x1b[?47l"));
    ok &= check(primary_retained_line_id(alternate_model, 0) == primary_top_id &&
        primary_retained_line_id(alternate_model, 1) == primary_bottom_id,
        "leaving alternate restores saved primary provenance");
    alternate_model.ingest(QByteArrayLiteral("\x1b[?47h"));
    ok &= check(alternate_retained_line_id(alternate_model, 0) == alternate_top_id &&
        alternate_retained_line_id(alternate_model, 1) == alternate_bottom_id,
        "re-entering alternate restores saved alternate provenance");
    alternate_model.ingest(QByteArrayLiteral("\x1b[?47l\x1b[?1047h"));
    ok &= check(alternate_retained_line_id(alternate_model, 0) != alternate_top_id &&
        alternate_retained_line_id(alternate_model, 1) != alternate_bottom_id,
        "clearing alternate replaces alternate retained line ids");
    ok &= check_primary_retained_line_ids_unique(
        alternate_model,
        "alternate round trip primary retained line ids stay unique");
    ok &= check_alternate_screen_retained_line_ids_unique(
        alternate_model,
        "alternate round trip alternate retained line ids stay unique");

    return ok;
}

bool test_recovery_disabled_non_scroll_sources_do_not_grow_retained_history()
{
    bool ok = true;

    Scrollback_delta_observer observer(false);
    auto check_observed_boundary_no_retained_history_growth = [&](
        term::Terminal_screen_model&          model,
        Primary_backing_boundary              boundary_annotation,
        Scrollback_delta_operation_annotation operation_annotation,
        term::Terminal_buffer_id              expected_active_buffer_before,
        term::Terminal_buffer_id              expected_active_buffer_after,
        const char*                           label,
        bool                                  expect_no_backing_deltas,
        const auto&                           operation) {
        bool source_ok = true;

        const std::size_t pre_retained_line_count = primary_retained_line_ids(model).size();
        const auto observation = observer.observe(
            model,
            boundary_annotation,
            operation_annotation,
            operation);
        const std::size_t post_retained_line_count = primary_retained_line_ids(model).size();

        source_ok &= check(
            observation.boundary_annotation == boundary_annotation,
            label);
        source_ok &= check(
            observation.operation_annotation == operation_annotation,
            label);
        source_ok &= check(
            observation.active_buffer_before == expected_active_buffer_before &&
                observation.active_buffer_after == expected_active_buffer_after,
            label);
        source_ok &= check(!observation.recovery_enabled_annotation, label);
        source_ok &= check(scrollback_rows_delta(observation) == 0, label);
        source_ok &= check(observation.evicted_scrollback_rows == 0, label);
        if (expect_no_backing_deltas) {
            source_ok &= check(observation.backing_deltas.empty(), label);
        }
        source_ok &= check(post_retained_line_count == pre_retained_line_count, label);
        return source_ok;
    };

    auto check_observed_no_retained_history_growth = [&](
        term::Terminal_screen_model&          model,
        Scrollback_delta_operation_annotation operation_annotation,
        const char*                           label,
        const auto&                           operation) {
        return check_observed_boundary_no_retained_history_growth(
            model,
            Primary_backing_boundary::INGEST,
            operation_annotation,
            term::Terminal_buffer_id::PRIMARY,
            term::Terminal_buffer_id::PRIMARY,
            label,
            true,
            operation);
    };

    term::Terminal_screen_model resize_model =
        make_recovery_disabled_primary_backing_model(3, 5, 5);
    resize_model.ingest(QByteArrayLiteral("abc"));
    ok &= check_observed_boundary_no_retained_history_growth(
        resize_model,
        Primary_backing_boundary::RESIZE,
        Scrollback_delta_operation_annotation::RESIZE,
        term::Terminal_buffer_id::PRIMARY,
        term::Terminal_buffer_id::PRIMARY,
        "same-row resize does not grow retained history",
        false,
        [&] {
            return resize_model.resize(term::terminal_grid_size_t{3, 7});
        });
    ok &= check(resize_model.grid_size().rows == 3 &&
        resize_model.grid_size().columns == 7,
        "same-row resize applies requested grid");

    term::Terminal_screen_model alternate_boundary_model =
        make_recovery_disabled_primary_backing_model(2, 4, 4);
    alternate_boundary_model.ingest(QByteArrayLiteral("P"));
    ok &= check_observed_boundary_no_retained_history_growth(
        alternate_boundary_model,
        Primary_backing_boundary::ALTERNATE_ENTER,
        Scrollback_delta_operation_annotation::ALTERNATE_ENTER_LEAVE,
        term::Terminal_buffer_id::PRIMARY,
        term::Terminal_buffer_id::ALTERNATE,
        "alternate enter does not grow retained history",
        false,
        [&] {
            return alternate_boundary_model.ingest(QByteArrayLiteral("\x1b[?47h"));
        });
    ok &= check(alternate_boundary_model.active_buffer_id() ==
        term::Terminal_buffer_id::ALTERNATE,
        "alternate enter switches to alternate buffer");
    ok &= check_observed_boundary_no_retained_history_growth(
        alternate_boundary_model,
        Primary_backing_boundary::ALTERNATE_LEAVE,
        Scrollback_delta_operation_annotation::ALTERNATE_ENTER_LEAVE,
        term::Terminal_buffer_id::ALTERNATE,
        term::Terminal_buffer_id::PRIMARY,
        "alternate leave does not grow retained history",
        false,
        [&] {
            return alternate_boundary_model.ingest(QByteArrayLiteral("\x1b[?47l"));
        });
    ok &= check(alternate_boundary_model.active_buffer_id() ==
        term::Terminal_buffer_id::PRIMARY,
        "alternate leave returns to primary buffer");

    term::Terminal_screen_model cursor_model =
        make_recovery_disabled_primary_backing_model(3, 5, 5);
    cursor_model.ingest(QByteArrayLiteral("abc"));
    ok &= check_observed_no_retained_history_growth(
        cursor_model,
        Scrollback_delta_operation_annotation::REPAINT,
        "cursor movement repaint does not grow retained history",
        [&] {
            return cursor_model.ingest(QByteArrayLiteral("\x1b[3;4H"));
        });
    ok &= check(cursor_model.cursor_position().row == 2 &&
        cursor_model.cursor_position().column == 3,
        "cursor movement repaint moves the cursor");

    term::Terminal_screen_model erase_line_model =
        make_recovery_disabled_primary_backing_model(3, 6, 5);
    erase_line_model.ingest(QByteArrayLiteral("abcdef"));
    ok &= check_observed_no_retained_history_growth(
        erase_line_model,
        Scrollback_delta_operation_annotation::REPAINT,
        "EL visible clear does not grow retained history",
        [&] {
            return erase_line_model.ingest(QByteArrayLiteral("\x1b[1;3H\x1b[K"));
        });
    ok &= check(erase_line_model.row_text(0) == QStringLiteral("ab"),
        "EL visible clear erases from the cursor to end of row");

    term::Terminal_screen_model erase_display_model =
        make_recovery_disabled_primary_backing_model(3, 6, 5);
    erase_display_model.ingest(
        QByteArrayLiteral("abcd\x1b[2;1Hefgh\x1b[3;1Hijkl"));
    ok &= check_observed_no_retained_history_growth(
        erase_display_model,
        Scrollback_delta_operation_annotation::REPAINT,
        "ED visible clear does not grow retained history",
        [&] {
            return erase_display_model.ingest(QByteArrayLiteral("\x1b[2;3H\x1b[J"));
        });
    ok &= check(erase_display_model.row_text(0) == QStringLiteral("abcd"),
        "ED visible clear preserves rows before the cursor");
    ok &= check(erase_display_model.row_text(1) == QStringLiteral("ef"),
        "ED visible clear erases the current row from the cursor");
    ok &= check(erase_display_model.row_text(2).isEmpty(),
        "ED visible clear erases following rows");

    term::Terminal_screen_model overwrite_model =
        make_recovery_disabled_primary_backing_model(2, 5, 5);
    overwrite_model.ingest(QByteArrayLiteral("abcde"));
    ok &= check_observed_no_retained_history_growth(
        overwrite_model,
        Scrollback_delta_operation_annotation::REPAINT,
        "text overwrite does not grow retained history",
        [&] {
            return overwrite_model.ingest(QByteArrayLiteral("\x1b[1;3HX"));
        });
    ok &= check(overwrite_model.row_text(0) == QStringLiteral("abXde"),
        "text overwrite changes an existing visible cell");

    term::Terminal_screen_model non_top_region_model =
        make_recovery_disabled_primary_backing_model(4, 5, 5);
    non_top_region_model.ingest(
        QByteArrayLiteral("\x1b[2;3r\x1b[1;1Htop\x1b[2;1Hone\x1b[3;1Htwo\x1b[4;1Hbot"));
    ok &= check_observed_no_retained_history_growth(
        non_top_region_model,
        Scrollback_delta_operation_annotation::SCROLL_REGION_WITHOUT_HISTORY_APPEND,
        "non-top scroll region does not grow retained history",
        [&] {
            return non_top_region_model.ingest(QByteArrayLiteral("\x1b[3;1H\n"));
        });
    ok &= check(non_top_region_model.row_text(0) == QStringLiteral("top"),
        "non-top scroll region preserves rows above the region");
    ok &= check(non_top_region_model.row_text(1) == QStringLiteral("two"),
        "non-top scroll region scrolls region content upward");
    ok &= check(non_top_region_model.row_text(2).isEmpty(),
        "non-top scroll region blanks the exposed bottom margin");
    ok &= check(non_top_region_model.row_text(3) == QStringLiteral("bot"),
        "non-top scroll region preserves rows below the region");

    return ok;
}

bool test_recovery_disabled_chunk_split_invariance_for_non_scroll_sources()
{
    bool ok = true;

    auto run_non_scroll_fixture = [&](const std::vector<QByteArray>& chunks,
        bool resize_before_chunks,
        const char* label) {
        term::Terminal_screen_model model =
            make_recovery_disabled_primary_backing_model(3, 8, 8);
        term::Terminal_screen_model_result result =
            model.ingest(QByteArrayLiteral("aaa\r\nbbb\r\nccc"));
        ok &= check(diagnostic_count(result) == 0, label);
        ok &= check(model.scrollback_size() == 0, label);
        if (resize_before_chunks) {
            result = model.resize(term::terminal_grid_size_t{3, 10});
            ok &= check(result.scrollback_rows == 0, label);
            ok &= check(model.scrollback_size() == 0, label);
        }

        for (const QByteArray& chunk : chunks) {
            result = model.ingest(chunk);
            ok &= check(diagnostic_count(result) == 0, label);
        }
        ok &= check(model.scrollback_size() == 0, label);
        return primary_backing_model_summary(model);
    };

    auto check_chunk_split = [&](
        const std::vector<QByteArray>& combined_chunks,
        const std::vector<QByteArray>& split_chunks,
        bool                           resize_before_chunks,
        const char*                    label)
    {
        const primary_backing_model_summary_t combined =
            run_non_scroll_fixture(combined_chunks, resize_before_chunks, label);
        const primary_backing_model_summary_t split =
            run_non_scroll_fixture(split_chunks, resize_before_chunks, label);
        ok &= check(primary_backing_model_summaries_equal(combined, split), label);
    };

    check_chunk_split(
        {
            QByteArrayLiteral("\x1b[Hxxx\x1b[K\r\nyyy\x1b[K\r\nzzz\x1b[K"),
        },
        {
            QByteArrayLiteral("\x1b[Hxxx"),
            QByteArrayLiteral("\x1b[K\r\n"),
            QByteArrayLiteral("yyy\x1b[K\r\nzzz"),
            QByteArrayLiteral("\x1b[K"),
        },
        false,
        "cursor-home repaint chunk split is invariant with recovery disabled");

    check_chunk_split(
        {
            QByteArrayLiteral("\x1b[Hwide\x1b[K\r\nresize\x1b[K"),
        },
        {
            QByteArrayLiteral("\x1b[H"),
            QByteArrayLiteral("wide\x1b[K\r\n"),
            QByteArrayLiteral("resize"),
            QByteArrayLiteral("\x1b[K"),
        },
        true,
        "resize-adjacent repaint chunk split is invariant with recovery disabled");

    check_chunk_split(
        {
            QByteArrayLiteral("\x1b[?2026h\x1b[Hheld\x1b[K\r\npublic\x1b[K\x1b[?2026l"),
        },
        {
            QByteArrayLiteral("\x1b[?2026h\x1b[H"),
            QByteArrayLiteral("held\x1b[K"),
            QByteArrayLiteral("\r\npublic\x1b[K"),
            QByteArrayLiteral("\x1b[?2026l"),
        },
        false,
        "synchronized-output repaint chunk split is invariant with recovery disabled");

    return ok;
}

bool test_recovery_disabled_scrollback_limit_changes_do_not_grow_retained_history()
{
    bool ok = true;

    Scrollback_delta_observer observer(false);

    term::Terminal_screen_model discard_model =
        make_recovery_disabled_primary_backing_model(2, 4, 0);
    const term::Terminal_screen_model_result discard_result =
        discard_model.ingest(QByteArrayLiteral("aa\r\nbb\r\ncc"));
    ok &= check(discard_result.evicted_scrollback_rows == 1 &&
        discard_model.scrollback_size() == 0,
        "zero scrollback limit preserves scalar discarded-row compatibility");
    ok &= check(discard_result.backing_deltas.size() == 1U &&
            primary_backing_delta_matches(
                discard_result.backing_deltas.front(),
                term::Terminal_backing_delta_kind::PRIMARY_HISTORY_DISCARDED,
                0,
                0,
                0,
                0,
                1),
        "zero scrollback limit records discarded rows separately from eviction");
    ok &= check(evicted_scrollback_rows_in_backing_deltas(
            discard_result.backing_deltas) == 0 &&
        discarded_scrollback_rows_in_backing_deltas(
            discard_result.backing_deltas) == 1,
        "zero scrollback limit does not mislabel discarded rows as stored-history eviction");

    term::Terminal_screen_model increased_limit_model =
        make_recovery_disabled_primary_backing_model(2, 4, 2);
    increased_limit_model.ingest(QByteArrayLiteral("aa\r\nbb\r\ncc\r\ndd"));
    const int increased_limit_rows_before = increased_limit_model.scrollback_size();
    const std::vector<std::uint64_t> increased_limit_ids_before =
        primary_retained_line_ids(increased_limit_model);

    ok &= check(increased_limit_rows_before == 2,
        "scrollback limit increase fixture starts at the original limit");

    const auto increased_limit = observer.observe(
        increased_limit_model,
        Primary_backing_boundary::SCROLLBACK_LIMIT_CHANGE,
        Scrollback_delta_operation_annotation::RESIZE,
        [&] {
            return increased_limit_model.set_scrollback_limit(4);
        });
    ok &= check(increased_limit.boundary_annotation ==
            Primary_backing_boundary::SCROLLBACK_LIMIT_CHANGE &&
        increased_limit.classification_annotation ==
            Primary_backing_observation_classification::TEST_ONLY &&
        !increased_limit.recovery_enabled_annotation,
        "scrollback limit increase is observed as a recovery-disabled test-only boundary");
    ok &= check(increased_limit.active_buffer_before == term::Terminal_buffer_id::PRIMARY &&
        increased_limit.active_buffer_after == term::Terminal_buffer_id::PRIMARY,
        "scrollback limit increase stays on the primary buffer");
    ok &= check(scrollback_rows_delta(increased_limit) == 0,
        "scrollback limit increase does not grow retained history");
    ok &= check(increased_limit.evicted_scrollback_rows == 0,
        "scrollback limit increase does not evict retained history");
    ok &= single_primary_backing_delta_equal(
        increased_limit.backing_deltas,
        term::Terminal_backing_delta_kind::BACKING_UNCHANGED,
        increased_limit_rows_before,
        increased_limit_rows_before,
        0,
        0,
        "scrollback limit increase records an explicit storage no-op delta");
    ok &= check(increased_limit.result_scrollback_rows == increased_limit_rows_before &&
        increased_limit_model.scrollback_size() == increased_limit_rows_before,
        "scrollback limit increase reports unchanged retained history rows");
    ok &= check(primary_retained_line_ids(increased_limit_model) ==
        increased_limit_ids_before,
        "scrollback limit increase preserves retained line identities");

    const term::Terminal_screen_model_result equal_limit =
        increased_limit_model.set_scrollback_limit(4);
    ok &= single_primary_backing_delta_equal(
        equal_limit.backing_deltas,
        term::Terminal_backing_delta_kind::BACKING_UNCHANGED,
        increased_limit_rows_before,
        increased_limit_rows_before,
        0,
        0,
        "unchanged scrollback limit records an explicit storage no-op delta");

    constexpr int expected_increased_scrollback_rows = 4;

    const term::Terminal_screen_model_result append_after_increase =
        increased_limit_model.ingest(QByteArrayLiteral("\r\nee"));
    ok &= check(append_after_increase.evicted_scrollback_rows == 0 &&
        increased_limit_model.scrollback_size() == increased_limit_rows_before + 1,
        "increased scrollback limit accepts a later terminal-scroll append");
    ok &= single_primary_backing_delta_equal(
        append_after_increase.backing_deltas,
        term::Terminal_backing_delta_kind::PRIMARY_HISTORY_APPENDED,
        increased_limit_rows_before,
        increased_limit_rows_before + 1,
        1,
        0,
        "append after limit increase records the backing append delta");

    const term::Terminal_screen_model_result append_to_increased_limit =
        increased_limit_model.ingest(QByteArrayLiteral("\r\nff"));
    ok &= check(append_to_increased_limit.evicted_scrollback_rows == 0 &&
        increased_limit_model.scrollback_size() == expected_increased_scrollback_rows,
        "later terminal-scroll append reaches the increased scrollback limit");
    ok &= single_primary_backing_delta_equal(
        append_to_increased_limit.backing_deltas,
        term::Terminal_backing_delta_kind::PRIMARY_HISTORY_APPENDED,
        increased_limit_rows_before + 1,
        expected_increased_scrollback_rows,
        1,
        0,
        "append to increased limit records the backing append delta");

    const term::Terminal_screen_model_result append_past_increased_limit =
        increased_limit_model.ingest(QByteArrayLiteral("\r\ngg"));
    ok &= check(append_past_increased_limit.evicted_scrollback_rows == 1 &&
        increased_limit_model.scrollback_size() == expected_increased_scrollback_rows,
        "later terminal-scroll append keeps the increased scrollback limit capped");
    ok &= check(append_past_increased_limit.backing_deltas.size() == 2U &&
            primary_backing_delta_matches(
                append_past_increased_limit.backing_deltas[0],
                term::Terminal_backing_delta_kind::PRIMARY_HISTORY_APPENDED,
                expected_increased_scrollback_rows,
                expected_increased_scrollback_rows + 1,
                1,
                0) &&
            primary_backing_delta_matches(
                append_past_increased_limit.backing_deltas[1],
                term::Terminal_backing_delta_kind::PRIMARY_HISTORY_EVICTED,
                expected_increased_scrollback_rows + 1,
                expected_increased_scrollback_rows,
                0,
                1),
        "append past increased limit records append and eviction backing deltas");

    term::Terminal_screen_model shrunk_limit_model =
        make_recovery_disabled_primary_backing_model(2, 4, 4);
    shrunk_limit_model.ingest(QByteArrayLiteral("aa\r\nbb\r\ncc\r\ndd\r\nee"));
    const int shrunk_limit_rows_before = shrunk_limit_model.scrollback_size();
    const std::vector<std::uint64_t> shrunk_limit_ids_before =
        primary_retained_line_ids(shrunk_limit_model);

    constexpr int expected_shrunk_scrollback_rows = 1;
    ok &= check(shrunk_limit_rows_before == 3,
        "scrollback limit shrink fixture starts above the smaller limit");

    const auto shrunk_limit = observer.observe(
        shrunk_limit_model,
        Primary_backing_boundary::SCROLLBACK_LIMIT_CHANGE,
        Scrollback_delta_operation_annotation::RESIZE,
        [&] {
            return shrunk_limit_model.set_scrollback_limit(
                expected_shrunk_scrollback_rows);
        });
    const int expected_evicted_scrollback_rows =
        shrunk_limit_rows_before - expected_shrunk_scrollback_rows;
    ok &= check(shrunk_limit.boundary_annotation ==
            Primary_backing_boundary::SCROLLBACK_LIMIT_CHANGE &&
        shrunk_limit.classification_annotation ==
            Primary_backing_observation_classification::TEST_ONLY &&
        !shrunk_limit.recovery_enabled_annotation,
        "scrollback limit shrink is observed as a recovery-disabled test-only boundary");
    ok &= check(shrunk_limit.active_buffer_before == term::Terminal_buffer_id::PRIMARY &&
        shrunk_limit.active_buffer_after == term::Terminal_buffer_id::PRIMARY,
        "scrollback limit shrink stays on the primary buffer");
    ok &= check(scrollback_rows_delta(shrunk_limit) ==
            -expected_evicted_scrollback_rows &&
        shrunk_limit.result_scrollback_rows == expected_shrunk_scrollback_rows &&
        shrunk_limit_model.scrollback_size() == expected_shrunk_scrollback_rows,
        "scrollback limit shrink applies the smaller retention limit");
    ok &= check(shrunk_limit.evicted_scrollback_rows ==
        expected_evicted_scrollback_rows,
        "scrollback limit shrink reports evicted history separately");
    ok &= check(appended_scrollback_rows_in_backing_deltas(
            shrunk_limit.backing_deltas) == 0 &&
        evicted_scrollback_rows_in_backing_deltas(shrunk_limit.backing_deltas) ==
            expected_evicted_scrollback_rows,
        "scrollback limit shrink records eviction-only backing deltas");

    const std::vector<std::uint64_t> shrunk_limit_ids_after =
        primary_retained_line_ids(shrunk_limit_model);
    const std::vector<std::uint64_t> expected_shrunk_limit_ids_after(
        shrunk_limit_ids_before.begin() + expected_evicted_scrollback_rows,
        shrunk_limit_ids_before.end());
    ok &= check(shrunk_limit_ids_after == expected_shrunk_limit_ids_after,
        "scrollback limit shrink evicts the oldest retained history rows");
    ok &= check(scrollback_rows_delta(shrunk_limit) <= 0,
        "scrollback limit shrink is not retained-history growth");

    const term::Terminal_screen_model_result first_append_after_shrink =
        shrunk_limit_model.ingest(QByteArrayLiteral("\r\nff"));
    ok &= check(first_append_after_shrink.evicted_scrollback_rows == 1 &&
        shrunk_limit_model.scrollback_size() == expected_shrunk_scrollback_rows,
        "shrunk scrollback limit evicts on a later terminal-scroll append");
    ok &= check(appended_scrollback_rows_in_backing_deltas(
            first_append_after_shrink.backing_deltas) == 1 &&
        evicted_scrollback_rows_in_backing_deltas(
            first_append_after_shrink.backing_deltas) == 1,
        "first append after shrink records append and eviction backing deltas");

    const term::Terminal_screen_model_result second_append_after_shrink =
        shrunk_limit_model.ingest(QByteArrayLiteral("\r\ngg"));
    ok &= check(second_append_after_shrink.evicted_scrollback_rows == 1 &&
        shrunk_limit_model.scrollback_size() == expected_shrunk_scrollback_rows,
        "shrunk scrollback limit remains enforced after later terminal-scroll appends");
    ok &= check(appended_scrollback_rows_in_backing_deltas(
            second_append_after_shrink.backing_deltas) == 1 &&
        evicted_scrollback_rows_in_backing_deltas(
            second_append_after_shrink.backing_deltas) == 1,
        "second append after shrink records append and eviction backing deltas");

    return ok;
}

bool test_retained_line_content_generation_mutations()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(1, 8);
    const std::uint64_t retained_line_id = primary_retained_line_id(model, 0);
    const std::uint64_t initial_generation =
        primary_retained_line_generation(model, 0);
    model.ingest(QByteArrayLiteral("ABC"));
    ok &= check(primary_retained_line_id(model, 0) == retained_line_id,
        "content writes preserve retained line id");
    ok &= check(primary_retained_line_generation(model, 0) == initial_generation + 1U,
        "printable span increments generation once when row content changes");

    const std::uint64_t after_write_generation =
        primary_retained_line_generation(model, 0);
    model.ingest(QByteArrayLiteral("\rABC"));
    ok &= check(primary_retained_line_generation(model, 0) == after_write_generation,
        "idempotent printable write does not increment generation");

    model.ingest(QByteArrayLiteral("\x1b[1;5H"));
    ok &= check(primary_retained_line_generation(model, 0) == after_write_generation,
        "cursor-only update does not increment generation");

    term::Terminal_screen_model shift_model = make_model(1, 8);
    shift_model.ingest(QByteArrayLiteral("ABCD"));
    std::uint64_t generation = primary_retained_line_generation(shift_model, 0);
    shift_model.ingest(QByteArrayLiteral("\x1b[1;2H\x1b[@"));
    ok &= check(primary_retained_line_generation(shift_model, 0) == generation + 1U,
        "insert-cell content shift increments generation");
    generation = primary_retained_line_generation(shift_model, 0);
    shift_model.ingest(QByteArrayLiteral("\x1b[P"));
    ok &= check(primary_retained_line_generation(shift_model, 0) == generation + 1U,
        "delete-cell content shift increments generation");

    term::Terminal_screen_model erase_model = make_model(1, 4);
    generation = primary_retained_line_generation(erase_model, 0);
    erase_model.ingest(QByteArrayLiteral("\x1b[X"));
    ok &= check(primary_retained_line_generation(erase_model, 0) == generation,
        "erase-character on already blank content does not increment generation");
    erase_model.ingest(QByteArrayLiteral("AB"));
    generation = primary_retained_line_generation(erase_model, 0);
    erase_model.ingest(QByteArrayLiteral("\x1b[1;1H\x1b[X"));
    ok &= check(primary_retained_line_generation(erase_model, 0) == generation + 1U,
        "erase-character changing content increments generation");
    generation = primary_retained_line_generation(erase_model, 0);
    erase_model.ingest(QByteArrayLiteral("\x1b[1;1H\x1b[X"));
    ok &= check(primary_retained_line_generation(erase_model, 0) == generation,
        "idempotent erase-character does not increment generation");

    term::Terminal_screen_model wide_model = make_model(1, 4);
    wide_model.ingest(bytes_from_hex("e4b880"));
    generation = primary_retained_line_generation(wide_model, 0);
    wide_model.ingest(QByteArrayLiteral("\rA"));
    ok &= check(primary_retained_line_generation(wide_model, 0) == generation + 1U,
        "wide-cell occupancy change increments generation");

    const QByteArray wide_scalar      = bytes_from_hex("e4b880");
    const QByteArray other_wide_scalar = bytes_from_hex("e7958c");
    term::Terminal_screen_model non_ascii_model = make_model(1, 4);
    non_ascii_model.ingest(wide_scalar);
    generation = primary_retained_line_generation(non_ascii_model, 0);
    non_ascii_model.ingest(QByteArrayLiteral("\r") + wide_scalar);
    ok &= check(primary_retained_line_generation(non_ascii_model, 0) == generation,
        "idempotent non-ASCII scalar rewrite does not increment generation");
    non_ascii_model.ingest(QByteArrayLiteral("\r") + other_wide_scalar);
    ok &= check(primary_retained_line_generation(non_ascii_model, 0) == generation + 1U,
        "changed non-ASCII scalar rewrite increments generation");

    term::Terminal_screen_model non_ascii_style_model = make_model(1, 4);
    non_ascii_style_model.ingest(wide_scalar);
    generation = primary_retained_line_generation(non_ascii_style_model, 0);
    non_ascii_style_model.ingest(QByteArrayLiteral("\r\x1b[31m") + wide_scalar);
    ok &= check(primary_retained_line_generation(non_ascii_style_model, 0) == generation,
        "style-only non-ASCII scalar rewrite does not increment generation");

    term::Terminal_screen_model non_ascii_hyperlink_model = make_model(1, 4);
    non_ascii_hyperlink_model.ingest(
        QByteArrayLiteral("\x1b]8;;https://one.example\x1b\\") +
        wide_scalar +
        QByteArrayLiteral("\x1b]8;;\x1b\\"));
    generation = primary_retained_line_generation(non_ascii_hyperlink_model, 0);
    non_ascii_hyperlink_model.ingest(
        QByteArrayLiteral("\r\x1b]8;;https://two.example\x1b\\") +
        wide_scalar +
        QByteArrayLiteral("\x1b]8;;\x1b\\"));
    ok &= check(primary_retained_line_generation(non_ascii_hyperlink_model, 0) == generation,
        "hyperlink-only non-ASCII scalar rewrite does not increment generation");

    term::Terminal_screen_model wide_base_model = make_model(1, 4);
    wide_base_model.ingest(wide_scalar);
    generation = primary_retained_line_generation(wide_base_model, 0);
    wide_base_model.ingest(QByteArrayLiteral("\r") + other_wide_scalar);
    ok &= check(primary_retained_line_generation(wide_base_model, 0) == generation + 1U &&
            wide_base_model.row_text(0) == QString::fromUtf8(other_wide_scalar),
        "non-ASCII overwrite of wide base updates text and generation");

    term::Terminal_screen_model wide_continuation_model = make_model(1, 4);
    wide_continuation_model.ingest(bytes_from_hex("e4b880"));
    generation = primary_retained_line_generation(wide_continuation_model, 0);
    wide_continuation_model.ingest(QByteArrayLiteral("\x1b[1;2HA"));
    ok &= check(
        primary_retained_line_generation(wide_continuation_model, 0) == generation + 1U,
        "ASCII overwrite of wide-continuation column increments generation");

    term::Terminal_screen_model non_ascii_wide_continuation_model = make_model(1, 4);
    non_ascii_wide_continuation_model.ingest(wide_scalar);
    generation = primary_retained_line_generation(non_ascii_wide_continuation_model, 0);
    non_ascii_wide_continuation_model.ingest(
        QByteArrayLiteral("\x1b[1;2H") + other_wide_scalar);
    ok &= check(
        primary_retained_line_generation(non_ascii_wide_continuation_model, 0) ==
            generation + 1U,
        "non-ASCII overwrite of wide-continuation column increments generation");
    ok &= check(
        non_ascii_wide_continuation_model.row_text(0) ==
            QStringLiteral(" ") + QString::fromUtf8(other_wide_scalar),
        "non-ASCII overwrite of wide-continuation column updates text and generation");

    term::Terminal_screen_model no_autowrap_model = make_model(1, 4);
    no_autowrap_model.ingest(QByteArrayLiteral("\x1b[?7lABCDE"));
    generation = primary_retained_line_generation(no_autowrap_model, 0);
    no_autowrap_model.ingest(QByteArrayLiteral("\rABCDE"));
    ok &= check(primary_retained_line_generation(no_autowrap_model, 0) == generation,
        "no-autowrap clipped identical row rewrite does not increment generation");
    no_autowrap_model.ingest(QByteArrayLiteral("\rABCDX"));
    ok &= check(primary_retained_line_generation(no_autowrap_model, 0) == generation + 1U,
        "no-autowrap clipped last-cell change increments generation");

    term::Terminal_screen_model combining_model = make_model(1, 4);
    combining_model.ingest(QByteArrayLiteral("e"));
    generation = primary_retained_line_generation(combining_model, 0);
    combining_model.ingest(bytes_from_hex("cc81"));
    ok &= check(primary_retained_line_generation(combining_model, 0) == generation + 1U,
        "combining sequence change increments generation");

    term::Terminal_screen_model variation_model = make_model(1, 4);
    variation_model.ingest(bytes_from_hex("e29da4"));
    generation = primary_retained_line_generation(variation_model, 0);
    variation_model.ingest(bytes_from_hex("efb88f"));
    ok &= check(primary_retained_line_generation(variation_model, 0) == generation + 1U,
        "variation sequence change increments generation");

    term::Terminal_screen_model overflow_combining_model = make_model(2, 4);
    overflow_combining_model.ingest(QByteArrayLiteral("abc") + bytes_from_hex("e29da4"));
    const std::uint64_t overflow_source_generation =
        primary_retained_line_generation(overflow_combining_model, 0);
    const std::uint64_t overflow_destination_generation =
        primary_retained_line_generation(overflow_combining_model, 1);
    overflow_combining_model.ingest(bytes_from_hex("efb88f"));
    ok &= check(
        primary_retained_line_generation(overflow_combining_model, 0) ==
            overflow_source_generation + 1U &&
            primary_retained_line_generation(overflow_combining_model, 1) ==
                overflow_destination_generation + 1U &&
            overflow_combining_model.row_text(0) == QStringLiteral("abc") &&
            overflow_combining_model.row_text(1) ==
                QString::fromUtf8(bytes_from_hex("e29da4efb88f")),
        "combining autowrap overflow clears source row and installs destination row");

    term::Terminal_screen_model style_model = make_model(1, 4);
    style_model.ingest(QByteArrayLiteral("A"));
    generation = primary_retained_line_generation(style_model, 0);
    style_model.ingest(QByteArrayLiteral("\r\x1b[31mA"));
    ok &= check(primary_retained_line_generation(style_model, 0) == generation,
        "style-only rewrite of identical text does not increment generation");

    term::Terminal_screen_model hyperlink_model = make_model(1, 4);
    hyperlink_model.ingest(
        QByteArrayLiteral("\x1b]8;;https://one.example\x1b\\A\x1b]8;;\x1b\\"));
    generation = primary_retained_line_generation(hyperlink_model, 0);
    hyperlink_model.ingest(
        QByteArrayLiteral("\r\x1b]8;;https://two.example\x1b\\A\x1b]8;;\x1b\\"));
    ok &= check(primary_retained_line_generation(hyperlink_model, 0) == generation,
        "hyperlink-only rewrite of identical text does not increment generation");

    return ok;
}

bool test_replies_and_cursor_save_restore()
{
    bool ok = true;

    auto check_rejected_csi_t_query = [&](const term::Terminal_screen_model_result& query_result,
        term::Parser_diagnostic_code expected_code,
        const char* label) {
        const std::vector<term::Terminal_reply> query_replies = replies_in(query_result);
        ok &= check(query_replies.empty(), label);
        ok &= check(diagnostic_count(query_result) == 1, label);
        ok &= check(first_diagnostic(query_result).code == expected_code, label);
    };

    term::Terminal_screen_model model = make_model(4, 5);
    term::Terminal_screen_model_result result = model.ingest(
        QByteArrayLiteral("\x1b[2;3H\x1b[6nB\x1b[c\x1b[>c\x1b[?6$p"));
    ok &= check(diagnostic_count(result) == 0, "terminal queries have no diagnostics");

    const std::vector<term::Terminal_reply> replies = replies_in(result);
    ok &= check(replies.size() == 4U, "terminal queries emit four replies");
    const term::Terminal_reply& dsr_reply    = reply_at(replies, 0U);
    const term::Terminal_reply& da1_reply    = reply_at(replies, 1U);
    const term::Terminal_reply& da2_reply    = reply_at(replies, 2U);
    const term::Terminal_reply& decrqm_reply = reply_at(replies, 3U);
    ok &= check(dsr_reply.kind == term::Terminal_reply_kind::DSR_CURSOR_POSITION &&
        dsr_reply.wire_bytes == QByteArrayLiteral("\x1b[2;3R"),
        "DSR cursor reply");
    ok &= check(da1_reply.kind == term::Terminal_reply_kind::DA1 &&
        da1_reply.wire_bytes == QByteArrayLiteral("\x1b[?1;2c"),
        "DA1 reply");
    ok &= check(da2_reply.kind == term::Terminal_reply_kind::DA2 &&
        da2_reply.wire_bytes == QByteArrayLiteral("\x1b[>0;0;0c"),
        "DA2 reply");
    ok &= check(decrqm_reply.kind == term::Terminal_reply_kind::DECRQM &&
        decrqm_reply.wire_bytes == QByteArrayLiteral("\x1b[?6;2$y"),
        "DECRQM origin reset reply");
    ok &= check(snapshot_valid(model, 40U), "reply mixed snapshot validates");

    model = make_model(4, 5);
    result = model.ingest(QByteArrayLiteral("\x1b[2;4r\x1b[?6h\x1b[1;2H\x1b[6n"));
    const std::vector<term::Terminal_reply> origin_replies = replies_in(result);
    ok &= check(origin_replies.size() == 1U, "origin DSR emits one reply");
    ok &= check(reply_at(origin_replies, 0U).wire_bytes == QByteArrayLiteral("\x1b[1;2R"),
        "origin DSR reports region-relative row");
    ok &= check(snapshot_valid(model, 43U), "origin DSR snapshot validates");

    model = make_model(7, 13);
    model.ingest(QByteArrayLiteral("abc"));
    result = model.ingest(QByteArrayLiteral("\x1b[18t"));
    ok &= check(diagnostic_count(result) == 0, "text-area size query has no diagnostics");
    const std::vector<term::Terminal_reply> text_area_size_replies = replies_in(result);
    ok &= check(text_area_size_replies.size() == 1U,
        "text-area size query emits one reply");
    const term::Terminal_reply& text_area_size_reply =
        reply_at(text_area_size_replies, 0U);
    ok &= check(text_area_size_reply.kind == term::Terminal_reply_kind::TEXT_AREA_SIZE &&
        text_area_size_reply.wire_bytes == QByteArrayLiteral("\x1b[8;7;13t"),
        "text-area size query reports configured grid");
    ok &= check(text_area_size_reply.source_sequence == QStringLiteral("CSI 18 t"),
        "text-area size query reply source");
    ok &= check(model.row_text(0) == QStringLiteral("abc"),
        "text-area size query does not mutate screen text");
    ok &= dirty_rows_equal(result, {}, "text-area size query has no dirty rows");
    ok &= check(!result.viewport_changed,
        "text-area size query does not move viewport");

    result  = model.ingest(QByteArrayLiteral("\x1b[8;5;9t"));
    ok     &= check(diagnostic_count(result) == 0,
        "text-area resize request has no diagnostics");
    ok     &= check(replies_in(result).empty(),
        "text-area resize request emits no terminal reply");
    const std::vector<term::Parser_notification> text_area_resize_notifications =
        notifications_in(result);
    ok &= check(text_area_resize_notifications.size() == 1U,
        "text-area resize request emits one notification");
    if (!text_area_resize_notifications.empty()) {
        const term::Parser_notification& notification =
            text_area_resize_notifications.front();
        ok &= check(notification.kind ==
            term::Parser_notification_kind::TEXT_AREA_RESIZE_REQUESTED &&
            notification.rows == 5 &&
            notification.columns == 9,
            "text-area resize request carries requested grid");
    }
    ok &= check(model.grid_size().rows == 5 && model.grid_size().columns == 9,
        "text-area resize request applies requested grid");
    ok &= dirty_rows_equal(result, row_range(5),
        "text-area resize request dirties resized visible rows");
    ok &= check(result.backing_deltas.size() == 2U &&
            active_grid_backing_delta_matches(
                result.backing_deltas[0],
                term::Terminal_backing_delta_kind::ACTIVE_GRID_RESIZED,
                term::Terminal_buffer_id::PRIMARY,
                term::terminal_grid_size_t{7, 13},
                term::terminal_grid_size_t{5, 9}) &&
            active_grid_backing_delta_matches(
                result.backing_deltas[1],
                term::Terminal_backing_delta_kind::COLUMN_REFLOWED,
                term::Terminal_buffer_id::PRIMARY,
                term::terminal_grid_size_t{7, 13},
                term::terminal_grid_size_t{5, 9}),
        "text-area resize request records active-grid resize and column-reflow deltas");

    result  = model.ingest(QByteArrayLiteral("\x1b[8;4097;9t"));
    ok     &= check(diagnostic_count(result) == 1,
        "oversized text-area resize request emits a diagnostic");
    ok     &= check(first_diagnostic(result).code ==
        term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
        "oversized text-area resize request is unsupported");
    ok     &= check(notifications_in(result).empty(),
        "oversized text-area resize request emits no notification");
    ok     &= check(model.grid_size().rows == 5 && model.grid_size().columns == 9,
        "oversized text-area resize request leaves grid unchanged");

    model   = make_model(2, 4);
    result  = model.ingest(QByteArrayLiteral("aa\x1b[8;3;5t\x1b[3;5HZ"));
    ok     &= check(diagnostic_count(result) == 0,
        "inline text-area resize sequence has no diagnostics");
    ok     &= check(model.grid_size().rows == 3 && model.grid_size().columns == 5,
        "inline text-area resize updates grid before following output");
    ok     &= check(model.row_text(2) == QStringLiteral("    Z"),
        "inline text-area resize interprets following cursor address in resized grid");
    ok     &= check(result.backing_deltas.size() == 2U &&
            active_grid_backing_delta_matches(
                result.backing_deltas[0],
                term::Terminal_backing_delta_kind::ACTIVE_GRID_RESIZED,
                term::Terminal_buffer_id::PRIMARY,
                term::terminal_grid_size_t{2, 4},
                term::terminal_grid_size_t{3, 5}) &&
            active_grid_backing_delta_matches(
                result.backing_deltas[1],
                term::Terminal_backing_delta_kind::COLUMN_REFLOWED,
                term::Terminal_buffer_id::PRIMARY,
                term::terminal_grid_size_t{2, 4},
                term::terminal_grid_size_t{3, 5}),
        "inline text-area resize records active-grid resize and column-reflow deltas");
    ok     &= check(snapshot_valid(model, 44U),
        "inline text-area resize snapshot validates");

    result = model.ingest(QByteArrayLiteral("\x1b[14t"));
    check_rejected_csi_t_query(
        result,
        term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
        "CSI 14 t is an unsupported lowercase t query");

    result = model.ingest(QByteArrayLiteral("\x1b[19t"));
    check_rejected_csi_t_query(
        result,
        term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
        "CSI 19 t is an unsupported lowercase t query");

    result = model.ingest(QByteArrayLiteral("\x1b[22;0t"));
    check_rejected_csi_t_query(
        result,
        term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
        "CSI 22;0 t is an unsupported lowercase t subcommand");

    result = model.ingest(QByteArrayLiteral("\x1b[18:1t"));
    check_rejected_csi_t_query(
        result,
        term::Parser_diagnostic_code::MALFORMED_INPUT,
        "CSI 18:1 t is malformed for lowercase t");

    result = model.ingest(QByteArrayLiteral("\x1b[?18t"));
    check_rejected_csi_t_query(
        result,
        term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
        "CSI ?18 t is an unsupported private lowercase t query");

    result = model.ingest(QByteArrayLiteral("\x1b[>18t"));
    check_rejected_csi_t_query(
        result,
        term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
        "CSI >18 t is an unsupported private lowercase t query");

    result = model.ingest(QByteArrayLiteral("\x1b[18$t"));
    check_rejected_csi_t_query(
        result,
        term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
        "CSI 18 $ t is an unsupported intermediate lowercase t query");

    result = model.ingest(QByteArrayLiteral("\x1b[t"));
    check_rejected_csi_t_query(
        result,
        term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
        "bare CSI t is an unsupported lowercase t query");

    model = make_model(3, 4);
    model.resize(term::terminal_grid_size_t{9, 11});
    result = model.ingest(QByteArrayLiteral("\x1b[18t"));
    ok &= check(diagnostic_count(result) == 0,
        "resized text-area size query has no diagnostics");
    const std::vector<term::Terminal_reply> resized_text_area_size_replies =
        replies_in(result);
    ok &= check(resized_text_area_size_replies.size() == 1U,
        "resized text-area size query emits one reply");
    const term::Terminal_reply& resized_text_area_size_reply =
        reply_at(resized_text_area_size_replies, 0U);
    ok &= check(resized_text_area_size_reply.kind == term::Terminal_reply_kind::TEXT_AREA_SIZE &&
        resized_text_area_size_reply.wire_bytes == QByteArrayLiteral("\x1b[8;9;11t"),
        "resized text-area size query reports post-resize grid");

    model   = make_model(2, 5);
    result  = model.ingest(QByteArrayLiteral("\x1b[2;4H\x1b" "7\x1b[1;1HA\x1b" "8B"));
    ok     &= check(diagnostic_count(result) == 0, "ESC save/restore has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("A"), "restore leaves home write");
    ok     &= check(model.row_text(1) == QStringLiteral("   B"), "restore writes saved cursor");
    ok     &= check(snapshot_valid(model, 41U), "ESC restore snapshot validates");

    model   = make_model(2, 5);
    result  = model.ingest(QByteArrayLiteral("\x1b[2;4H\x1b[s\x1b[1;1HA\x1b[uB"));
    ok     &= check(diagnostic_count(result) == 0, "CSI save/restore has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("A"), "CSI restore leaves home write");
    ok     &= check(model.row_text(1) == QStringLiteral("   B"), "CSI restore writes saved cursor");
    ok     &= check(snapshot_valid(model, 42U), "CSI restore snapshot validates");

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_cursor_addressing_and_split_csi();
    ok &= test_scrollback_growth_observer_seam();
    ok &= test_phase_r_repaint_recovery_shift_helper_matches_policy();
    ok &= test_phase_r_primary_repaint_recovery_accepts_distinct_shift();
    ok &= test_flat_ring_phase3_retained_record_producer_contract();
    ok &= test_flat_ring_phase7_recovery_shared_producer_boundary();
    ok &= test_flat_ring_phase5a_resize_projects_retained_history_without_mutation();
    ok &= test_flat_ring_phase5b_retained_hyperlink_metadata_authority();
    ok &= test_phase_r_primary_repaint_recovery_suppresses_false_positives();
    ok &= test_phase_r_primary_repaint_recovery_toggle_cancels_candidate();
    ok &= test_erase_operations_and_wide_damage();
    ok &= test_scroll_region_and_origin_mode();
    ok &= test_top_anchored_scroll_region_appends_scrollback();
    ok &= test_alternate_top_anchored_scroll_region_does_not_append_scrollback();
    ok &= test_escape_index_controls();
    ok &= test_scroll_region_history_insert_after_reverse_index();
    ok &= test_scroll_region_zero_bottom_defaults_to_screen_bottom();
    ok &= test_scroll_up_down_sequences();
    ok &= test_blank_fill_operations_use_current_style();
    ok &= test_insert_delete_lines_cells_and_tabs();
    ok &= test_retained_line_provenance_lifecycle();
    ok &= test_recovery_disabled_non_scroll_sources_do_not_grow_retained_history();
    ok &= test_recovery_disabled_chunk_split_invariance_for_non_scroll_sources();
    ok &= test_recovery_disabled_scrollback_limit_changes_do_not_grow_retained_history();
    ok &= test_retained_line_content_generation_mutations();
    ok &= test_replies_and_cursor_save_restore();
    return ok ? 0 : 1;
}
