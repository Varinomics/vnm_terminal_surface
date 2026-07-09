#include "vnm_terminal/internal/terminal_screen_model.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QString>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <variant>

namespace term = vnm_terminal::internal;

namespace {

// The model seeds its color state from the default (Campbell) scheme, so the
// basic-16 SGR colors and the default foreground/background resolve to Campbell
// palette values rather than the Terminal_color_state struct fallbacks.
constexpr quint32 k_default_foreground_rgba = 0xffccccccU; // Campbell foreground
constexpr quint32 k_default_background_rgba = 0xff0c0c0cU; // Campbell background
constexpr quint32 k_red_rgba          = 0xffc50f1fU; // Campbell palette slot 1
constexpr quint32 k_green_rgba        = 0xff13a10eU; // Campbell palette slot 2
constexpr quint32 k_blue_rgba         = 0xff0037daU; // Campbell palette slot 4
constexpr quint32 k_magenta_rgba      = 0xff881798U; // Campbell palette slot 5
constexpr quint32 k_bright_red_rgba   = 0xffff0000U;
constexpr quint32 k_bright_green_rgba = 0xff00ff00U;
constexpr quint32 k_bright_blue_rgba  = 0xff0000ffU;
constexpr quint32 k_orange_256_rgba   = 0xffff5f00U;
constexpr quint32 k_gray_232_rgba     = 0xff080808U;
constexpr quint32 k_gray_255_rgba     = 0xffeeeeeeU;
constexpr quint32 k_truecolor_fg_rgba = 0xff0c2238U;
constexpr quint32 k_truecolor_bg_rgba = 0xffc89664U;

using vnm_terminal::test_helpers::check;

[[noreturn]] void fail_required_value(const char* message)
{
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

term::Terminal_screen_model make_model(int rows = 3, int columns = 32)
{
    return term::Terminal_screen_model({
        term::terminal_grid_size_t{rows, columns},
        4,
        4,
    });
}

QByteArray set_truecolor_foreground(int red, int green, int blue)
{
    return
        QByteArrayLiteral("\x1b[38;2;") +
        QByteArray::number(red)          + ';' +
        QByteArray::number(green)        + ';' +
        QByteArray::number(blue)         + 'm';
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

int screen_mutation_count(const term::Terminal_screen_model_result& result)
{
    int count = 0;
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) == term::Parser_action_kind::SCREEN_MUTATION) {
            ++count;
        }
    }
    return count;
}

int terminal_reply_count(const term::Terminal_screen_model_result& result)
{
    int count = 0;
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) == term::Parser_action_kind::TERMINAL_REPLY) {
            ++count;
        }
    }
    return count;
}

int first_action_index(
    const term::Terminal_screen_model_result&  result,
    term::Parser_action_kind                   kind)
{
    for (std::size_t i = 0; i < result.actions.size(); ++i) {
        if (term::parser_action_kind(result.actions[i]) == kind) {
            return static_cast<int>(i);
        }
    }

    return -1;
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

const term::Terminal_reply& terminal_reply_at(
    const term::Terminal_screen_model_result&  result,
    int                                        index)
{
    int current = 0;
    for (const term::Parser_action& action : result.actions) {
        if (term::parser_action_kind(action) != term::Parser_action_kind::TERMINAL_REPLY) {
            continue;
        }

        if (current == index) {
            return std::get<term::Terminal_reply>(action.payload);
        }

        ++current;
    }

    fail_required_value("expected terminal reply action");
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

const term::Terminal_text_style& style_for_cell(
    const term::Terminal_render_snapshot&  snapshot,
    const term::Terminal_render_cell&      cell)
{
    const std::size_t style_index = static_cast<std::size_t>(cell.style_id);
    if (style_index >= snapshot.styles.size()) {
        fail_required_value("expected render snapshot style");
    }

    return snapshot.styles[style_index];
}

quint32 foreground_rgba_for_cell(
    const term::Terminal_render_snapshot&  snapshot,
    const term::Terminal_render_cell&      cell)
{
    const term::Terminal_text_style& style = style_for_cell(snapshot, cell);
    return term::resolve_terminal_color_ref(style.foreground, snapshot.color_state, true);
}

quint32 background_rgba_for_cell(
    const term::Terminal_render_snapshot&  snapshot,
    const term::Terminal_render_cell&      cell)
{
    const term::Terminal_text_style& style = style_for_cell(snapshot, cell);
    return term::resolve_terminal_color_ref(style.background, snapshot.color_state, false);
}

bool has_attribute(
    const term::Terminal_text_style&       style,
    term::Terminal_style_attribute         attribute)
{
    return term::terminal_style_has_attribute(style, attribute);
}

bool check_cell_colors(
    const term::Terminal_render_snapshot&  snapshot,
    const term::Terminal_render_cell&      cell,
    quint32                                foreground,
    quint32                                background,
    const char*                            label)
{
    bool ok = true;
    ok &= check(foreground_rgba_for_cell(snapshot, cell) == foreground, label);
    ok &= check(background_rgba_for_cell(snapshot, cell) == background, label);
    return ok;
}

bool test_nested_sgr_and_resets()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model();
    const term::Terminal_screen_model_result result = model.ingest(
        QByteArrayLiteral("\x1b[31mA\x1b[1mB\x1b[4mC\x1b[22mD\x1b[39mE\x1b[0mF"));
    ok &= check(diagnostic_count(result) == 0, "nested SGR has no diagnostics");
    ok &= check(model.row_text(0) == QStringLiteral("ABCDEF"), "nested SGR text");

    const term::Terminal_render_snapshot snapshot = model.render_snapshot(1U);
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "nested SGR snapshot validates");
    const term::Terminal_render_cell& cell_a  = cell_at(snapshot, 0, 0);
    const term::Terminal_render_cell& cell_e  = cell_at(snapshot, 0, 4);
    const term::Terminal_render_cell& cell_f  = cell_at(snapshot, 0, 5);
    const term::Terminal_text_style&  style_a = style_for_cell(snapshot, cell_a);
    const term::Terminal_text_style&  style_b = style_for_cell(snapshot, cell_at(snapshot, 0, 1));
    const term::Terminal_text_style&  style_c = style_for_cell(snapshot, cell_at(snapshot, 0, 2));
    const term::Terminal_text_style&  style_d = style_for_cell(snapshot, cell_at(snapshot, 0, 3));
    const term::Terminal_text_style&  style_e = style_for_cell(snapshot, cell_e);
    const term::Terminal_text_style&  style_f = style_for_cell(snapshot, cell_f);

    ok &= check_cell_colors(
        snapshot,
        cell_a,
        k_red_rgba,
        k_default_background_rgba,
        "A captures red foreground");
    ok &= check(!has_attribute(style_a, term::Terminal_style_attribute::BOLD),
        "A is not retroactively bold");
    ok &= check(has_attribute(style_b, term::Terminal_style_attribute::BOLD),
        "B captures bold");
    ok &= check(has_attribute(style_c, term::Terminal_style_attribute::BOLD) &&
        has_attribute(style_c, term::Terminal_style_attribute::UNDERLINE),
        "C captures bold underline");
    ok &= check(!has_attribute(style_d, term::Terminal_style_attribute::BOLD) &&
        has_attribute(style_d, term::Terminal_style_attribute::UNDERLINE),
        "22 clears bold and keeps underline");
    ok &= check_cell_colors(
        snapshot,
        cell_e,
        k_default_foreground_rgba,
        k_default_background_rgba,
        "39 resets foreground");
    ok &= check(has_attribute(style_e, term::Terminal_style_attribute::UNDERLINE),
        "39 keeps underline");
    ok &= check(style_f.attributes == 0U, "0 clears attributes");
    ok &= check_cell_colors(
        snapshot,
        cell_f,
        k_default_foreground_rgba,
        k_default_background_rgba,
        "0 resets colors");
    return ok;
}

bool test_partial_attribute_resets()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model();
    const term::Terminal_screen_model_result result = model.ingest(
        QByteArrayLiteral("\x1b[1;2;3;4;5;7;8;9;31;44mA"
            "\x1b[22mB\x1b[23mC\x1b[24mD\x1b[25mE"
            "\x1b[27mF\x1b[28mG\x1b[29mH"));
    ok &= check(diagnostic_count(result) == 0, "partial reset SGR has no diagnostics");
    ok &= check(model.row_text(0) == QStringLiteral("ABCDEFGH"),
        "invisible cells remain in row text");

    const term::Terminal_render_snapshot snapshot = model.render_snapshot(2U);
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "partial reset snapshot validates");
    const term::Terminal_text_style& style_a = style_for_cell(snapshot, cell_at(snapshot, 0, 0));
    const term::Terminal_text_style& style_b = style_for_cell(snapshot, cell_at(snapshot, 0, 1));
    const term::Terminal_text_style& style_c = style_for_cell(snapshot, cell_at(snapshot, 0, 2));
    const term::Terminal_text_style& style_d = style_for_cell(snapshot, cell_at(snapshot, 0, 3));
    const term::Terminal_text_style& style_e = style_for_cell(snapshot, cell_at(snapshot, 0, 4));
    const term::Terminal_text_style& style_f = style_for_cell(snapshot, cell_at(snapshot, 0, 5));
    const term::Terminal_text_style& style_g = style_for_cell(snapshot, cell_at(snapshot, 0, 6));
    const term::Terminal_text_style& style_h = style_for_cell(snapshot, cell_at(snapshot, 0, 7));

    ok &= check_cell_colors(snapshot, cell_at(snapshot, 0, 0), k_red_rgba, k_blue_rgba,
        "full attribute colors");
    ok &= check(has_attribute(style_a, term::Terminal_style_attribute::BOLD) &&
        has_attribute(style_a, term::Terminal_style_attribute::FAINT) &&
        has_attribute(style_a, term::Terminal_style_attribute::ITALIC) &&
        has_attribute(style_a, term::Terminal_style_attribute::UNDERLINE) &&
        has_attribute(style_a, term::Terminal_style_attribute::BLINK) &&
        has_attribute(style_a, term::Terminal_style_attribute::INVERSE) &&
        has_attribute(style_a, term::Terminal_style_attribute::INVISIBLE) &&
        has_attribute(style_a, term::Terminal_style_attribute::STRIKE),
        "full attribute set captured");
    ok &= check(!has_attribute(style_b, term::Terminal_style_attribute::BOLD) &&
        !has_attribute(style_b, term::Terminal_style_attribute::FAINT),
        "22 clears bold and faint");
    ok &= check(!has_attribute(style_c, term::Terminal_style_attribute::ITALIC),
        "23 clears italic");
    ok &= check(!has_attribute(style_d, term::Terminal_style_attribute::UNDERLINE),
        "24 clears underline");
    ok &= check(!has_attribute(style_e, term::Terminal_style_attribute::BLINK),
        "25 clears blink");
    ok &= check(!has_attribute(style_f, term::Terminal_style_attribute::INVERSE),
        "27 clears inverse");
    ok &= check(!has_attribute(style_g, term::Terminal_style_attribute::INVISIBLE),
        "28 clears invisible");
    ok &= check(!has_attribute(style_h, term::Terminal_style_attribute::STRIKE),
        "29 clears strike");
    return ok;
}

bool test_color_modes()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model();
    const term::Terminal_screen_model_result result = model.ingest(
        QByteArrayLiteral("\x1b[38;5;196mA\x1b[38;5;46mB\x1b[38;5;21mC"
            "\x1b[38;5;232mD\x1b[38;5;255mE\x1b[48;5;202mF"
            "\x1b[38;2;12;34;56;48;2;200;150;100mG"
            "\x1b[38:2::12:34:56;48:2::200:150:100mH"
            "\x1b[38:5:196mI"
            "\x1b[38:2:99:12:34:56mJ"
            "\x1b[38:5:999:196mK"));
    ok &= check(diagnostic_count(result) == 0, "color SGR has no diagnostics");

    const term::Terminal_render_snapshot snapshot = model.render_snapshot(3U);
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "color mode snapshot validates");
    ok &= check(foreground_rgba_for_cell(snapshot, cell_at(snapshot, 0, 0)) ==
        k_bright_red_rgba,
        "256 color red");
    ok &= check(foreground_rgba_for_cell(snapshot, cell_at(snapshot, 0, 1)) ==
        k_bright_green_rgba,
        "256 color green");
    ok &= check(foreground_rgba_for_cell(snapshot, cell_at(snapshot, 0, 2)) ==
        k_bright_blue_rgba,
        "256 color blue");
    ok &= check(foreground_rgba_for_cell(snapshot, cell_at(snapshot, 0, 3)) ==
        k_gray_232_rgba,
        "256 grayscale low");
    ok &= check(foreground_rgba_for_cell(snapshot, cell_at(snapshot, 0, 4)) ==
        k_gray_255_rgba,
        "256 grayscale high");
    ok &= check(background_rgba_for_cell(snapshot, cell_at(snapshot, 0, 5)) ==
        k_orange_256_rgba,
        "256 background orange");
    ok &= check_cell_colors(
        snapshot,
        cell_at(snapshot, 0, 6),
        k_truecolor_fg_rgba,
        k_truecolor_bg_rgba,
        "semicolon truecolor");
    ok &= check_cell_colors(
        snapshot,
        cell_at(snapshot, 0, 7),
        k_truecolor_fg_rgba,
        k_truecolor_bg_rgba,
        "colon truecolor");
    ok &= check(foreground_rgba_for_cell(snapshot, cell_at(snapshot, 0, 8)) ==
        k_bright_red_rgba,
        "colon 256-color foreground");
    ok &= check(foreground_rgba_for_cell(snapshot, cell_at(snapshot, 0, 9)) ==
        k_truecolor_fg_rgba,
        "colon truecolor accepts color-space atom");
    ok &= check(foreground_rgba_for_cell(snapshot, cell_at(snapshot, 0, 10)) ==
        k_bright_red_rgba,
        "colon 256-color foreground uses last atom");
    return ok;
}

bool test_style_persistence_and_style_ids()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(3, 10);
    term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("\x1b[1;32;45m"));
    ok &= check(diagnostic_count(result) == 0, "SGR-only update has no diagnostics");
    ok &= check(screen_mutation_count(result) == 0, "SGR-only update has no screen mutation");
    ok &= check(result.dirty_rows.empty(), "SGR-only update has no dirty rows");

    result  = model.ingest(QByteArrayLiteral("A\r\nB\tC\x1b[0mD"));
    ok     &= check(diagnostic_count(result) == 0, "styled movement has no diagnostics");
    ok     &= check(model.row_text(0) == QStringLiteral("A"), "styled CRLF first row");
    ok     &= check(model.row_text(1) == QStringLiteral("B   CD"), "styled tab row text");

    const term::Terminal_render_snapshot snapshot = model.render_snapshot(4U);
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled persistence snapshot validates");
    const term::Terminal_text_style& style_a = style_for_cell(snapshot, cell_at(snapshot, 0, 0));
    const term::Terminal_text_style& style_b = style_for_cell(snapshot, cell_at(snapshot, 1, 0));
    const term::Terminal_text_style& style_c = style_for_cell(snapshot, cell_at(snapshot, 1, 4));
    const term::Terminal_text_style& style_d = style_for_cell(snapshot, cell_at(snapshot, 1, 5));
    ok &= check(foreground_rgba_for_cell(snapshot, cell_at(snapshot, 0, 0)) == k_green_rgba &&
        background_rgba_for_cell(snapshot, cell_at(snapshot, 0, 0)) == k_magenta_rgba &&
        has_attribute(style_a, term::Terminal_style_attribute::BOLD),
        "A captures persistent style");
    ok &= check(style_b == style_a && style_c == style_a,
        "style persists through CRLF and tab movement");
    ok &= check(snapshot.cells.size() == 4U, "tab movement does not materialize styled blanks");
    ok &= check(style_d.attributes == 0U &&
        foreground_rgba_for_cell(snapshot, cell_at(snapshot, 1, 5)) ==
            k_default_foreground_rgba &&
        background_rgba_for_cell(snapshot, cell_at(snapshot, 1, 5)) ==
            k_default_background_rgba,
        "reset style applies to D");

    term::Terminal_screen_model reuse_model = make_model();
    result = reuse_model.ingest(QByteArrayLiteral("\x1b[31mA\x1b[0mB\x1b[31mC"));
    const term::Terminal_render_snapshot reuse_snapshot = reuse_model.render_snapshot(5U);
    ok &= check(term::validate_render_snapshot(reuse_snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "style reuse snapshot validates");
    ok &= check(cell_at(reuse_snapshot, 0, 0).style_id == cell_at(reuse_snapshot, 0, 2).style_id,
        "equal styles reuse snapshot style id");
    ok &= check(cell_at(reuse_snapshot, 0, 1).style_id == term::k_default_terminal_style_id,
        "default style remains id zero");
    return ok;
}

bool test_style_table_compaction_and_capacity_policy()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(1, 2);
    model.set_style_table_limits_for_testing(64U, 64U);
    for (int index = 1; index <= 63; ++index) {
        model.ingest(
            QByteArrayLiteral("\r") +
            set_truecolor_foreground(index, index + 32, index + 64) +
            QByteArrayLiteral("X"));
    }

    term::terminal_screen_model_style_table_stats_t stats = model.style_table_stats();
    ok &= check(stats.current_style_count == 64U &&
        stats.peak_style_count == 64U &&
        stats.compaction_count == 0U,
        "truecolor churn reaches the style cap before compaction");

    model.ingest(
        QByteArrayLiteral("\r") +
        set_truecolor_foreground(64, 96, 128) +
        QByteArrayLiteral("Y"));
    stats = model.style_table_stats();
    ok &= check(stats.current_style_count == 3U &&
        stats.peak_style_count == 64U &&
        stats.compaction_count == 1U &&
        stats.reclaimed_styles == 62U,
        "cap-triggered compaction reclaims overwritten truecolor styles");

    const term::Terminal_render_snapshot snapshot = model.render_snapshot(12U);
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "style table compaction snapshot validates");
    ok &= check(foreground_rgba_for_cell(snapshot, cell_at(snapshot, 0, 0)) ==
        term::rgba_from_components(64, 96, 128),
        "style table compaction preserves the surviving truecolor value");

    term::Terminal_screen_model threshold_model = make_model(1, 2);
    threshold_model.set_style_table_limits_for_testing(4U, 16U);
    for (int index = 1; index <= 9; ++index) {
        threshold_model.ingest(
            QByteArrayLiteral("\r") +
            set_truecolor_foreground(index, index + 16, index + 32) +
            QByteArrayLiteral("T"));
    }
    const term::terminal_screen_model_style_table_stats_t threshold_stats =
        threshold_model.style_table_stats();
    ok &= check(threshold_stats.current_style_count == 3U &&
            threshold_stats.peak_style_count == 7U &&
            threshold_stats.compaction_count == 2U &&
            threshold_stats.reclaimed_styles == 7U,
        "soft style threshold schedules another compaction after bounded growth");

    term::Terminal_screen_model current_transition_model = make_model(1, 2);
    current_transition_model.set_style_table_limits_for_testing(3U, 3U);
    current_transition_model.ingest(
        set_truecolor_foreground(14, 24, 34) + QByteArrayLiteral("A"));
    current_transition_model.ingest(set_truecolor_foreground(44, 54, 64));
    current_transition_model.ingest(
        set_truecolor_foreground(74, 84, 94) + QByteArrayLiteral("C"));

    const term::terminal_screen_model_style_table_stats_t current_transition_stats =
        current_transition_model.style_table_stats();
    ok &= check(current_transition_stats.current_style_count == 3U &&
            current_transition_stats.peak_style_count == 3U &&
            current_transition_stats.compaction_count == 1U &&
            current_transition_stats.reclaimed_styles == 1U,
        "hard-cap transition reclaims an otherwise unreferenced outgoing current style");

    const term::Terminal_render_snapshot current_transition_snapshot =
        current_transition_model.render_snapshot(16U);
    ok &= check(term::validate_render_snapshot(current_transition_snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "outgoing-current reclamation snapshot validates");
    ok &= check(foreground_rgba_for_cell(
            current_transition_snapshot,
            cell_at(current_transition_snapshot, 0, 0)) ==
                term::rgba_from_components(14, 24, 34) &&
        foreground_rgba_for_cell(
            current_transition_snapshot,
            cell_at(current_transition_snapshot, 0, 1)) ==
                term::rgba_from_components(74, 84, 94),
        "outgoing-current reclamation preserves the cell root and installs the pending style");

    term::Terminal_screen_model capped_model = make_model(1, 2);
    capped_model.set_style_table_limits_for_testing(3U, 3U);
    capped_model.ingest(set_truecolor_foreground(41, 51, 61) + QByteArrayLiteral("A"));
    capped_model.ingest(set_truecolor_foreground(46, 56, 66) + QByteArrayLiteral("B"));

    bool exhausted = false;
    try {
        capped_model.ingest(set_truecolor_foreground(43, 53, 63));
    }
    catch (const std::overflow_error&) {
        exhausted = true;
    }
    ok &= check(exhausted,
        "style table allocation fails explicitly when every capped slot is live");

    const term::terminal_screen_model_style_table_stats_t capped_stats =
        capped_model.style_table_stats();
    ok &= check(capped_stats.current_style_count == 3U &&
        capped_stats.peak_style_count == 3U &&
        capped_stats.compaction_count == 0U,
        "failed style table compaction leaves diagnostics and table state unchanged");

    const term::Terminal_render_snapshot capped_snapshot = capped_model.render_snapshot(13U);
    ok &= check(term::validate_render_snapshot(capped_snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "style table exhaustion preserves a valid prior snapshot");
    ok &= check(foreground_rgba_for_cell(capped_snapshot, cell_at(capped_snapshot, 0, 0)) ==
            term::rgba_from_components(41, 51, 61) &&
        foreground_rgba_for_cell(capped_snapshot, cell_at(capped_snapshot, 0, 1)) ==
            term::rgba_from_components(46, 56, 66),
        "style table exhaustion preserves previously written style values");

    capped_model.ingest(QByteArrayLiteral("\rZ"));
    const term::Terminal_render_snapshot post_failure_snapshot =
        capped_model.render_snapshot(17U);
    ok &= check(foreground_rgba_for_cell(
            post_failure_snapshot,
            cell_at(post_failure_snapshot, 0, 0)) ==
                term::rgba_from_components(46, 56, 66),
        "style table exhaustion preserves the outgoing current style atomically");
    return ok;
}

bool test_style_table_compaction_remaps_saved_buffer_cursors()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model(1, 8);
    model.set_style_table_limits_for_testing(4U, 8U);

    model.ingest(set_truecolor_foreground(71, 81, 91));
    model.ingest(set_truecolor_foreground(72, 82, 92) + QByteArrayLiteral("P\x1b" "7"));
    model.ingest(QByteArrayLiteral("\x1b[?47h") +
        set_truecolor_foreground(73, 83, 93) + QByteArrayLiteral("A\x1b" "7"));
    model.ingest(QByteArrayLiteral("\x1b[?47l") +
        set_truecolor_foreground(74, 84, 94));

    const term::terminal_screen_model_style_table_stats_t stats = model.style_table_stats();
    ok &= check(stats.compaction_count == 1U && stats.reclaimed_styles == 1U,
        "style compaction remaps buffer roots after reclaiming an unused style");

    model.ingest(QByteArrayLiteral("\x1b[?47h\x1b" "8B"));
    const term::Terminal_render_snapshot alternate_snapshot = model.render_snapshot(14U);
    ok &= check(term::validate_render_snapshot(alternate_snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "style compaction alternate saved-cursor snapshot validates");
    ok &= check(foreground_rgba_for_cell(
            alternate_snapshot,
            cell_at(alternate_snapshot, 0, 0)) == term::rgba_from_components(73, 83, 93) &&
        foreground_rgba_for_cell(
            alternate_snapshot,
            cell_at(alternate_snapshot, 0, 1)) == term::rgba_from_components(73, 83, 93),
        "style compaction preserves alternate row and saved-cursor styles");

    model.ingest(QByteArrayLiteral("\x1b[?47l\x1b" "8Q"));
    const term::Terminal_render_snapshot primary_snapshot = model.render_snapshot(15U);
    ok &= check(term::validate_render_snapshot(primary_snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "style compaction primary saved-cursor snapshot validates");
    ok &= check(foreground_rgba_for_cell(
            primary_snapshot,
            cell_at(primary_snapshot, 0, 0)) == term::rgba_from_components(72, 82, 92) &&
        foreground_rgba_for_cell(
            primary_snapshot,
            cell_at(primary_snapshot, 0, 1)) == term::rgba_from_components(72, 82, 92),
        "style compaction preserves primary row and saved-cursor styles");
    return ok;
}

bool test_malformed_sgr_is_atomic()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model();
    const term::Terminal_screen_model_result result =
        model.ingest(QByteArrayLiteral("\x1b[31mR\x1b[1;38;5;256mX"));
    ok &= check(diagnostic_count(result) == 1, "malformed SGR emits one diagnostic");
    ok &= check(first_diagnostic(result).code == term::Parser_diagnostic_code::MALFORMED_INPUT,
        "malformed SGR diagnostic code");

    const term::Terminal_render_snapshot snapshot = model.render_snapshot(6U);
    const term::Terminal_text_style&     style_r  = style_for_cell(snapshot, cell_at(snapshot, 0, 0));
    const term::Terminal_text_style&     style_x  = style_for_cell(snapshot, cell_at(snapshot, 0, 1));
    ok &= check(foreground_rgba_for_cell(snapshot, cell_at(snapshot, 0, 0)) == k_red_rgba,
        "setup red style applied");
    ok &= check(foreground_rgba_for_cell(snapshot, cell_at(snapshot, 0, 1)) == k_red_rgba,
        "malformed SGR keeps previous style");
    ok &= check(!has_attribute(style_x, term::Terminal_style_attribute::BOLD),
        "malformed SGR applies no partial bold");

    model = make_model();
    term::Terminal_screen_model_result colon_result =
        model.ingest(QByteArrayLiteral("\x1b[1:2mX"));
    ok &= check(diagnostic_count(colon_result) == 1,
        "malformed simple colon SGR emits diagnostic");
    const term::Terminal_render_snapshot simple_colon_snapshot = model.render_snapshot(7U);
    ok &= check(cell_at(simple_colon_snapshot, 0, 0).style_id ==
        term::k_default_terminal_style_id,
        "malformed simple colon SGR keeps default style");

    model         = make_model();
    colon_result  = model.ingest(QByteArrayLiteral("\x1b[31:999mX"));
    ok           &= check(diagnostic_count(colon_result) == 1,
        "malformed color colon SGR emits diagnostic");
    const term::Terminal_render_snapshot color_colon_snapshot = model.render_snapshot(8U);
    ok &= check(cell_at(color_colon_snapshot, 0, 0).style_id ==
        term::k_default_terminal_style_id,
        "malformed color colon SGR keeps default style");

    model         = make_model();
    colon_result  = model.ingest(QByteArrayLiteral("\x1b[38:2:12::34:56mX"));
    ok           &= check(diagnostic_count(colon_result) == 1,
        "colon truecolor missing component emits diagnostic");
    const term::Terminal_render_snapshot truecolor_colon_snapshot = model.render_snapshot(9U);
    ok &= check(cell_at(truecolor_colon_snapshot, 0, 0).style_id ==
        term::k_default_terminal_style_id,
        "colon truecolor missing component keeps default style");
    return ok;
}

bool test_sgr_boundaries_and_c1_recovery()
{
    bool                               ok     = true;
    term::Terminal_screen_model        model  = make_model();
    term::Terminal_screen_model_result result = model.ingest(QByteArrayLiteral("\x1b[31"));
    ok &= check(diagnostic_count(result) == 0, "split supported SGR waits");
    ok &= check(result.dirty_rows.empty(), "split supported SGR does not dirty early");

    result = model.ingest(QByteArrayLiteral("mX"));
    ok &= check(diagnostic_count(result) == 0, "split supported SGR completes");
    const term::Terminal_render_snapshot split_snapshot = model.render_snapshot(10U);
    ok &= check(foreground_rgba_for_cell(split_snapshot, cell_at(split_snapshot, 0, 0)) ==
        k_red_rgba,
        "split supported SGR applies style");

    model = make_model();
    QByteArray c1_recovery("\x1b[?", 3);
    c1_recovery.append(static_cast<char>(0x9b));
    c1_recovery.append("31mY");
    result = model.ingest(c1_recovery);
    ok &= check(diagnostic_count(result) == 1, "C1 CSI boundary emits prior diagnostic");
    const term::Terminal_render_snapshot c1_snapshot = model.render_snapshot(11U);
    ok &= check(model.row_text(0) == QStringLiteral("Y"), "C1 CSI recovery prints following text");
    ok &= check(foreground_rgba_for_cell(c1_snapshot, cell_at(c1_snapshot, 0, 0)) ==
        k_red_rgba,
        "C1 CSI recovery reprocesses supported SGR");

    model = make_model();
    QByteArray overlong("\x1b[", 2);
    overlong.append(
        QByteArray(static_cast<int>(term::k_control_sequence_pending_limit_bytes + 1U), '1'));
    overlong.append("mZ");
    result  = model.ingest(overlong);
    ok     &= check(diagnostic_count(result) == 1, "complete overlong SGR emits one diagnostic");
    ok     &= check(first_diagnostic(result).family == term::Parser_sequence_family::CSI,
        "complete overlong SGR diagnostic family");
    ok     &= check(model.row_text(0) == QStringLiteral("Z"),
        "complete overlong SGR recovers after final byte");
    return ok;
}

bool test_osc_color_queries()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model();
    const term::Terminal_screen_model_result result = model.ingest(
        QByteArrayLiteral("\x1b]10;?\x1b\\\x1b]11;?\x1b\\\x1b]12;?\x1b\\"
            "\x1b]4;196;?\x1b\\"));
    ok &= check(diagnostic_count(result) == 0, "OSC color queries have no diagnostics");
    ok &= check(screen_mutation_count(result) == 0, "OSC color queries do not mutate screen");
    ok &= check(result.dirty_rows.empty(), "OSC color queries have no dirty rows");
    ok &= check(terminal_reply_count(result) == 4, "OSC color queries emit replies");

    ok &= check(terminal_reply_at(result, 0).wire_bytes ==
        QByteArrayLiteral("\x1b]10;rgb:cccc/cccc/cccc\x1b\\"),
        "OSC 10 reply bytes");
    ok &= check(terminal_reply_at(result, 1).wire_bytes ==
        QByteArrayLiteral("\x1b]11;rgb:0c0c/0c0c/0c0c\x1b\\"),
        "OSC 11 reply bytes");
    ok &= check(terminal_reply_at(result, 2).wire_bytes ==
        QByteArrayLiteral("\x1b]12;rgb:ffff/ffff/ffff\x1b\\"),
        "OSC 12 reply bytes");
    ok &= check(terminal_reply_at(result, 3).wire_bytes ==
        QByteArrayLiteral("\x1b]4;196;rgb:ffff/0000/0000\x1b\\"),
        "OSC 4 palette reply bytes");
    ok &= check(terminal_reply_at(result, 0).kind == term::Terminal_reply_kind::OSC_QUERY &&
        terminal_reply_at(result, 0).source_family == term::Parser_sequence_family::OSC,
        "OSC query reply metadata");

    term::Terminal_screen_model ordered_model = make_model();
    const term::Terminal_screen_model_result ordered =
        ordered_model.ingest(QByteArrayLiteral("\x1b]10;?\x1b\\X"));
    const int query_index = first_action_index(ordered, term::Parser_action_kind::TERMINAL_QUERY);
    const int reply_index = first_action_index(ordered, term::Parser_action_kind::TERMINAL_REPLY);
    const int mutation_index =
        first_action_index(ordered, term::Parser_action_kind::SCREEN_MUTATION);
    ok &= check(terminal_reply_count(ordered) == 1, "ordered OSC query emits one reply");
    ok &= check(query_index >= 0 && reply_index >= 0 && mutation_index >= 0,
        "ordered OSC query action kinds are present");
    ok &= check(query_index < reply_index && reply_index < mutation_index,
        "OSC query reply preserves action order before later output");

    term::Terminal_screen_model unsupported_model = make_model();
    const term::Terminal_screen_model_result unsupported =
        unsupported_model.ingest(QByteArrayLiteral("\x1b]4;300;?\x1b\\X"));
    ok &= check(diagnostic_count(unsupported) == 1,
        "unsupported OSC color query emits diagnostic");
    ok &= check(first_diagnostic(unsupported).code ==
        term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
        "unsupported OSC color query diagnostic code");
    ok &= check(terminal_reply_count(unsupported) == 0,
        "unsupported OSC color query emits no reply");
    ok &= check(unsupported_model.row_text(0) == QStringLiteral("X"),
        "parser resumes after unsupported OSC color query");
    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_nested_sgr_and_resets();
    ok &= test_partial_attribute_resets();
    ok &= test_color_modes();
    ok &= test_style_persistence_and_style_ids();
    ok &= test_style_table_compaction_and_capacity_policy();
    ok &= test_style_table_compaction_remaps_saved_buffer_cursors();
    ok &= test_malformed_sgr_is_atomic();
    ok &= test_sgr_boundaries_and_c1_recovery();
    ok &= test_osc_color_queries();
    return ok ? 0 : 1;
}
