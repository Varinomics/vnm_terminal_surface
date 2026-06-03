#include "vnm_terminal/internal/qsg_terminal_render_frame.h"
#include "helpers/test_check.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

term::Terminal_color_state color_state()
{
    term::Terminal_color_state state;
    state.default_foreground_rgba = 0xffc4e6c9U;
    state.default_background_rgba = 0xff090c10U;
    state.cursor_rgba             = 0xffffffffU;
    for (std::size_t i = 0; i < state.palette_rgba.size(); ++i) {
        state.palette_rgba[i] = 0xff202020U + static_cast<quint32>(i);
    }
    return state;
}

term::Terminal_render_snapshot empty_snapshot(term::terminal_grid_size_t grid_size)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = grid_size.rows;

    term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot(grid_size, viewport, 100U);
    snapshot.color_state = color_state();
    snapshot.cursor.position = {0, 0};
    return snapshot;
}

term::terminal_cell_metrics_t metrics()
{
    return {10.0, 20.0, 14.0, 6.0};
}

term::Terminal_render_options options()
{
    term::Terminal_render_options options;
    options.default_background = QColor(9,   12,  16);
    options.default_foreground = QColor(196, 230, 201);
    options.cursor_color       = QColor(255, 255, 255);
    return options;
}

term::Terminal_render_frame build(
    const term::Terminal_render_snapshot&  snapshot,
    term::Terminal_render_options          render_options = options(),
    bool                                   cursor_blink_visible = true,
    const term::Ime_preedit_state*         ime_preedit_override = nullptr)
{
    return
        term::build_terminal_render_frame(
            &snapshot,
            QSizeF(160.0, 100.0),
            metrics(),
            render_options,
            cursor_blink_visible,
            ime_preedit_override);
}

const term::Terminal_render_text_run* run_at(
    const term::Terminal_render_frame&     frame,
    int                                    row,
    int                                    column)
{
    for (const term::Terminal_render_text_run& run : frame.text_runs) {
        if (run.row == row && run.column == column) {
            return &run;
        }
    }
    return nullptr;
}

std::vector<const term::Terminal_render_text_run*> text_runs_for_row(
    const term::Terminal_render_frame& frame,
    int                                row)
{
    std::vector<const term::Terminal_render_text_run*> runs;
    for (const term::Terminal_render_text_run& run : frame.text_runs) {
        if (run.row == row) {
            runs.push_back(&run);
        }
    }

    return runs;
}

QString packed_text(
    const term::Terminal_render_frame&         frame,
    const term::terminal_packed_text_span_t&   span)
{
    return QString::fromUtf8(
        frame.packed_text_bytes.data() + span.text_offset,
        static_cast<int>(span.text_length));
}

QString packed_text_payload(const term::Terminal_render_frame& frame)
{
    return QString::fromUtf8(
        frame.packed_text_bytes.data(),
        static_cast<int>(frame.packed_text_bytes.size()));
}

bool has_packed_graphic_codepoint(
    const term::Terminal_render_frame& frame,
    std::uint32_t                      codepoint)
{
    return std::find(
        frame.packed_graphic_codepoints.begin(),
        frame.packed_graphic_codepoints.end(),
        codepoint) != frame.packed_graphic_codepoints.end();
}

bool has_graphic_rect(
    const term::Terminal_render_frame& frame,
    const QRectF&                      rect)
{
    for (const term::Terminal_render_rect& graphic_rect : frame.graphic_rects) {
        if (graphic_rect.rect == rect) {
            return true;
        }
    }

    return false;
}

bool snapshot_cells_are_row_major_column_sorted(
    const term::Terminal_render_snapshot& snapshot)
{
    for (std::size_t index = 1U; index < snapshot.cells.size(); ++index) {
        const term::Terminal_render_cell& previous = snapshot.cells[index - 1U];
        const term::Terminal_render_cell& current  = snapshot.cells[index];
        if (current.position.row < previous.position.row) {
            return false;
        }
        if (current.position.row    == previous.position.row &&
            current.position.column <  previous.position.column)
        {
            return false;
        }
    }

    return true;
}

bool has_background_rect(
    const term::Terminal_render_frame& frame,
    const QRectF&                      rect,
    const QColor&                      color)
{
    for (const term::Terminal_render_rect& background_rect : frame.background_rects) {
        if (background_rect.rect  == rect &&
            background_rect.color == color)
        {
            return true;
        }
    }

    return false;
}

bool has_graphic_rect_antialias(
    const term::Terminal_render_frame& frame,
    const QRectF&                      rect,
    bool                               antialias)
{
    for (const term::Terminal_render_rect& graphic_rect : frame.graphic_rects) {
        if (graphic_rect.rect      == rect &&
            graphic_rect.antialias == antialias)
        {
            return true;
        }
    }

    return false;
}

bool has_graphic_arc(
    const term::Terminal_render_frame& frame,
    term::Terminal_render_arc_kind     kind,
    const QRectF&                      rect,
    qreal                              stroke)
{
    for (const term::Terminal_render_arc& arc : frame.graphic_arcs) {
        if (arc.kind   == kind &&
            arc.rect   == rect &&
            arc.stroke == stroke)
        {
            return true;
        }
    }

    return false;
}

bool test_plain_text_color_inverse_and_wide_skip()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({3, 16});

    term::Terminal_text_style colored = term::make_default_terminal_text_style();
    colored.foreground = term::make_rgb_terminal_color_ref(0xffcc0000U);
    colored.background = term::make_rgb_terminal_color_ref(0xff0030ccU);
    snapshot.styles.push_back(colored);

    term::Terminal_text_style inverse = colored;
    term::set_terminal_style_attribute(inverse, term::Terminal_style_attribute::INVERSE);
    snapshot.styles.push_back(inverse);

    term::Terminal_text_style faint = colored;
    term::set_terminal_style_attribute(faint, term::Terminal_style_attribute::FAINT);
    snapshot.styles.push_back(faint);

    snapshot.cells.push_back({{0, 0}, QStringLiteral("A"), 0U, 1, false, 1U});
    snapshot.cells.push_back({{0, 1}, QStringLiteral("B"), 0U, 1, false, 2U});
    snapshot.cells.push_back({{0, 2}, QStringLiteral("C"), 0U, 1, false, 3U});
    snapshot.cells.push_back({{0, 3}, QStringLiteral("\u754c"), 0U, 2, false, 1U});
    snapshot.cells.push_back({{0, 4}, {}, 0U, 0, true, 1U});

    const term::Terminal_render_frame frame = build(snapshot);
    ok &= check(frame.text_runs.size() == 4U, "frame skips wide continuation text run");
    ok &= check(frame.text_runs[0].foreground == QColor(204, 0, 0) &&
        frame.text_runs[0].background == QColor(0, 48, 204),
        "frame resolves explicit foreground/background");
    ok &= check(frame.text_runs[1].foreground == QColor(0, 48, 204) &&
        frame.text_runs[1].background == QColor(204, 0, 0),
        "frame applies inverse colors");
    ok &= check(frame.text_runs[2].foreground == QColor(204, 0, 0, 128) &&
        frame.text_runs[2].background == QColor(0, 48, 204),
        "frame applies faint foreground");
    ok &= check(frame.text_runs[3].rect.width() == 20.0,
        "wide base cell spans display width");
    return ok;
}

bool test_terminal_graphics_use_grid_rects()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({3, 8});
    snapshot.cells.push_back({{0, 0}, QStringLiteral("\u250c"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 1}, QStringLiteral("\u2500"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 2}, QStringLiteral("\u2510"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{1, 0}, QStringLiteral("\u2502"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{1, 1}, QStringLiteral("\u2588"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{1, 2}, QStringLiteral("\u2598"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{2, 0}, QStringLiteral("A"), 0U, 1, false, 0U});

    const term::Terminal_render_frame frame = build(snapshot);
    ok &= check(frame.text_runs.size() == 1U &&
        frame.text_runs.front().text == QStringLiteral("A"),
        "terminal graphic cells bypass font text runs");
    ok &= check(frame.stats.packed_graphic_cells >= 2 &&
        has_packed_graphic_codepoint(frame, 0x2588U) &&
        has_packed_graphic_codepoint(frame, 0x2598U),
        "terminal graphic cells preserve hard block source codepoints in packed sidecars");
    ok &= check(has_graphic_rect(frame, QRectF(10.0, 9.5, 10.0, 1.0)),
        "horizontal box drawing remains in direct graphic rectangles");
    ok &= check(has_graphic_rect_antialias(frame, QRectF(10.0, 9.5, 10.0, 1.0), true),
        "box drawing line strokes request antialiased rendering");
    ok &= check(!has_graphic_rect(frame, QRectF(10.0, 20.0, 10.0, 20.0)),
        "packed full block is not duplicated into direct graphic rectangles");
    ok &= check(!has_graphic_rect(frame, QRectF(20.0, 20.0, 5.0, 10.0)),
        "packed quadrant block is not duplicated into direct graphic rectangles");
    return ok;
}

bool test_rounded_box_corners_use_arc_primitives()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({1, 4});
    snapshot.cells.push_back({{0, 0}, QStringLiteral("\u256d"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 1}, QStringLiteral("\u256e"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 2}, QStringLiteral("\u256f"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 3}, QStringLiteral("\u2570"), 0U, 1, false, 0U});

    const term::Terminal_render_frame frame = build(snapshot);
    ok &= check(frame.text_runs.empty(),
        "rounded box corners bypass font text runs");
    ok &= check(frame.graphic_rects.empty(),
        "rounded box corners do not fall back to square corner rectangles");
    ok &= check(frame.graphic_arcs.size() == 4U,
        "rounded box corners produce arc primitives");
    ok &= check(has_graphic_arc(
        frame,
        term::Terminal_render_arc_kind::DOWN_RIGHT,
        QRectF(0.0, 0.0, 10.0, 20.0),
        1.0),
        "rounded down-right corner produces a down-right arc");
    ok &= check(has_graphic_arc(
        frame,
        term::Terminal_render_arc_kind::DOWN_LEFT,
        QRectF(10.0, 0.0, 10.0, 20.0),
        1.0),
        "rounded down-left corner produces a down-left arc");
    ok &= check(has_graphic_arc(
        frame,
        term::Terminal_render_arc_kind::UP_LEFT,
        QRectF(20.0, 0.0, 10.0, 20.0),
        1.0),
        "rounded up-left corner produces an up-left arc");
    ok &= check(has_graphic_arc(
        frame,
        term::Terminal_render_arc_kind::UP_RIGHT,
        QRectF(30.0, 0.0, 10.0, 20.0),
        1.0),
        "rounded up-right corner produces an up-right arc");
    return ok;
}

bool test_block_cursor_over_graphic_has_overlay_rects()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({2, 4});
    snapshot.cursor = {{0, 1}, term::Terminal_cursor_shape::BLOCK, true, false};
    snapshot.cells.push_back({{0, 1}, QStringLiteral("\u2500"), 0U, 1, false, 0U});

    const term::Terminal_render_frame frame = build(snapshot);
    ok &= check(frame.text_runs.empty(),
        "cursor-covered primitive graphic still bypasses main font text runs");
    ok &= check(frame.cursors.size() == 2U &&
        frame.cursors[0].kind == term::Terminal_cursor_shape::BLOCK &&
        frame.cursors[1].kind == term::Terminal_cursor_shape::BLOCK,
        "block cursor over primitive graphic is split around the graphic overlay");
    ok &= check(frame.graphic_rects.size() == 1U &&
        frame.graphic_rects.front().rect == QRectF(10.0, 9.5, 10.0, 1.0),
        "primitive graphic remains in the normal graphic layer");
    ok &= check(frame.cursor_graphic_rects.size() == 1U &&
        frame.cursor_graphic_rects.front().rect == frame.graphic_rects.front().rect &&
        frame.cursor_graphic_rects.front().antialias &&
        frame.cursor_graphic_rects.front().color.red() == 9 &&
        frame.cursor_graphic_rects.front().color.green() == 12 &&
        frame.cursor_graphic_rects.front().color.blue() == 16 &&
        frame.cursor_graphic_rects.front().color.alpha() == 254,
        "block cursor over primitive graphic emits a contrasting graphic overlay");
    ok &= check(frame.cursor_text_runs.empty(),
        "primitive graphic cursor overlay does not synthesize font cursor text");
    return ok;
}

bool test_mixed_unicode_row_geometry()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({2, 14});
    snapshot.cells.push_back({{0, 0}, QStringLiteral("A"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 1}, QStringLiteral("\u754c"), 0U, 2, false, 0U});
    snapshot.cells.push_back({{0, 2}, {}, 0U, 0, true, 0U});
    snapshot.cells.push_back({{0, 3}, QStringLiteral("Z"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 4}, QStringLiteral("e\u0301"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 5}, QString::fromUcs4(U"\u2764\ufe0e"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 6}, QString::fromUcs4(U"\u2764\ufe0f"), 0U, 2, false, 0U});
    snapshot.cells.push_back({{0, 7}, {}, 0U, 0, true, 0U});
    snapshot.cells.push_back({{0, 8}, QString::fromUcs4(U"\U0001f600"), 0U, 2, false, 0U});
    snapshot.cells.push_back({{0, 9}, {}, 0U, 0, true, 0U});
    snapshot.cells.push_back({{0, 10}, QStringLiteral("\u03a9"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 11}, QStringLiteral("S"), 0U, 1, false, 0U});

    const term::Terminal_render_frame frame = build(snapshot);
    ok &= check(frame.text_runs.size() == 9U,
        "mixed row skips wide continuations but keeps base clusters");
    ok &= check(run_at(frame, 0, 0) != nullptr &&
        run_at(frame, 0, 0)->rect == QRectF(0.0, 0.0, 10.0, 20.0),
        "ASCII run stays in column 0");
    ok &= check(run_at(frame, 0, 1) != nullptr &&
        run_at(frame, 0, 1)->rect == QRectF(10.0, 0.0, 20.0, 20.0),
        "CJK wide run spans columns 1-2");
    ok &= check(run_at(frame, 0, 3) != nullptr &&
        run_at(frame, 0, 3)->rect == QRectF(30.0, 0.0, 10.0, 20.0),
        "ASCII sentinel after CJK stays aligned");
    ok &= check(run_at(frame, 0, 4) != nullptr &&
        run_at(frame, 0, 4)->rect == QRectF(40.0, 0.0, 10.0, 20.0),
        "combining cluster stays one cell");
    ok &= check(run_at(frame, 0, 5) != nullptr &&
        run_at(frame, 0, 5)->rect == QRectF(50.0, 0.0, 10.0, 20.0),
        "VS15 heart stays one cell");
    ok &= check(run_at(frame, 0, 6) != nullptr &&
        run_at(frame, 0, 6)->rect == QRectF(60.0, 0.0, 20.0, 20.0),
        "VS16 heart spans two cells");
    ok &= check(run_at(frame, 0, 8) != nullptr &&
        run_at(frame, 0, 8)->rect == QRectF(80.0, 0.0, 20.0, 20.0),
        "default emoji spans two cells");
    ok &= check(run_at(frame, 0, 10) != nullptr &&
        run_at(frame, 0, 10)->rect == QRectF(100.0, 0.0, 10.0, 20.0),
        "ambiguous omega stays one cell");
    ok &= check(run_at(frame, 0, 11) != nullptr &&
        run_at(frame, 0, 11)->rect == QRectF(110.0, 0.0, 10.0, 20.0),
        "final ASCII sentinel stays aligned");
    return ok;
}

bool test_single_cell_unicode_text_is_not_clipped_to_cell()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({1, 4});
    snapshot.cells.push_back({{0, 0}, QStringLiteral("\u26a0"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 1}, QStringLiteral("H"), 0U, 1, false, 0U});

    const term::Terminal_render_frame frame = build(snapshot);
    const term::Terminal_render_text_run* warning  = run_at(frame, 0, 0);
    const term::Terminal_render_text_run* sentinel = run_at(frame, 0, 1);
    ok &= check(warning != nullptr &&
        warning->rect == QRectF(0.0, 0.0, 10.0, 20.0) &&
        !warning->clip_rect.isValid(),
        "single-cell Unicode warning sign remains unclipped");
    ok &= check(sentinel != nullptr &&
        sentinel->text == QStringLiteral("H") &&
        sentinel->rect == QRectF(10.0, 0.0, 10.0, 20.0) &&
        !sentinel->clip_rect.isValid(),
        "ASCII sentinel after warning sign stays unclipped in the next cell");

    return ok;
}

bool test_direct_ascii_text_runs_coalesce_contiguous_plain_cells()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({1, 6});
    snapshot.cursor.visible = false;
    snapshot.cells.push_back({.position = {0, 0}, .text = QStringLiteral("A")});
    snapshot.cells.push_back({.position = {0, 1}, .text = QStringLiteral("B")});
    snapshot.cells.push_back({.position = {0, 2}, .text = QStringLiteral("C")});

    const term::Terminal_render_frame frame = build(snapshot);
    ok &= check(frame.text_runs.size() == 1U &&
        frame.text_runs.front().row == 0 &&
        frame.text_runs.front().column == 0 &&
        frame.text_runs.front().text == QStringLiteral("ABC") &&
        frame.text_runs.front().rect == QRectF(0.0, 0.0, 30.0, 20.0),
        "contiguous same-style printable ASCII emits one coalesced text run");
    ok &= check(frame.stats.text_runs_emitted == 1 &&
        frame.stats.text_cells_rendered_as_text == 3,
        "coalesced direct ASCII still counts the rendered source cells");
    return ok;
}

bool test_direct_ascii_text_runs_split_at_guard_boundaries()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({5, 8});

    term::Terminal_text_style accent = term::make_default_terminal_text_style();
    accent.foreground = term::make_rgb_terminal_color_ref(0xffcc1010U);
    accent.background = term::make_rgb_terminal_color_ref(0xff1030ccU);
    snapshot.styles.push_back(accent);

    term::Terminal_text_style decorated = term::make_default_terminal_text_style();
    term::set_terminal_style_attribute(decorated, term::Terminal_style_attribute::UNDERLINE);
    snapshot.styles.push_back(decorated);

    snapshot.hyperlinks.push_back({17U, QByteArrayLiteral("uri:https://example.test"),
        QByteArrayLiteral("https://example.test")});
    snapshot.cursor = {{4, 1}, term::Terminal_cursor_shape::BLOCK, true, false};

    const auto add_cell = [&](
        int                       row,
        int                       column,
        const QString&            text,
        term::Terminal_style_id   style_id = 0U,
        std::uint64_t             hyperlink_id = 0U) {
        term::Terminal_render_cell cell;
        cell.position     = {row, column};
        cell.text         = text;
        cell.hyperlink_id = hyperlink_id;
        cell.style_id     = style_id;
        snapshot.cells.push_back(cell);
    };

    add_cell(0, 0, QStringLiteral("A"));
    add_cell(0, 1, QStringLiteral("B"), 1U);
    add_cell(0, 2, QStringLiteral("C"));
    add_cell(1, 0, QStringLiteral("D"));
    add_cell(1, 1, QStringLiteral("E"), 2U);
    add_cell(1, 2, QStringLiteral("F"));
    add_cell(2, 0, QStringLiteral("G"));
    add_cell(2, 1, QStringLiteral("H"), 0U, 17U);
    add_cell(2, 2, QStringLiteral("I"));
    add_cell(3, 0, QStringLiteral("J"));
    add_cell(3, 2, QStringLiteral("K"));
    add_cell(4, 0, QStringLiteral("L"));
    add_cell(4, 1, QStringLiteral("M"));
    add_cell(4, 2, QStringLiteral("N"));

    const term::Terminal_render_frame frame = build(snapshot);

    const std::vector<const term::Terminal_render_text_run*> style_runs =
        text_runs_for_row(frame, 0);
    ok &= check(style_runs.size() == 3U &&
        style_runs[0]->text == QStringLiteral("A") &&
        style_runs[1]->text == QStringLiteral("B") &&
        style_runs[1]->style_id == 1U &&
        style_runs[1]->foreground == QColor(204, 16, 16) &&
        style_runs[1]->background == QColor(16, 48, 204) &&
        style_runs[2]->text == QStringLiteral("C"),
        "direct ASCII coalescing splits at foreground/background/style boundaries");

    const std::vector<const term::Terminal_render_text_run*> decoration_runs =
        text_runs_for_row(frame, 1);
    ok &= check(decoration_runs.size() == 3U &&
        decoration_runs[1]->text == QStringLiteral("E") &&
        decoration_runs[1]->underline,
        "direct ASCII coalescing splits at decoration boundaries");

    const std::vector<const term::Terminal_render_text_run*> hyperlink_runs =
        text_runs_for_row(frame, 2);
    ok &= check(hyperlink_runs.size() == 3U &&
        hyperlink_runs[1]->text == QStringLiteral("H") &&
        hyperlink_runs[1]->hyperlink_id == 17U,
        "direct ASCII coalescing splits at hyperlink boundaries");

    const std::vector<const term::Terminal_render_text_run*> gap_runs =
        text_runs_for_row(frame, 3);
    ok &= check(gap_runs.size() == 2U &&
        gap_runs[0]->text == QStringLiteral("J") &&
        gap_runs[1]->column == 2 &&
        gap_runs[1]->text == QStringLiteral("K"),
        "direct ASCII coalescing splits across column gaps");

    const std::vector<const term::Terminal_render_text_run*> cursor_runs =
        text_runs_for_row(frame, 4);
    ok &= check(cursor_runs.size() == 3U &&
        cursor_runs[0]->text == QStringLiteral("L") &&
        cursor_runs[1]->text == QStringLiteral("M") &&
        cursor_runs[2]->text == QStringLiteral("N"),
        "direct ASCII coalescing splits around a block-cursor-covered cell");
    ok &= check(frame.cursor_text_runs.size() == 1U &&
        frame.cursor_text_runs.front().text == QStringLiteral("M"),
        "block cursor overlay copies only the protected standalone cell");
    return ok;
}

bool test_direct_ascii_text_runs_split_at_max_coalesced_length()
{
    constexpr int k_expected_max_coalesced_ascii_text_run_length = 384;

    bool ok = true;
    term::Terminal_render_snapshot snapshot =
        empty_snapshot({1, k_expected_max_coalesced_ascii_text_run_length + 1});
    snapshot.cursor.visible = false;
    for (int column = 0; column <= k_expected_max_coalesced_ascii_text_run_length; ++column) {
        snapshot.cells.push_back({.position = {0, column}, .text = QStringLiteral("A")});
    }

    const term::Terminal_render_frame frame = build(snapshot);
    ok &= check(frame.text_runs.size() == 2U,
        "direct ASCII coalescing caps the maximum text-run length");
    ok &= check(frame.text_runs.size() == 2U &&
        frame.text_runs[0].column == 0 &&
        frame.text_runs[0].text.size() == k_expected_max_coalesced_ascii_text_run_length &&
        frame.text_runs[0].rect.width() ==
            metrics().width * k_expected_max_coalesced_ascii_text_run_length &&
        frame.text_runs[1].column == k_expected_max_coalesced_ascii_text_run_length &&
        frame.text_runs[1].text == QStringLiteral("A"),
        "direct ASCII coalescing starts a new run at the configured length cap");
    return ok;
}

bool test_inverse_and_reverse_video_xor()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({3, 16});

    term::Terminal_text_style colored = term::make_default_terminal_text_style();
    colored.foreground = term::make_rgb_terminal_color_ref(0xffcc0000U);
    colored.background = term::make_rgb_terminal_color_ref(0xff0030ccU);
    snapshot.styles.push_back(colored);

    term::Terminal_text_style inverse = colored;
    term::set_terminal_style_attribute(inverse, term::Terminal_style_attribute::INVERSE);
    snapshot.styles.push_back(inverse);

    snapshot.cells.push_back({{0, 0}, QStringLiteral("A"), 0U, 1, false, 1U});
    snapshot.cells.push_back({{0, 1}, QStringLiteral("B"), 0U, 1, false, 2U});

    term::Terminal_render_frame frame = build(snapshot);
    ok &= check(frame.text_runs[0].foreground == QColor(204, 0, 0) &&
        frame.text_runs[0].background == QColor(0, 48, 204),
        "plain style does not invert without reverse video");
    ok &= check(frame.text_runs[1].foreground == QColor(0, 48, 204) &&
        frame.text_runs[1].background == QColor(204, 0, 0),
        "SGR inverse swaps colors");

    snapshot.modes.reverse_video  = true;
    frame                         = build(snapshot);
    ok                           &= check(frame.text_runs[0].foreground == QColor(0, 48, 204) &&
        frame.text_runs[0].background == QColor(204, 0, 0),
        "reverse video swaps plain style colors");
    ok                           &= check(frame.text_runs[1].foreground == QColor(204, 0, 0) &&
        frame.text_runs[1].background == QColor(0, 48, 204),
        "reverse video and SGR inverse cancel");
    return ok;
}

bool test_default_and_reverse_full_grid_background()
{
    bool ok = true;

    term::Terminal_render_snapshot snapshot       = empty_snapshot({3, 16});
    term::Terminal_render_options  render_options = options();
    render_options.default_background = QColor(1, 2, 3);

    term::Terminal_render_frame frame = build(snapshot, render_options);
    ok &= check(frame.background_rects.size() == 1U &&
        frame.background_rects.front().color == QColor(9, 12, 16),
        "empty snapshot uses terminal default background instead of surface fallback");

    snapshot.cells.push_back({{0, 0}, QStringLiteral("A"), 0U, 1, false, 0U});
    frame = build(snapshot, render_options);
    ok &= check(frame.background_rects.size() == 1U &&
        frame.background_rects.front().color == frame.text_runs.front().background,
        "blank cells share default-style occupied cell background");

    snapshot.modes.reverse_video  = true;
    frame                         = build(snapshot, render_options);
    ok                           &= check(frame.background_rects.size() == 1U &&
        frame.background_rects.front().color == QColor(196, 230, 201) &&
        frame.text_runs.front().background == QColor(196, 230, 201),
        "reverse video applies to full grid background including blanks");
    return ok;
}

bool test_styled_blank_cells_emit_background_rects()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({1, 8});
    snapshot.cursor.visible = false;

    term::Terminal_text_style highlighted = term::make_default_terminal_text_style();
    highlighted.background = term::make_rgb_terminal_color_ref(0xff143c78U);
    snapshot.styles.push_back(highlighted);

    snapshot.cells.push_back({{0, 0}, QStringLiteral("A"), 0U, 1, false, 1U});
    snapshot.cells.push_back({{0, 1}, QStringLiteral(" "), 0U, 1, false, 1U});
    snapshot.cells.push_back({{0, 2}, QStringLiteral(" "), 0U, 1, false, 1U});
    snapshot.cells.push_back({{0, 3}, QStringLiteral("D"), 0U, 1, false, 1U});

    const QColor highlight_color(20, 60, 120);
    const term::Terminal_render_frame frame = build(snapshot);
    ok &= check(has_background_rect(frame, QRectF(10.0, 0.0, 10.0, 20.0), highlight_color),
        "styled blank cell emits its own background rectangle");
    ok &= check(has_background_rect(frame, QRectF(20.0, 0.0, 10.0, 20.0), highlight_color),
        "adjacent styled blank cell emits its own background rectangle");
    ok &= check(frame.text_runs.size() == 1U &&
        frame.text_runs.front().text == QStringLiteral("A  D") &&
        frame.text_runs.front().rect.width() == 40.0,
        "styled blank cells remain represented in coalesced text runs");
    return ok;
}

bool test_selection_cursor_blink_and_shapes()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({4, 12});
    snapshot.selection_spans.push_back({
        { {1, 1}, {1, 5}, term::Terminal_selection_mode::NORMAL },
        1,
        1,
        4,
    });

    snapshot.cursor = {{2, 3}, term::Terminal_cursor_shape::BLOCK, true, true};
    term::Terminal_render_frame frame = build(snapshot, options(), false);
    ok &= check(frame.selection_rects.size() == 1U &&
        frame.selection_rects.front().rect == QRectF(10.0, 20.0, 40.0, 20.0),
        "selection span becomes grid-relative rect");
    ok &= check(frame.cursors.empty(), "blink-hidden cursor is omitted");

    snapshot.cursor.blink_enabled  = false;
    frame                          = build(snapshot, options(), false);
    ok                            &= check(frame.cursors.size() == 1U &&
        frame.cursors.front().kind == term::Terminal_cursor_shape::BLOCK,
        "blink-disabled cursor remains visible");

    snapshot.cells.push_back({{2, 3}, QStringLiteral("C"), 0U, 1, false, 0U});
    frame  = build(snapshot, options(), false);
    ok    &= check(frame.cursor_text_runs.size() == 1U &&
        frame.cursor_text_runs.front().text == QStringLiteral("C") &&
        frame.cursor_text_runs.front().foreground ==
            frame.cursor_text_runs.front().background,
        "block cursor duplicates covered glyph in contrasting color");
    ok    &= check(frame.cursor_text_runs.front().clip_rect == frame.cursors.front().rect,
        "block cursor text overlay is clipped to cursor cell");

    snapshot.cells.clear();
    snapshot.cursor = {{2, 3}, term::Terminal_cursor_shape::BLOCK, true, false};
    snapshot.cells.push_back({{2, 3}, QStringLiteral("\u754c"), 0U, 2, false, 0U});
    snapshot.cells.push_back({{2, 4}, {}, 0U, 0, true, 0U});
    frame = build(snapshot, options(), false);
    ok &= check(frame.cursor_text_runs.size() == 1U &&
        frame.cursor_text_runs.front().rect.width() == metrics().width * 2.0 &&
        frame.cursor_text_runs.front().clip_rect.width() == metrics().width,
        "wide glyph cursor overlay clips base glyph to one cursor cell");

    snapshot.cursor.position  = {2, 4};
    frame                     = build(snapshot, options(), false);
    ok                       &= check(frame.cursors.size() == 1U &&
        frame.cursor_text_runs.size() == 1U &&
        frame.cursor_text_runs.front().text == QStringLiteral("\u754c") &&
        frame.cursor_text_runs.front().rect == QRectF(30.0, 40.0, 20.0, 20.0) &&
        frame.cursor_text_runs.front().clip_rect == QRectF(40.0, 40.0, 10.0, 20.0),
        "wide glyph continuation cursor clips the intersecting base glyph to the cursor cell");

    snapshot.cursor  = {{2, 3}, term::Terminal_cursor_shape::BAR, true, false};
    frame            = build(snapshot);
    ok              &= check(frame.cursors.front().kind == term::Terminal_cursor_shape::BAR &&
        frame.cursors.front().rect.width() < metrics().width,
        "bar cursor is narrow");

    snapshot.cursor  = {{2, 3}, term::Terminal_cursor_shape::UNDERLINE, true, false};
    frame            = build(snapshot);
    ok              &= check(frame.cursors.front().kind == term::Terminal_cursor_shape::UNDERLINE &&
        frame.cursors.front().rect.height() < metrics().height,
        "underline cursor is short");
    return ok;
}

bool test_decorations_preedit_hyperlink_and_bell()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({4, 12});

    term::Terminal_text_style decorated = term::make_default_terminal_text_style();
    term::set_terminal_style_attribute(decorated, term::Terminal_style_attribute::UNDERLINE);
    term::set_terminal_style_attribute(decorated, term::Terminal_style_attribute::STRIKE);
    snapshot.styles.push_back(decorated);
    snapshot.cells.push_back({{0, 0}, QStringLiteral("U"), 7U, 1, false, 1U});
    snapshot.hyperlinks.push_back({7U, QByteArrayLiteral("uri:https://example.test"),
        QByteArrayLiteral("https://example.test")});
    snapshot.selection_spans.push_back({
        { {1, 1}, {1, 5}, term::Terminal_selection_mode::NORMAL },
        1,
        1,
        4,
    });
    snapshot.cursor.position             = {1, 2};
    snapshot.ime_preedit.text            = QStringLiteral("ime");
    snapshot.ime_preedit.cursor_position = 2;
    snapshot.ime_preedit.active          = true;
    snapshot.metadata.visual_bell_active = true;

    term::Terminal_render_options render_options = options();
    render_options.underline_hyperlinks = false;
    term::Terminal_render_frame frame = build(snapshot, render_options);

    ok &= check(frame.decorations.size() == 3U,
        "underline, strike, and preedit caret decorations are present");
    ok &= check(frame.selection_rects.size() == 2U &&
        frame.selection_rects[0].rect == QRectF(10.0, 20.0, 40.0, 20.0) &&
        frame.selection_rects[1].rect == QRectF(20.0, 20.0, 30.0, 20.0),
        "preedit background is emitted after overlapping selection background");
    ok &= check(frame.overlay_rects.size() == 1U, "visual bell adds one overlay");
    ok &= check(frame.text_runs.size() == 2U &&
        frame.text_runs.back().text == QStringLiteral("ime"),
        "preedit text is appended as overlay text run");

    render_options.underline_hyperlinks  = true;
    frame                                = build(snapshot, render_options);
    ok                                  &= check(frame.decorations.size() == 4U,
        "hyperlink underline policy adds decoration");

    render_options.visual_bell_enabled  = false;
    frame                               = build(snapshot, render_options);
    ok                                 &= check(frame.overlay_rects.empty(), "visual bell policy suppresses overlay");
    return ok;
}

bool test_malformed_snapshot_and_preedit_width()
{
    bool ok = true;
    term::Terminal_render_snapshot zero_grid = empty_snapshot({0, 0});
    zero_grid.cursor.position                = {0, 0};
    zero_grid.cursor.visible                 = true;
    zero_grid.ime_preedit.active             = true;
    zero_grid.ime_preedit.text               = QStringLiteral("\u754c");
    term::Terminal_render_frame frame = build(zero_grid);
    ok &= check(frame.text_runs.empty() &&
        frame.selection_rects.empty() &&
        frame.decorations.empty() &&
        frame.cursors.empty(),
        "zero-grid active preedit creates no primitives");

    term::Terminal_render_snapshot snapshot = empty_snapshot({2, 4});
    snapshot.cells.push_back({{0, 4}, QStringLiteral("X"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{2, 0}, QStringLiteral("Y"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 3}, QStringLiteral("\u754c"), 0U, 2, false, 0U});
    snapshot.cells.push_back({{0, 1}, QStringLiteral("A"), 0U, 1, false, 0U});
    frame = build(snapshot);
    ok &= check(frame.text_runs.size() == 1U &&
        frame.text_runs.front().text == QStringLiteral("A"),
        "offscreen and over-wide cells are skipped");

    snapshot                              = empty_snapshot({2, 8});
    snapshot.cursor.position              = {0, 1};
    snapshot.ime_preedit.active           = true;
    snapshot.ime_preedit.text             = QStringLiteral("\u754c");
    snapshot.ime_preedit.cursor_position  = 1;
    frame                                 = build(snapshot);
    ok                                   &= check(frame.text_runs.size() == 1U &&
        frame.text_runs.front().rect.width() == metrics().width * 2.0,
        "wide CJK preedit uses display-column width");
    ok                                   &= check(frame.decorations.size() == 1U &&
        frame.decorations.front().rect.x() == metrics().width * 3.0,
        "wide CJK preedit caret uses display-cell prefix width");

    snapshot.ime_preedit.text             = QString::fromUcs4(U"\U0001f600");
    snapshot.ime_preedit.cursor_position  = 2;
    frame                                 = build(snapshot);
    ok                                   &= check(frame.text_runs.size() == 1U &&
        frame.text_runs.front().rect.width() == metrics().width * 2.0,
        "emoji preedit uses display-column width");
    ok                                   &= check(frame.decorations.size() == 1U &&
        frame.decorations.front().rect.x() == metrics().width * 3.0,
        "emoji preedit caret uses display-cell prefix width");

    snapshot                              = empty_snapshot({2, 4});
    snapshot.cursor.position              = {0, 3};
    snapshot.ime_preedit.active           = true;
    snapshot.ime_preedit.text             = QStringLiteral("ab");
    snapshot.ime_preedit.cursor_position  = 2;
    frame                                 = build(snapshot);
    ok                                   &= check(frame.decorations.size() == 1U &&
        frame.decorations.front().rect.x() == metrics().width * 3.0,
        "right-edge preedit caret remains inside the grid");

    snapshot                             = empty_snapshot({2, 8});
    snapshot.cursor.position             = {0, 1};
    snapshot.ime_preedit.active          = true;
    snapshot.ime_preedit.text            = QStringLiteral("abcd");
    snapshot.ime_preedit.cursor_position = 1;
    frame                                = build(snapshot);
    const qreal caret_after_a = frame.decorations.front().rect.x();

    snapshot.ime_preedit.cursor_position  = 3;
    frame                                 = build(snapshot);
    ok                                   &= check(caret_after_a == metrics().width * 2.0 &&
        frame.decorations.front().rect.x() == metrics().width * 4.0,
        "preedit caret position movement updates the emitted caret rectangle");
    return ok;
}

bool test_preedit_override_takes_precedence_over_snapshot()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({2, 8});
    snapshot.cursor.position                = {0, 1};
    snapshot.ime_preedit.active             = true;
    snapshot.ime_preedit.text               = QStringLiteral("stale");
    snapshot.ime_preedit.cursor_position    = 5;

    term::Ime_preedit_state cleared_preedit;
    term::Terminal_render_frame frame = build(snapshot, options(), true, &cleared_preedit);
    ok &= check(frame.text_runs.empty() && frame.decorations.empty(),
        "inactive preedit override suppresses stale snapshot preedit");

    term::Ime_preedit_state active_preedit;
    active_preedit.active           = true;
    active_preedit.text             = QStringLiteral("live");
    active_preedit.cursor_position  = 2;
    frame                           = build(snapshot, options(), true, &active_preedit);
    ok                             &= check(frame.text_runs.size() == 1U &&
        frame.text_runs.front().text == QStringLiteral("live"),
        "active preedit override supplies rendered preedit text");
    ok                             &= check(snapshot.ime_preedit.text == QStringLiteral("stale"),
        "preedit override leaves snapshot immutable");
    return ok;
}

bool test_frame_stats_classify_rendered_cells()
{
    bool ok = true;

    term::Terminal_render_snapshot snapshot   = empty_snapshot({3, 8});
    term::Terminal_text_style      underlined = term::make_default_terminal_text_style();
    underlined.attributes =
        term::terminal_style_attribute_mask(term::Terminal_style_attribute::UNDERLINE);
    snapshot.styles.push_back(underlined);

    snapshot.cells.push_back({
        .position = {0, 0},
        .text     = QStringLiteral("A"),
        .style_id = 1U,
    });
    snapshot.cells.push_back({
        .position = {0, 1},
        .text     = QStringLiteral("\u03c0"),
    });
    snapshot.cells.push_back({
        .position      = {0, 2},
        .text          = QStringLiteral("\u754c"),
        .display_width = 2,
    });
    snapshot.cells.push_back({
        .position = {1, 0},
        .text     = QStringLiteral("\u2588"),
    });
    snapshot.cells.push_back({
        .position          = {1, 1},
        .wide_continuation = true,
    });
    snapshot.cells.push_back({
        .position = {8, 0},
        .text     = QStringLiteral("Z"),
    });

    const term::Terminal_render_frame frame = build(snapshot);
    ok &= check(frame.stats.cells_considered == 6,
        "frame stats count all snapshot cells considered");
    ok &= check(frame.stats.cells_skipped_wide_continuation == 1 &&
        frame.stats.cells_skipped_invalid == 1 &&
        frame.stats.cells_rendered == 4,
        "frame stats distinguish skipped and rendered cells");
    ok &= check(frame.stats.text_cells_rendered_as_text == 3 &&
        frame.stats.text_cells_rendered_as_graphic == 1,
        "frame stats distinguish text and graphic cell rendering");
    ok &= check(frame.stats.text_cells_printable_ascii == 1 &&
        frame.stats.text_cells_simple_ascii == 1 &&
        frame.stats.text_cells_non_ascii == 3,
        "frame stats classify simple ASCII and non-ASCII cells");
    ok &= check(frame.stats.text_cells_single_width == 3 &&
        frame.stats.text_cells_multi_width == 1,
        "frame stats classify display width");
    ok &= check(frame.stats.text_cells_with_decorations == 1 &&
        frame.stats.text_distinct_styles == 2 &&
        frame.stats.text_style_changes == 1,
        "frame stats capture style and decoration churn");
    ok &= check(frame.stats.text_runs_emitted == 3 &&
        frame.stats.packed_graphic_cells == 1 &&
        frame.stats.graphic_rects_emitted == 0 &&
        frame.stats.decoration_rects_emitted == 1,
        "frame stats count emitted primitives");
    return ok;
}

bool test_simple_content_classifier_and_stats()
{
    bool ok = true;
    const term::terminal_grid_size_t grid_size{3, 6};

    term::Terminal_render_cell ascii_cell;
    ascii_cell.position = {0, 0};
    ascii_cell.text     = QStringLiteral("A");
    const term::terminal_simple_content_classification_t ascii_classification =
        term::classify_terminal_simple_content_cell(
            ascii_cell,
            grid_size,
            2U,
            false,
            true);
    ok &= check(ascii_classification.fast_text_eligible &&
        ascii_classification.route == term::Terminal_simple_content_route::FAST_TEXT &&
        ascii_classification.rejection_reason ==
            term::Terminal_simple_content_rejection_reason::NONE &&
        ascii_classification.text_category ==
            term::Terminal_simple_content_text_category::PRINTABLE_ASCII &&
        ascii_classification.dirty_row,
        "simple-content classifier accepts clean one-cell printable ASCII with fallback category");

    term::Terminal_render_cell cached_ascii_cell = ascii_cell;
    cached_ascii_cell.text_category =
        term::Terminal_render_cell_text_category::PRINTABLE_ASCII;
    const term::terminal_simple_content_classification_t cached_ascii_classification =
        term::classify_terminal_simple_content_cell(
            cached_ascii_cell,
            grid_size,
            2U,
            false,
            true);
    ok &= check(cached_ascii_classification.fast_text_eligible &&
        cached_ascii_classification.text_category ==
            term::Terminal_simple_content_text_category::PRINTABLE_ASCII,
        "simple-content classifier accepts cached printable ASCII category");

    term::Terminal_render_cell non_ascii_cell = ascii_cell;
    non_ascii_cell.text = QStringLiteral("\u03c0");
    const term::terminal_simple_content_classification_t non_ascii_classification =
        term::classify_terminal_simple_content_cell(
            non_ascii_cell,
            grid_size,
            2U,
            false,
            false);
    ok &= check(!non_ascii_classification.fast_text_eligible &&
        non_ascii_classification.route ==
            term::Terminal_simple_content_route::QT_TEXT_LAYOUT &&
        non_ascii_classification.rejection_reason ==
            term::Terminal_simple_content_rejection_reason::NON_ASCII_TEXT,
        "simple-content classifier keeps non-ASCII text on the Qt text route");

    term::Terminal_render_cell graphic_cell = ascii_cell;
    graphic_cell.text = QStringLiteral("\u2588");
    const term::terminal_simple_content_classification_t graphic_classification =
        term::classify_terminal_simple_content_cell(
            graphic_cell,
            grid_size,
            2U,
            false,
            false);
    ok &= check(!graphic_classification.fast_text_eligible &&
        graphic_classification.route ==
            term::Terminal_simple_content_route::GRAPHIC_GEOMETRY &&
        graphic_classification.rejection_reason ==
            term::Terminal_simple_content_rejection_reason::TERMINAL_GRAPHIC,
        "simple-content classifier sends terminal graphics to geometry");

    term::Terminal_render_cell hyperlink_cell = ascii_cell;
    hyperlink_cell.hyperlink_id = 9U;
    const term::terminal_simple_content_classification_t hyperlink_classification =
        term::classify_terminal_simple_content_cell(
            hyperlink_cell,
            grid_size,
            2U,
            false,
            false);
    ok &= check(!hyperlink_classification.fast_text_eligible &&
        hyperlink_classification.rejection_reason ==
            term::Terminal_simple_content_rejection_reason::HYPERLINK,
        "simple-content classifier rejects hyperlink cells");

    const term::terminal_simple_content_classification_t decorated_hyperlink_classification =
        term::classify_terminal_simple_content_cell(
            hyperlink_cell,
            grid_size,
            2U,
            true,
            false);
    ok &= check(!decorated_hyperlink_classification.fast_text_eligible &&
        decorated_hyperlink_classification.rejection_reason ==
            term::Terminal_simple_content_rejection_reason::HYPERLINK,
        "simple-content classifier reports decorated hyperlinks as hyperlink cells");

    const term::terminal_simple_content_classification_t decoration_classification =
        term::classify_terminal_simple_content_cell(
            ascii_cell,
            grid_size,
            2U,
            true,
            false);
    ok &= check(!decoration_classification.fast_text_eligible &&
        decoration_classification.rejection_reason ==
            term::Terminal_simple_content_rejection_reason::DECORATION,
        "simple-content classifier rejects decorated cells");

    term::Terminal_render_cell continuation_cell = ascii_cell;
    continuation_cell.wide_continuation = true;
    const term::terminal_simple_content_classification_t continuation_classification =
        term::classify_terminal_simple_content_cell(
            continuation_cell,
            grid_size,
            2U,
            false,
            false);
    ok &= check(!continuation_classification.fast_text_eligible &&
        continuation_classification.route == term::Terminal_simple_content_route::NONE &&
        continuation_classification.rejection_reason ==
            term::Terminal_simple_content_rejection_reason::WIDE_CONTINUATION,
        "simple-content classifier ignores wide continuations");

    term::Terminal_render_cell invalid_style_cell = ascii_cell;
    invalid_style_cell.style_id = 4U;
    const term::terminal_simple_content_classification_t invalid_style_classification =
        term::classify_terminal_simple_content_cell(
            invalid_style_cell,
            grid_size,
            2U,
            false,
            false);
    ok &= check(!invalid_style_classification.fast_text_eligible &&
        invalid_style_classification.route ==
            term::Terminal_simple_content_route::FALLBACK &&
        invalid_style_classification.rejection_reason ==
            term::Terminal_simple_content_rejection_reason::INVALID_STYLE_ID,
        "simple-content classifier rejects out-of-range styles");

    const term::terminal_simple_content_classification_t invalid_grid_classification =
        term::classify_terminal_simple_content_cell(
            ascii_cell,
            { 0, 6 },
            2U,
            false,
            false);
    ok &= check(!invalid_grid_classification.fast_text_eligible &&
        invalid_grid_classification.route ==
            term::Terminal_simple_content_route::FALLBACK &&
        invalid_grid_classification.rejection_reason ==
            term::Terminal_simple_content_rejection_reason::INVALID_GRID,
        "simple-content classifier rejects invalid grids");

    term::Terminal_render_cell invalid_position_cell = ascii_cell;
    invalid_position_cell.position = {3, 0};
    const term::terminal_simple_content_classification_t invalid_position_classification =
        term::classify_terminal_simple_content_cell(
            invalid_position_cell,
            grid_size,
            2U,
            false,
            false);
    ok &= check(!invalid_position_classification.fast_text_eligible &&
        invalid_position_classification.route ==
            term::Terminal_simple_content_route::FALLBACK &&
        invalid_position_classification.rejection_reason ==
            term::Terminal_simple_content_rejection_reason::INVALID_POSITION,
        "simple-content classifier rejects positions outside the grid");

    term::Terminal_render_cell invalid_display_width_cell = ascii_cell;
    invalid_display_width_cell.display_width = 0;
    const term::terminal_simple_content_classification_t invalid_display_width_classification =
        term::classify_terminal_simple_content_cell(
            invalid_display_width_cell,
            grid_size,
            2U,
            false,
            false);
    ok &= check(!invalid_display_width_classification.fast_text_eligible &&
        invalid_display_width_classification.route ==
            term::Terminal_simple_content_route::FALLBACK &&
        invalid_display_width_classification.rejection_reason ==
            term::Terminal_simple_content_rejection_reason::INVALID_DISPLAY_WIDTH,
        "simple-content classifier rejects invalid display widths");

    term::Terminal_render_cell invalid_encoding_cell = ascii_cell;
    invalid_encoding_cell.text = QString(QChar(0xd800));
    const term::terminal_simple_content_classification_t invalid_encoding_classification =
        term::classify_terminal_simple_content_cell(
            invalid_encoding_cell,
            grid_size,
            2U,
            false,
            false);
    ok &= check(!invalid_encoding_classification.fast_text_eligible &&
        invalid_encoding_classification.route ==
            term::Terminal_simple_content_route::QT_TEXT_LAYOUT &&
        invalid_encoding_classification.rejection_reason ==
            term::Terminal_simple_content_rejection_reason::INVALID_TEXT_ENCODING,
        "simple-content classifier rejects invalid UTF-16 source text");

    term::Terminal_render_cell invalid_text_width_cell = ascii_cell;
    invalid_text_width_cell.display_width = 2;
    const term::terminal_simple_content_classification_t invalid_text_width_classification =
        term::classify_terminal_simple_content_cell(
            invalid_text_width_cell,
            grid_size,
            2U,
            false,
            false);
    ok &= check(!invalid_text_width_classification.fast_text_eligible &&
        invalid_text_width_classification.route ==
            term::Terminal_simple_content_route::QT_TEXT_LAYOUT &&
        invalid_text_width_classification.rejection_reason ==
            term::Terminal_simple_content_rejection_reason::INVALID_TEXT_WIDTH,
        "simple-content classifier rejects text/display-width mismatches");

    term::Terminal_render_cell multi_cell_text_cell = ascii_cell;
    multi_cell_text_cell.text          = QStringLiteral("\u754c");
    multi_cell_text_cell.display_width = 2;
    const term::terminal_simple_content_classification_t multi_cell_text_classification =
        term::classify_terminal_simple_content_cell(
            multi_cell_text_cell,
            grid_size,
            2U,
            false,
            false);
    ok &= check(!multi_cell_text_classification.fast_text_eligible &&
        multi_cell_text_classification.route ==
            term::Terminal_simple_content_route::QT_TEXT_LAYOUT &&
        multi_cell_text_classification.rejection_reason ==
            term::Terminal_simple_content_rejection_reason::MULTI_CELL_TEXT,
        "simple-content classifier rejects multi-cell text for the text candidate path");

    term::Terminal_render_cell non_printable_ascii_cell = ascii_cell;
    non_printable_ascii_cell.text = QStringLiteral("A\n");
    const term::terminal_simple_content_classification_t non_printable_ascii_classification =
        term::classify_terminal_simple_content_cell(
            non_printable_ascii_cell,
            grid_size,
            2U,
            false,
            false);
    ok &= check(!non_printable_ascii_classification.fast_text_eligible &&
        non_printable_ascii_classification.route ==
            term::Terminal_simple_content_route::QT_TEXT_LAYOUT &&
        non_printable_ascii_classification.rejection_reason ==
            term::Terminal_simple_content_rejection_reason::NON_PRINTABLE_ASCII,
        "simple-content classifier rejects non-printable ASCII");

    term::Terminal_render_cell presentation_mismatch_cell = ascii_cell;
    presentation_mismatch_cell.text          = QString::fromUcs4(U"\u2764\ufe0f");
    presentation_mismatch_cell.display_width = 2;
    const term::terminal_simple_content_classification_t presentation_mismatch_classification =
        term::classify_terminal_simple_content_cell(
            presentation_mismatch_cell,
            grid_size,
            2U,
            false,
            false,
            term::Terminal_shaped_presentation_mode::TEXT);
    ok &= check(!presentation_mismatch_classification.fast_text_eligible &&
        presentation_mismatch_classification.route ==
            term::Terminal_simple_content_route::QT_TEXT_LAYOUT &&
        presentation_mismatch_classification.rejection_reason ==
            term::Terminal_simple_content_rejection_reason::INVALID_TEXT_WIDTH,
        "simple-content classifier rejects presentation-mode mismatches");

    term::Terminal_render_snapshot snapshot = empty_snapshot(grid_size);
    snapshot.styles.push_back(term::make_default_terminal_text_style());
    snapshot.dirty_row_ranges = {{0, 1}};
    snapshot.cells.push_back({.position = {0, 0}, .text = QStringLiteral("A")});
    snapshot.cells.push_back({.position = {0, 1}, .text = QStringLiteral("B")});
    snapshot.cells.push_back({
        .position = {1, 0},
        .text     = QStringLiteral("C"),
        .style_id = 1U,
    });
    snapshot.cells.push_back({.position = {1, 1}, .text = QStringLiteral("\u03c0")});
    snapshot.cells.push_back({.position = {1, 2}, .text = QStringLiteral("\u2588")});
    snapshot.cells.push_back({.position = {2, 0}});
    snapshot.cells.push_back({
        .position     = {2, 1},
        .text         = QStringLiteral("H"),
        .hyperlink_id = 11U,
    });

    const term::Terminal_render_frame stats_frame = build(snapshot);
    const term::terminal_simple_content_stats_t& simple_stats =
        stats_frame.stats.simple_content;
    ok &= check(simple_stats.cells_considered == 7 &&
        simple_stats.eligible_cells == 3 &&
        simple_stats.eligible_after_all_gates_cells == 2,
        "simple-content frame stats distinguish candidates from cursor-covered cells");
    ok &= check(simple_stats.rows_with_eligible_cells == 2 &&
        simple_stats.styles_with_eligible_cells == 2 &&
        simple_stats.dirty_eligible_cells == 2 &&
        simple_stats.clean_eligible_cells == 1,
        "simple-content frame stats count eligible rows, styles, and dirty state");
    ok &= check(simple_stats.text_category_printable_ascii_cells == 4 &&
        simple_stats.text_category_non_ascii_cells == 2 &&
        simple_stats.text_category_empty_cells == 1,
        "simple-content frame stats classify text categories");
    ok &= check(simple_stats.route_fast_text_cells == 3 &&
        simple_stats.route_qt_text_layout_cells == 2 &&
        simple_stats.route_graphic_geometry_cells == 1 &&
        simple_stats.route_none_cells == 1,
        "simple-content frame stats count canonical routes");
    ok &= check(simple_stats.rejection_none_cells == 3 &&
        simple_stats.rejection_non_ascii_text_cells == 1 &&
        simple_stats.rejection_terminal_graphic_cells == 1 &&
        simple_stats.rejection_empty_text_cells == 1 &&
        simple_stats.rejection_hyperlink_cells == 1,
        "simple-content frame stats count rejection reasons");

    term::Terminal_render_options hyperlink_options = options();
    hyperlink_options.underline_hyperlinks = true;
    const term::Terminal_render_frame hyperlink_underlined_frame =
        build(snapshot, hyperlink_options);
    ok &= check(hyperlink_underlined_frame.stats.simple_content.rejection_hyperlink_cells ==
        simple_stats.rejection_hyperlink_cells &&
            hyperlink_underlined_frame.stats.simple_content.rejection_decoration_cells ==
        simple_stats.rejection_decoration_cells,
        "simple-content hyperlink rejection volume is stable across underline policy");

    term::Terminal_render_snapshot ime_snapshot = empty_snapshot({1, 4});
    ime_snapshot.cursor.visible     = false;
    ime_snapshot.cursor.position    = {0, 1};
    ime_snapshot.ime_preedit.active = true;
    ime_snapshot.ime_preedit.text   = QStringLiteral("xy");
    ime_snapshot.cells.push_back({.position = {0, 0}, .text = QStringLiteral("A")});
    ime_snapshot.cells.push_back({.position = {0, 1}, .text = QStringLiteral("B")});
    ime_snapshot.cells.push_back({.position = {0, 2}, .text = QStringLiteral("C")});

    const term::Terminal_render_frame ime_stats_frame = build(ime_snapshot);
    ok &= check(ime_stats_frame.stats.simple_content.eligible_cells == 3 &&
        ime_stats_frame.stats.simple_content.eligible_after_all_gates_cells == 1,
        "simple-content frame stats exclude IME-covered candidates after all gates");
    return ok;
}

bool test_frame_stats_count_distinct_rows_and_styles_by_identity()
{
    bool ok = true;

    term::Terminal_render_snapshot snapshot = empty_snapshot({3, 4});
    snapshot.styles.push_back(term::make_default_terminal_text_style());
    snapshot.cells.push_back({.position = {0, 0}, .text = QStringLiteral("A")});
    snapshot.cells.push_back({.position = {0, 1}, .text = QStringLiteral("B")});
    snapshot.cells.push_back({
        .position = {1, 0},
        .text     = QStringLiteral("C"),
        .style_id = 1U,
    });
    snapshot.cells.push_back({.position = {2, 0}, .text = QStringLiteral("D")});

    const term::Terminal_render_frame frame = build(snapshot);
    ok &= check(frame.stats.text_distinct_styles == 2 &&
        frame.stats.text_style_changes == 2,
        "frame stats count distinct styles by identity, not transitions");
    ok &= check(frame.stats.simple_content.rows_with_eligible_cells == 3 &&
        frame.stats.simple_content.styles_with_eligible_cells == 2,
        "simple-content stats count each eligible row and style once");

    term::Terminal_render_snapshot empty_styles_snapshot = empty_snapshot({1, 2});
    empty_styles_snapshot.styles.clear();
    empty_styles_snapshot.cells.push_back({
        .position = {0, 0},
        .text     = QStringLiteral("A"),
    });

    const term::Terminal_render_frame empty_styles_frame = build(empty_styles_snapshot);
    ok &= check(empty_styles_frame.stats.text_distinct_styles == 0 &&
        empty_styles_frame.stats.simple_content.eligible_cells == 0 &&
        empty_styles_frame.stats.simple_content.rows_with_eligible_cells == 0 &&
        empty_styles_frame.stats.simple_content.styles_with_eligible_cells == 0,
        "frame stats keep empty-style snapshots away from byte-flag indexing");

    return ok;
}

bool test_classify_terminal_simple_content_cell_unicode_property()
{
    bool ok = true;
    const term::terminal_grid_size_t grid_size{3, 8};

    term::Terminal_render_cell ascii_cell;
    ascii_cell.position = {0, 0};
    ascii_cell.text     = QStringLiteral("A");
    const std::vector<QString> strict_ascii_fast_texts = {
        QString(QChar(0x0020)),
        QStringLiteral("A"),
        QString(QChar(0x007e)),
    };
    const bool dirty_row_inputs[] = {false, true};
    for (const QString& text : strict_ascii_fast_texts) {
        for (const bool dirty_row : dirty_row_inputs) {
            term::Terminal_render_cell strict_ascii_cell = ascii_cell;
            strict_ascii_cell.position = {2, 4};
            strict_ascii_cell.text     = text;
            strict_ascii_cell.style_id = 1U;
            const term::terminal_simple_content_classification_t ascii_classification =
                term::classify_terminal_simple_content_cell(
                    strict_ascii_cell,
                    grid_size,
                    2U,
                    false,
                    dirty_row);
            ok &= check(ascii_classification.fast_text_eligible &&
                ascii_classification.route == term::Terminal_simple_content_route::FAST_TEXT &&
                ascii_classification.rejection_reason ==
                    term::Terminal_simple_content_rejection_reason::NONE &&
                ascii_classification.text_category ==
                    term::Terminal_simple_content_text_category::PRINTABLE_ASCII &&
                ascii_classification.valid_terminal_cell &&
                ascii_classification.row == strict_ascii_cell.position.row &&
                ascii_classification.style_id == strict_ascii_cell.style_id &&
                ascii_classification.dirty_row == dirty_row,
                "strict single-code-unit printable ASCII remains the only fast-text corpus case");
        }
    }

    const auto check_strict_ascii_gate_ordering = [&](
        const term::Terminal_render_cell&                 cell,
        term::terminal_grid_size_t                        case_grid_size,
        std::size_t                                       style_count,
        term::Terminal_simple_content_route               expected_route,
        term::Terminal_simple_content_rejection_reason    expected_reason,
        const char*                                       message) {
        const term::terminal_simple_content_classification_t classification =
            term::classify_terminal_simple_content_cell(
                cell,
                case_grid_size,
                style_count,
                false,
                false);
        return check(!classification.fast_text_eligible &&
            classification.route == expected_route &&
            classification.rejection_reason == expected_reason &&
            classification.text_category ==
                term::Terminal_simple_content_text_category::PRINTABLE_ASCII,
            message);
    };

    term::Terminal_render_cell invalid_grid_cell = ascii_cell;
    invalid_grid_cell.text = QStringLiteral("A");
    ok &= check_strict_ascii_gate_ordering(
        invalid_grid_cell,
        {0, 8},
        1U,
        term::Terminal_simple_content_route::FALLBACK,
        term::Terminal_simple_content_rejection_reason::INVALID_GRID,
        "strict ASCII bypass keeps invalid-grid rejection ahead of fast text");

    term::Terminal_render_cell invalid_position_cell = ascii_cell;
    invalid_position_cell.text     = QStringLiteral("A");
    invalid_position_cell.position = {3, 0};
    ok &= check_strict_ascii_gate_ordering(
        invalid_position_cell,
        grid_size,
        1U,
        term::Terminal_simple_content_route::FALLBACK,
        term::Terminal_simple_content_rejection_reason::INVALID_POSITION,
        "strict ASCII bypass keeps invalid-position rejection ahead of fast text");

    term::Terminal_render_cell invalid_style_cell = ascii_cell;
    invalid_style_cell.text     = QStringLiteral("A");
    invalid_style_cell.style_id = 1U;
    ok &= check_strict_ascii_gate_ordering(
        invalid_style_cell,
        grid_size,
        1U,
        term::Terminal_simple_content_route::FALLBACK,
        term::Terminal_simple_content_rejection_reason::INVALID_STYLE_ID,
        "strict ASCII bypass keeps invalid-style rejection ahead of fast text");

    term::Terminal_render_cell continuation_cell = ascii_cell;
    continuation_cell.text              = QStringLiteral("A");
    continuation_cell.wide_continuation = true;
    ok &= check_strict_ascii_gate_ordering(
        continuation_cell,
        grid_size,
        1U,
        term::Terminal_simple_content_route::NONE,
        term::Terminal_simple_content_rejection_reason::WIDE_CONTINUATION,
        "strict ASCII bypass keeps wide-continuation rejection ahead of fast text");

    term::Terminal_render_cell invalid_display_width_cell = ascii_cell;
    invalid_display_width_cell.text          = QStringLiteral("A");
    invalid_display_width_cell.display_width = 0;
    ok &= check_strict_ascii_gate_ordering(
        invalid_display_width_cell,
        grid_size,
        1U,
        term::Terminal_simple_content_route::FALLBACK,
        term::Terminal_simple_content_rejection_reason::INVALID_DISPLAY_WIDTH,
        "strict ASCII bypass keeps invalid-display-width rejection ahead of fast text");

    term::Terminal_render_cell out_of_row_width_cell = ascii_cell;
    out_of_row_width_cell.text          = QStringLiteral("A");
    out_of_row_width_cell.position      = {0, 7};
    out_of_row_width_cell.display_width = 2;
    ok &= check_strict_ascii_gate_ordering(
        out_of_row_width_cell,
        grid_size,
        1U,
        term::Terminal_simple_content_route::FALLBACK,
        term::Terminal_simple_content_rejection_reason::INVALID_DISPLAY_WIDTH,
        "strict ASCII bypass keeps out-of-row width rejection ahead of fast text");

    struct rejection_case_t
    {
        const char*       message       = "";
        QString           text;
        int               display_width = 1;
        std::uint64_t     hyperlink_id  = 0U;
        bool              has_decoration = false;
        bool              check_reason   = false;
        term::Terminal_simple_content_rejection_reason
                          expected_reason =
                              term::Terminal_simple_content_rejection_reason::NONE;
    };

    const std::vector<rejection_case_t> cases = {
        {
            "tab rejects fast text",
            QString(QChar(0x0009)),
            1,
            0U,
            false,
            true,
            term::Terminal_simple_content_rejection_reason::INVALID_TEXT_WIDTH,
        },
        {
            "newline rejects fast text",
            QString(QChar(0x000a)),
            1,
            0U,
            false,
            true,
            term::Terminal_simple_content_rejection_reason::INVALID_TEXT_WIDTH,
        },
        {
            "U+001F rejects fast text below printable ASCII",
            QString(QChar(0x001f)),
            1,
            0U,
            false,
            true,
            term::Terminal_simple_content_rejection_reason::INVALID_TEXT_WIDTH,
        },
        {
            "DEL rejects fast text",
            QString(QChar(0x007f)),
            1,
            0U,
            false,
            true,
            term::Terminal_simple_content_rejection_reason::INVALID_TEXT_WIDTH,
        },
        {
            "U+0080 rejects fast text just above ASCII",
            QString(QChar(0x0080)),
            1,
            0U,
            false,
            true,
            term::Terminal_simple_content_rejection_reason::INVALID_TEXT_WIDTH,
        },
        {
            "multi-code-unit ASCII rejects fast text",
            QStringLiteral("AB"),
            1,
            0U,
            false,
            true,
            term::Terminal_simple_content_rejection_reason::INVALID_TEXT_WIDTH,
        },
        {
            "multi-cell ASCII rejects fast text",
            QStringLiteral("AB"),
            2,
            0U,
            false,
            true,
            term::Terminal_simple_content_rejection_reason::MULTI_CELL_TEXT,
        },
        {
            "Omega rejects fast text",
            QStringLiteral("\u03a9"),
            1,
            0U,
            false,
            true,
            term::Terminal_simple_content_rejection_reason::NON_ASCII_TEXT,
        },
        {
            "CJK text rejects fast text",
            QStringLiteral("\u754c"),
            2,
            0U,
            false,
            true,
            term::Terminal_simple_content_rejection_reason::MULTI_CELL_TEXT,
        },
        {
            "combining mark rejects fast text",
            QStringLiteral("e\u0301"),
            1,
            0U,
            false,
            true,
            term::Terminal_simple_content_rejection_reason::NON_ASCII_TEXT,
        },
        {
            "variation selector rejects fast text",
            QString::fromUcs4(U"\u2764\ufe0e"),
            1,
            0U,
            false,
            true,
            term::Terminal_simple_content_rejection_reason::NON_ASCII_TEXT,
        },
        {
            "emoji rejects fast text",
            QString::fromUcs4(U"\U0001f600"),
            2,
            0U,
            false,
            true,
            term::Terminal_simple_content_rejection_reason::MULTI_CELL_TEXT,
        },
        {
            "ZWJ emoji rejects fast text",
            QString::fromUcs4(U"\U0001f469\u200d\U0001f4bb"),
            2,
            0U,
            false,
            true,
            term::Terminal_simple_content_rejection_reason::INVALID_TEXT_ENCODING,
        },
        {
            "invalid surrogate rejects fast text",
            QString(QChar(0xd800)),
            1,
            0U,
            false,
            true,
            term::Terminal_simple_content_rejection_reason::INVALID_TEXT_ENCODING,
        },
        {
            "bidi control rejects fast text",
            QString(QChar(0x202e)),
            1,
            0U,
            false,
            true,
            term::Terminal_simple_content_rejection_reason::INVALID_TEXT_WIDTH,
        },
        {
            "hyperlink rejects fast text",
            QStringLiteral("A"),
            1,
            17U,
            false,
            true,
            term::Terminal_simple_content_rejection_reason::HYPERLINK,
        },
        {
            "decoration rejects fast text",
            QStringLiteral("A"),
            1,
            0U,
            true,
            true,
            term::Terminal_simple_content_rejection_reason::DECORATION,
        },
        {
            "width mismatch rejects fast text",
            QStringLiteral("A"),
            2,
            0U,
            false,
            true,
            term::Terminal_simple_content_rejection_reason::INVALID_TEXT_WIDTH,
        },
    };

    for (const rejection_case_t& entry : cases) {
        term::Terminal_render_cell cell = ascii_cell;
        cell.text          = entry.text;
        cell.display_width = entry.display_width;
        cell.hyperlink_id  = entry.hyperlink_id;
        const term::terminal_simple_content_classification_t classification =
            term::classify_terminal_simple_content_cell(
                cell,
                grid_size,
                1U,
                entry.has_decoration,
                false);
        ok &= check(!classification.fast_text_eligible &&
            classification.route != term::Terminal_simple_content_route::FAST_TEXT,
            entry.message);
        if (entry.check_reason) {
            ok &= check(classification.rejection_reason == entry.expected_reason,
                entry.message);
        }
    }

    return ok;
}

bool test_fragmented_dirty_ranges_drive_simple_content_membership()
{
    bool ok = true;

    term::Terminal_render_snapshot snapshot = empty_snapshot({4, 4});
    snapshot.cursor.visible     = false;
    snapshot.dirty_row_ranges   = {{0, 1}, {2, 1}};
    snapshot.cells.push_back({.position = {0, 0}, .text = QStringLiteral("A")});
    snapshot.cells.push_back({.position = {1, 0}, .text = QStringLiteral("B")});
    snapshot.cells.push_back({.position = {2, 0}, .text = QStringLiteral("C")});
    snapshot.cells.push_back({.position = {3, 0}, .text = QStringLiteral("D")});

    term::Terminal_render_options render_options = options();
    render_options.packed_text_sidecars_enabled = true;
    const term::Terminal_render_frame frame = build(snapshot, render_options);
    const term::terminal_simple_content_stats_t& simple_stats =
        frame.stats.simple_content;

    ok &= check(frame.dirty_row_ranges.size() == 2U &&
        frame.dirty_row_ranges[0].first_row == 0 &&
        frame.dirty_row_ranges[0].row_count == 1 &&
        frame.dirty_row_ranges[1].first_row == 2 &&
        frame.dirty_row_ranges[1].row_count == 1,
        "fragmented dirty ranges are preserved on the render frame");
    ok &= check(simple_stats.eligible_cells == 4 &&
        simple_stats.eligible_after_all_gates_cells == 4 &&
        simple_stats.dirty_eligible_cells == 2 &&
        simple_stats.clean_eligible_cells == 2,
        "fragmented dirty ranges classify selected rows dirty and gaps clean");
    ok &= check(frame.stats.dirty_row_lookup_count == 8,
        "fragmented dirty row membership preserves logical lookup probe count");
    ok &= check(frame.stats.cell_pass_classification_calls == 4 &&
        frame.stats.packed_pass_classification_calls == 4,
        "fragmented dirty row membership exposes cell-pass and packed-pass classification calls");
    ok &= check(frame.stats.packed_text_cells == 4,
        "fragmented dirty row membership also feeds packed text sidecar classification");
    ok &= check(frame.stats.packed_text_ascii_direct_cells == 4 &&
        frame.stats.packed_text_ascii_direct_bytes == 4U &&
        frame.stats.packed_text_utf8_cells == 0U,
        "printable ASCII packed text cells use the direct byte-copy path");

    return ok;
}

bool test_cell_pass_and_packed_pass_classification_agree()
{
    bool ok = true;

    enum class decoration_t
    {
        NONE,
        UNDERLINE,
        STRIKE,
    };

    struct agreement_case_t
    {
        const char*    message               = "";
        QString        text;
        decoration_t   decoration           = decoration_t::NONE;
        std::uint64_t  hyperlink_id          = 0U;
        bool           dirty_row             = true;
        term::Terminal_simple_content_route
                       expected_route        = term::Terminal_simple_content_route::FAST_TEXT;
        term::Terminal_simple_content_rejection_reason
                       expected_reason       =
                           term::Terminal_simple_content_rejection_reason::NONE;
        QString        expected_packed_text;
        std::uint32_t  expected_graphic      = 0U;
        int            expected_dirty_fast   = 0;
        int            expected_clean_fast   = 0;
    };

    const std::vector<agreement_case_t> cases = {
        {
            "dirty ASCII fast-text cell agrees across passes",
            QStringLiteral("A"),
            decoration_t::NONE,
            0U,
            true,
            term::Terminal_simple_content_route::FAST_TEXT,
            term::Terminal_simple_content_rejection_reason::NONE,
            QStringLiteral("A"),
            0U,
            1,
            0,
        },
        {
            "clean ASCII fast-text cell agrees across passes",
            QStringLiteral("B"),
            decoration_t::NONE,
            0U,
            false,
            term::Terminal_simple_content_route::FAST_TEXT,
            term::Terminal_simple_content_rejection_reason::NONE,
            QStringLiteral("B"),
            0U,
            0,
            1,
        },
        {
            "non-ASCII text cell stays off packed fast text",
            QStringLiteral("\u03c0"),
            decoration_t::NONE,
            0U,
            true,
            term::Terminal_simple_content_route::QT_TEXT_LAYOUT,
            term::Terminal_simple_content_rejection_reason::NON_ASCII_TEXT,
        },
        {
            "terminal graphic cell agrees on packed graphic route",
            QStringLiteral("\u2588"),
            decoration_t::NONE,
            0U,
            true,
            term::Terminal_simple_content_route::GRAPHIC_GEOMETRY,
            term::Terminal_simple_content_rejection_reason::TERMINAL_GRAPHIC,
            {},
            0x2588U,
        },
        {
            "underlined ASCII cell stays off packed fast text",
            QStringLiteral("D"),
            decoration_t::UNDERLINE,
            0U,
            true,
            term::Terminal_simple_content_route::QT_TEXT_LAYOUT,
            term::Terminal_simple_content_rejection_reason::DECORATION,
        },
        {
            "strike-only ASCII cell stays off packed fast text",
            QStringLiteral("E"),
            decoration_t::STRIKE,
            0U,
            true,
            term::Terminal_simple_content_route::QT_TEXT_LAYOUT,
            term::Terminal_simple_content_rejection_reason::DECORATION,
        },
        {
            "hyperlink ASCII cell stays off packed fast text",
            QStringLiteral("F"),
            decoration_t::NONE,
            17U,
            true,
            term::Terminal_simple_content_route::QT_TEXT_LAYOUT,
            term::Terminal_simple_content_rejection_reason::HYPERLINK,
        },
    };

    const auto route_count = [](
        const term::terminal_simple_content_stats_t& stats,
        term::Terminal_simple_content_route route) {
        switch (route) {
            case term::Terminal_simple_content_route::NONE:
                return stats.route_none_cells;
            case term::Terminal_simple_content_route::FAST_TEXT:
                return stats.route_fast_text_cells;
            case term::Terminal_simple_content_route::QT_TEXT_LAYOUT:
                return stats.route_qt_text_layout_cells;
            case term::Terminal_simple_content_route::GRAPHIC_GEOMETRY:
                return stats.route_graphic_geometry_cells;
            case term::Terminal_simple_content_route::FALLBACK:
                return stats.route_fallback_cells;
        }
        return 0;
    };
    const auto route_total = [](
        const term::terminal_simple_content_stats_t& stats) {
        return
            stats.route_none_cells             +
            stats.route_fast_text_cells        +
            stats.route_qt_text_layout_cells   +
            stats.route_graphic_geometry_cells +
            stats.route_fallback_cells;
    };
    const auto rejection_count = [](
        const term::terminal_simple_content_stats_t& stats,
        term::Terminal_simple_content_rejection_reason reason) {
        switch (reason) {
            case term::Terminal_simple_content_rejection_reason::NONE:
                return stats.rejection_none_cells;
            case term::Terminal_simple_content_rejection_reason::EMPTY_TEXT:
                return stats.rejection_empty_text_cells;
            case term::Terminal_simple_content_rejection_reason::INVALID_GRID:
                return stats.rejection_invalid_grid_cells;
            case term::Terminal_simple_content_rejection_reason::INVALID_POSITION:
                return stats.rejection_invalid_position_cells;
            case term::Terminal_simple_content_rejection_reason::INVALID_STYLE_ID:
                return stats.rejection_invalid_style_id_cells;
            case term::Terminal_simple_content_rejection_reason::WIDE_CONTINUATION:
                return stats.rejection_wide_continuation_cells;
            case term::Terminal_simple_content_rejection_reason::INVALID_DISPLAY_WIDTH:
                return stats.rejection_invalid_display_width_cells;
            case term::Terminal_simple_content_rejection_reason::INVALID_TEXT_ENCODING:
                return stats.rejection_invalid_text_encoding_cells;
            case term::Terminal_simple_content_rejection_reason::INVALID_TEXT_WIDTH:
                return stats.rejection_invalid_text_width_cells;
            case term::Terminal_simple_content_rejection_reason::MULTI_CELL_TEXT:
                return stats.rejection_multi_cell_text_cells;
            case term::Terminal_simple_content_rejection_reason::NON_PRINTABLE_ASCII:
                return stats.rejection_non_printable_ascii_cells;
            case term::Terminal_simple_content_rejection_reason::NON_ASCII_TEXT:
                return stats.rejection_non_ascii_text_cells;
            case term::Terminal_simple_content_rejection_reason::DECORATION:
                return stats.rejection_decoration_cells;
            case term::Terminal_simple_content_rejection_reason::HYPERLINK:
                return stats.rejection_hyperlink_cells;
            case term::Terminal_simple_content_rejection_reason::TERMINAL_GRAPHIC:
                return stats.rejection_terminal_graphic_cells;
        }
        return 0;
    };
    const auto rejection_total = [](
        const term::terminal_simple_content_stats_t& stats) {
        return
            stats.rejection_none_cells                  +
            stats.rejection_empty_text_cells            +
            stats.rejection_invalid_grid_cells          +
            stats.rejection_invalid_position_cells      +
            stats.rejection_invalid_style_id_cells      +
            stats.rejection_wide_continuation_cells     +
            stats.rejection_invalid_display_width_cells +
            stats.rejection_invalid_text_encoding_cells +
            stats.rejection_invalid_text_width_cells    +
            stats.rejection_multi_cell_text_cells       +
            stats.rejection_non_printable_ascii_cells   +
            stats.rejection_non_ascii_text_cells        +
            stats.rejection_decoration_cells            +
            stats.rejection_hyperlink_cells             +
            stats.rejection_terminal_graphic_cells;
    };

    for (const agreement_case_t& entry : cases) {
        term::Terminal_render_snapshot snapshot = empty_snapshot({1, 8});
        snapshot.cursor.visible                 = false;
        snapshot.dirty_row_ranges               = entry.dirty_row
            ? std::vector<term::Terminal_render_dirty_row_range>{{0, 1}}
            : std::vector<term::Terminal_render_dirty_row_range>{};

        if (entry.decoration != decoration_t::NONE) {
            term::Terminal_text_style style = term::make_default_terminal_text_style();
            const term::Terminal_style_attribute attribute =
                entry.decoration == decoration_t::UNDERLINE
                    ? term::Terminal_style_attribute::UNDERLINE
                    : term::Terminal_style_attribute::STRIKE;
            term::set_terminal_style_attribute(style, attribute);
            snapshot.styles.push_back(style);
        }

        term::Terminal_render_cell cell;
        cell.position     = {0, 0};
        cell.text         = entry.text;
        cell.hyperlink_id = entry.hyperlink_id;
        cell.style_id     = entry.decoration == decoration_t::NONE ? 0U : 1U;
        snapshot.cells.push_back(cell);

        const bool has_decoration = entry.decoration != decoration_t::NONE;
        const term::terminal_simple_content_classification_t classification =
            term::classify_terminal_simple_content_cell(
                cell,
                snapshot.grid_size,
                snapshot.styles.size(),
                has_decoration,
                entry.dirty_row);
        const term::Terminal_render_frame frame = build(snapshot);
        const term::terminal_simple_content_stats_t& simple_stats =
            frame.stats.simple_content;

        ok &= check(classification.route == entry.expected_route &&
            classification.rejection_reason == entry.expected_reason &&
            classification.dirty_row == entry.dirty_row,
            entry.message);
        ok &= check(frame.stats.cell_pass_classification_calls == 1 &&
            frame.stats.packed_pass_classification_calls == 1,
            "single-cell agreement case classifies once in each pass");
        ok &= check(simple_stats.cells_considered == 1 &&
            route_total(simple_stats) == 1 &&
            route_count(simple_stats, classification.route) == 1 &&
            rejection_total(simple_stats) == 1 &&
            rejection_count(simple_stats, classification.rejection_reason) == 1,
            "single-cell agreement case records the direct route and rejection");
        ok &= check(simple_stats.dirty_eligible_cells == entry.expected_dirty_fast &&
            simple_stats.clean_eligible_cells == entry.expected_clean_fast,
            "single-cell agreement case preserves dirty-row eligibility");

        if (entry.expected_route == term::Terminal_simple_content_route::FAST_TEXT) {
            ok &= check(frame.stats.packed_text_cells == 1 &&
                frame.stats.packed_graphic_cells == 0 &&
                packed_text_payload(frame) == entry.expected_packed_text,
                "packed text output agrees with the fast-text classification");
        }
        else
        if (entry.expected_route == term::Terminal_simple_content_route::GRAPHIC_GEOMETRY) {
            ok &= check(frame.stats.packed_text_cells == 0 &&
                frame.stats.packed_graphic_cells == 1 &&
                has_packed_graphic_codepoint(frame, entry.expected_graphic),
                "packed graphic output agrees with the graphic classification");
        }
        else {
            ok &= check(frame.stats.packed_text_cells == 0 &&
                frame.stats.packed_graphic_cells == 0,
                "packed output excludes cells rejected from packed fast routes");
        }
    }

    return ok;
}

bool test_snapshot_cells_are_row_major_column_sorted()
{
    bool ok = true;

    term::Terminal_render_snapshot snapshot = empty_snapshot({3, 8});
    snapshot.cursor.visible = false;
    snapshot.cells.push_back({.position = {0, 0}, .text = QStringLiteral("A")});
    snapshot.cells.push_back({.position = {0, 3}, .text = QStringLiteral("D")});
    snapshot.cells.push_back({.position = {1, 0}, .text = QStringLiteral("B")});
    snapshot.cells.push_back({.position = {2, 2}, .text = QStringLiteral("C")});

    ok &= check(snapshot_cells_are_row_major_column_sorted(snapshot),
        "snapshot cell fixture is row-major and column sorted");

    const term::Terminal_render_frame frame = build(snapshot);
    ok &= check(frame.text_runs.size() == 4U &&
        frame.text_runs[0].row == 0 && frame.text_runs[0].column == 0 &&
        frame.text_runs[1].row == 0 && frame.text_runs[1].column == 3 &&
        frame.text_runs[2].row == 1 && frame.text_runs[2].column == 0 &&
        frame.text_runs[3].row == 2 && frame.text_runs[3].column == 2,
        "cell pass observes the row-major snapshot ordering invariant");
    ok &= check(frame.stats.packed_text_cells == 4 &&
        packed_text_payload(frame) == QStringLiteral("ADBC"),
        "packed pass output matches row-major column order for sorted snapshots");

    term::Terminal_render_snapshot unsorted_snapshot = snapshot;
    std::swap(unsorted_snapshot.cells[0], unsorted_snapshot.cells[1]);
    ok &= check(!snapshot_cells_are_row_major_column_sorted(unsorted_snapshot),
        "snapshot cell order fixture detects unsorted columns");

    return ok;
}

bool test_packed_text_rows_spans_and_order()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({3, 8});
    snapshot.cursor.visible                 = false;
    snapshot.viewport.scrollback_rows       = 9;
    snapshot.viewport.offset_from_tail      = 4;

    term::Terminal_text_style accent = term::make_default_terminal_text_style();
    accent.foreground = term::make_rgb_terminal_color_ref(0xffcc1010U);
    accent.background = term::make_rgb_terminal_color_ref(0xff1030ccU);
    snapshot.styles.push_back(accent);

    snapshot.cells.push_back({.position = {0, 4}, .text = QStringLiteral("E"), .style_id = 1U});
    snapshot.cells.push_back({.position = {0, 1}, .text = QStringLiteral("B")});
    snapshot.cells.push_back({.position = {2, 2}, .text = QStringLiteral("C")});
    snapshot.cells.push_back({.position = {0, 0}, .text = QStringLiteral("A")});
    snapshot.cells.push_back({.position = {0, 3}, .text = QStringLiteral("D")});
    snapshot.cells.push_back({.position = {0, 5}, .text = QStringLiteral("F"), .style_id = 1U});
    snapshot.cells.push_back({.position = {1, 0}, .text = QStringLiteral("\u03c0")});

    const term::Terminal_render_frame frame = build(snapshot);
    ok &= check(frame.packed_rows.size() == 3U &&
        frame.stats.packed_rows == 3 &&
        frame.packed_rows[0].active_buffer == term::Terminal_buffer_id::PRIMARY &&
        frame.packed_rows[0].viewport_row == 0 &&
        frame.packed_rows[0].logical_row == 5 &&
        frame.packed_rows[2].logical_row == 7,
        "packed rows preserve active-buffer, viewport-row, and logical-row identity");
    ok &= check(frame.packed_text_spans.size() == 4U &&
        frame.stats.packed_text_spans == 4 &&
        frame.stats.packed_text_cells == 6,
        "packed text spans count coalesced simple text cells");
    ok &= check(frame.stats.packed_text_ascii_direct_cells == 6 &&
        frame.stats.packed_text_ascii_direct_bytes == 6U &&
        frame.stats.packed_text_utf8_cells == 0U,
        "packed text byte-path counters match the accepted ASCII cells");

    const term::terminal_packed_render_row_t& row_zero = frame.packed_rows[0];
    ok &= check(row_zero.first_text_span == 0U && row_zero.text_span_count == 3U,
        "packed row records its local text span range");
    ok &= check(packed_text(frame, frame.packed_text_spans[0]) == QStringLiteral("AB") &&
        frame.packed_text_spans[0].first_column == 0 &&
        frame.packed_text_spans[0].column_count == 2,
        "packed text coalesces out-of-order adjacent cells by column");
    ok &= check(packed_text(frame, frame.packed_text_spans[1]) == QStringLiteral("D") &&
        frame.packed_text_spans[1].first_column == 3,
        "packed text starts a new span across a column gap");
    ok &= check(packed_text(frame, frame.packed_text_spans[2]) == QStringLiteral("EF") &&
        frame.packed_text_spans[2].style_id == 1U &&
        frame.packed_text_spans[2].foreground_rgba ==
            static_cast<std::uint32_t>(QColor(204, 16, 16).rgba()) &&
        frame.packed_text_spans[2].background_rgba ==
            static_cast<std::uint32_t>(QColor(16, 48, 204).rgba()),
        "packed text starts a new span at style and color boundaries");
    ok &= check(frame.packed_rows[1].text_span_count == 0U &&
        frame.stats.simple_content.rejection_non_ascii_text_cells == 1,
        "packed text excludes complex non-ASCII text while stats keep the rejection reason");
    ok &= check(frame.stats.packed_payload_bytes > 0U,
        "packed text frame reports payload bytes");
    return ok;
}

bool test_packed_graphic_source_codepoint_spans()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({2, 8});
    snapshot.cursor.visible = false;

    term::Terminal_text_style accent = term::make_default_terminal_text_style();
    accent.foreground = term::make_rgb_terminal_color_ref(0xff20d0f0U);
    snapshot.styles.push_back(accent);

    snapshot.cells.push_back({.position = {0, 2}, .text = QStringLiteral("\u2588")});
    snapshot.cells.push_back({.position = {0, 0}, .text = QStringLiteral("\u250c")});
    snapshot.cells.push_back({.position = {0, 1}, .text = QStringLiteral("\u2500")});
    snapshot.cells.push_back({.position = {0, 4}, .text = QStringLiteral("\u2510"), .style_id = 1U});
    snapshot.cells.push_back({.position = {1, 0}, .text = QStringLiteral("A")});

    const term::Terminal_render_frame frame = build(snapshot);
    ok &= check(has_graphic_rect(frame, QRectF(10.0, 9.5, 10.0, 1.0)) &&
        !has_graphic_rect(frame, QRectF(20.0, 0.0, 10.0, 20.0)),
        "packed hard blocks are not duplicated while box graphics remain direct");
    ok &= check(frame.packed_graphic_spans.size() == 2U &&
        frame.packed_graphic_codepoints.size() == 4U &&
        frame.stats.packed_graphic_spans == 2 &&
        frame.stats.packed_graphic_cells == 4,
        "packed graphics store source codepoint spans");

    const term::terminal_packed_graphic_span_t& default_span =
        frame.packed_graphic_spans[0];
    ok &= check(default_span.first_column == 0 &&
        default_span.column_count == 3 &&
        default_span.codepoint_offset == 0U &&
        default_span.codepoint_count == 3U &&
        frame.packed_graphic_codepoints[0] == 0x250cU &&
        frame.packed_graphic_codepoints[1] == 0x2500U &&
        frame.packed_graphic_codepoints[2] == 0x2588U,
        "packed graphic source span follows sorted cell columns");

    const term::terminal_packed_graphic_span_t& accent_span =
        frame.packed_graphic_spans[1];
    ok &= check(accent_span.first_column == 4 &&
        accent_span.style_id == 1U &&
        accent_span.foreground_rgba ==
            static_cast<std::uint32_t>(QColor(32, 208, 240).rgba()) &&
        frame.packed_graphic_codepoints[accent_span.codepoint_offset] == 0x2510U,
        "packed graphic spans preserve style and source codepoints");
    ok &= check(frame.stats.simple_content.route_graphic_geometry_cells ==
        frame.stats.packed_graphic_cells,
        "packed graphic cell count matches canonical graphic-route candidates");
    return ok;
}

bool test_packed_graphics_exclude_unrepresented_cells()
{
    bool ok = true;

    term::Terminal_render_snapshot semantic_snapshot = empty_snapshot({1, 8});
    semantic_snapshot.cursor.visible = false;
    term::Terminal_text_style decorated = term::make_default_terminal_text_style();
    term::set_terminal_style_attribute(decorated, term::Terminal_style_attribute::UNDERLINE);
    semantic_snapshot.styles.push_back(decorated);
    semantic_snapshot.hyperlinks.push_back({17U, QByteArrayLiteral("uri:https://example.test"),
        QByteArrayLiteral("https://example.test")});
    semantic_snapshot.cells.push_back({.position = {0, 0}, .text = QStringLiteral("\u250c")});
    semantic_snapshot.cells.push_back({
        .position     = {0, 1},
        .text         = QStringLiteral("\u2500"),
        .hyperlink_id = 17U,
    });
    semantic_snapshot.cells.push_back({
        .position = {0, 2},
        .text     = QStringLiteral("\u2510"),
        .style_id = 1U,
    });
    semantic_snapshot.cells.push_back({
        .position      = {0, 4},
        .text          = QStringLiteral("\u2588"),
        .display_width = 2,
    });

    const term::Terminal_render_frame semantic_frame = build(semantic_snapshot);
    ok &= check(semantic_frame.stats.text_cells_rendered_as_graphic == 4,
        "unrepresented graphic sidecar cases still use canonical visual graphics");
    ok &= check(semantic_frame.stats.packed_graphic_cells == 1 &&
        semantic_frame.packed_graphic_codepoints.size() == 1U &&
        semantic_frame.packed_graphic_codepoints.front() == 0x250cU,
        "packed graphics exclude hyperlink, decorated, and width-mismatched graphics");
    ok &= check(has_graphic_rect(semantic_frame, QRectF(40.0, 0.0, 20.0, 20.0)),
        "width-mismatched hard block remains on the direct graphic route");
    ok &= check(semantic_frame.stats.simple_content.route_graphic_geometry_cells == 1 &&
        semantic_frame.stats.simple_content.rejection_hyperlink_cells == 1 &&
        semantic_frame.stats.simple_content.rejection_decoration_cells == 1 &&
        semantic_frame.stats.simple_content.rejection_invalid_text_width_cells == 1,
        "simple-content classifier rejects graphic cells with unrepresented semantics");

    term::Terminal_render_snapshot cursor_snapshot = empty_snapshot({1, 4});
    cursor_snapshot.cursor = {{0, 0}, term::Terminal_cursor_shape::BLOCK, true, false};
    cursor_snapshot.cells.push_back({.position = {0, 0}, .text = QStringLiteral("\u2588")});
    cursor_snapshot.cells.push_back({.position = {0, 1}, .text = QStringLiteral("\u2500")});

    const term::Terminal_render_frame cursor_frame = build(cursor_snapshot);
    ok &= check(cursor_frame.stats.text_cells_rendered_as_graphic == 2 &&
        !cursor_frame.cursor_graphic_rects.empty(),
        "block cursor over packed-excluded graphic still emits canonical cursor graphics");
    ok &= check(has_graphic_rect(cursor_frame, QRectF(0.0, 0.0, 10.0, 20.0)),
        "block-cursor-covered hard block remains on the direct graphic route");
    ok &= check(cursor_frame.stats.simple_content.route_graphic_geometry_cells == 2 &&
        cursor_frame.stats.packed_graphic_cells == 1 &&
        cursor_frame.packed_graphic_codepoints.size() == 1U &&
        cursor_frame.packed_graphic_codepoints.front() == 0x2500U,
        "packed graphics exclude block-cursor-covered graphics");

    term::Terminal_render_snapshot ime_snapshot = empty_snapshot({1, 5});
    ime_snapshot.cursor.visible     = false;
    ime_snapshot.cursor.position    = {0, 0};
    ime_snapshot.ime_preedit.active = true;
    ime_snapshot.ime_preedit.text   = QStringLiteral("xy");
    ime_snapshot.cells.push_back({.position = {0, 0}, .text = QStringLiteral("\u2588")});
    ime_snapshot.cells.push_back({.position = {0, 1}, .text = QStringLiteral("\u2500")});
    ime_snapshot.cells.push_back({.position = {0, 2}, .text = QStringLiteral("\u2510")});

    const term::Terminal_render_frame ime_frame = build(ime_snapshot);
    ok &= check(ime_frame.stats.text_cells_rendered_as_graphic == 3 &&
        !ime_frame.text_runs.empty() &&
        ime_frame.text_runs.back().text == QStringLiteral("xy"),
        "IME over packed-excluded graphics still emits canonical preedit text");
    ok &= check(has_graphic_rect(ime_frame, QRectF(0.0, 0.0, 10.0, 20.0)),
        "IME-covered hard block remains on the direct graphic route");
    ok &= check(ime_frame.stats.simple_content.route_graphic_geometry_cells == 3 &&
        ime_frame.stats.packed_graphic_cells == 1 &&
        ime_frame.packed_graphic_codepoints.size() == 1U &&
        ime_frame.packed_graphic_codepoints.front() == 0x2510U,
        "packed graphics exclude IME-covered graphics");
    return ok;
}

bool test_disabled_packed_text_sidecars_skip_scan_and_keep_graphics()
{
    bool ok = true;

    term::Terminal_render_options render_options = options();
    render_options.packed_text_sidecars_enabled = false;

    term::Terminal_render_snapshot text_snapshot = empty_snapshot({3, 8});
    text_snapshot.cursor.visible = false;
    text_snapshot.cells.push_back({.position = {0, 0}, .text = QStringLiteral("A")});
    text_snapshot.cells.push_back({.position = {1, 0}, .text = QStringLiteral("B")});
    text_snapshot.cells.push_back({.position = {2, 0}, .text = QStringLiteral("\u03c0")});

    const term::Terminal_render_frame text_frame =
        build(text_snapshot, render_options);
    ok &= check(text_frame.stats.packed_text_sidecars_enabled == 0 &&
        text_frame.stats.packed_text_sidecars_disabled == 1,
        "disabled packed text sidecars are reported on the frame");
    ok &= check(text_frame.stats.packed_pass_input_cells == 0 &&
        text_frame.stats.packed_pass_cells_scanned == 0 &&
        text_frame.stats.packed_pass_classification_calls == 0 &&
        text_frame.stats.packed_text_disabled_cells_skipped ==
            text_frame.stats.simple_content.eligible_after_all_gates_cells &&
        text_frame.stats.packed_text_disabled_cells_skipped == 2,
        "disabled packed text sidecars skip the separate packed-data scan");
    ok &= check(text_frame.packed_rows.size() == 3U &&
        text_frame.packed_rows[0].viewport_row == 0 &&
        text_frame.packed_rows[1].viewport_row == 1 &&
        text_frame.packed_rows[2].viewport_row == 2 &&
        text_frame.packed_text_spans.empty() &&
        text_frame.packed_graphic_spans.empty() &&
        text_frame.stats.packed_rows == 3 &&
        text_frame.stats.packed_text_cells == 0 &&
        text_frame.stats.packed_graphic_cells == 0 &&
        text_frame.text_runs.size() == 3U &&
        text_frame.text_runs[0].text == QStringLiteral("A") &&
        text_frame.text_runs[1].text == QStringLiteral("B") &&
        text_frame.text_runs[2].text == QStringLiteral("\u03c0") &&
        text_frame.stats.packed_payload_bytes > 0U,
        "text-only disabled-sidecar frames keep ordered packed row identity");

    term::Terminal_render_snapshot graphics_snapshot = empty_snapshot({2, 8});
    graphics_snapshot.cursor.visible = false;
    graphics_snapshot.cells.push_back({.position = {0, 2}, .text = QStringLiteral("\u2598")});
    graphics_snapshot.cells.push_back({.position = {1, 1}, .text = QStringLiteral("\u2588")});
    graphics_snapshot.cells.push_back({.position = {0, 0}, .text = QStringLiteral("\u2588")});
    graphics_snapshot.cells.push_back({.position = {0, 1}, .text = QStringLiteral("\u2598")});

    const term::Terminal_render_frame graphics_frame =
        build(graphics_snapshot, render_options);
    ok &= check(graphics_frame.stats.packed_pass_input_cells == 0 &&
        graphics_frame.stats.packed_pass_cells_scanned == 0 &&
        graphics_frame.stats.packed_pass_classification_calls == 0 &&
        graphics_frame.stats.packed_text_cells == 0,
        "disabled packed text sidecars keep graphics off the separate packed pass");
    ok &= check(graphics_frame.packed_rows.size() == 2U &&
        graphics_frame.stats.packed_rows == 2 &&
        graphics_frame.packed_rows[0].viewport_row == 0 &&
        graphics_frame.packed_rows[0].logical_row == 0 &&
        graphics_frame.packed_rows[1].viewport_row == 1 &&
        graphics_frame.packed_rows[1].logical_row == 1,
        "disabled-sidecar packed rows are indexed by viewport row");
    ok &= check(graphics_frame.stats.text_cells_rendered_as_graphic == 4 &&
        graphics_frame.stats.packed_graphic_candidates_classified == 4 &&
        graphics_frame.stats.packed_graphic_cells == 4 &&
        has_packed_graphic_codepoint(graphics_frame, 0x2588U) &&
        has_packed_graphic_codepoint(graphics_frame, 0x2598U) &&
        graphics_frame.graphic_rects.empty(),
        "hard block graphics remain packed for rendering with text sidecars disabled");
    ok &= check(graphics_frame.stats.packed_payload_bytes > 0U,
        "disabled-sidecar packed graphics report payload bytes");

    const auto first_graphic_codepoint_for_row = [&](
        const term::terminal_packed_render_row_t& row) {
        if (row.graphic_span_count == 0U) {
            return 0U;
        }

        const std::size_t span_index =
            static_cast<std::size_t>(row.first_graphic_span);
        if (span_index >= graphics_frame.packed_graphic_spans.size()) {
            return 0U;
        }

        const term::terminal_packed_graphic_span_t& span =
            graphics_frame.packed_graphic_spans[span_index];
        const std::size_t codepoint_index =
            static_cast<std::size_t>(span.codepoint_offset);
        if (span.codepoint_count == 0U ||
            codepoint_index >= graphics_frame.packed_graphic_codepoints.size())
        {
            return 0U;
        }

        return graphics_frame.packed_graphic_codepoints[codepoint_index];
    };
    const term::terminal_packed_render_row_t empty_graphics_row;
    const term::terminal_packed_render_row_t& graphics_row_zero =
        !graphics_frame.packed_rows.empty()
            ? graphics_frame.packed_rows[0]
            : empty_graphics_row;
    const term::terminal_packed_render_row_t& graphics_row_one =
        graphics_frame.packed_rows.size() > 1U
            ? graphics_frame.packed_rows[1]
            : empty_graphics_row;
    ok &= check(graphics_row_zero.graphic_span_count == 1U &&
        graphics_row_one.graphic_span_count == 1U &&
        first_graphic_codepoint_for_row(graphics_row_zero) == 0x2588U &&
        first_graphic_codepoint_for_row(graphics_row_one) == 0x2588U,
        "indexed disabled-sidecar graphic rows reference their own codepoints");
    if (graphics_row_zero.graphic_span_count > 0U &&
        graphics_row_zero.first_graphic_span < graphics_frame.packed_graphic_spans.size())
    {
        const term::terminal_packed_graphic_span_t& row_zero_span =
            graphics_frame.packed_graphic_spans[graphics_row_zero.first_graphic_span];
        const std::size_t row_zero_codepoint_offset =
            static_cast<std::size_t>(row_zero_span.codepoint_offset);
        ok &= check(row_zero_span.first_column == 0 &&
            row_zero_span.column_count == 3 &&
            row_zero_span.codepoint_count == 3U &&
            row_zero_codepoint_offset + 2U <
                graphics_frame.packed_graphic_codepoints.size() &&
            graphics_frame.packed_graphic_codepoints[row_zero_codepoint_offset] ==
                0x2588U &&
            graphics_frame.packed_graphic_codepoints[row_zero_codepoint_offset + 1U] ==
                0x2598U &&
            graphics_frame.packed_graphic_codepoints[row_zero_codepoint_offset + 2U] ==
                0x2598U,
            "disabled-sidecar graphic rows sort interleaved cells before coalescing");
    }

    term::Terminal_render_snapshot semantic_snapshot = empty_snapshot({1, 8});
    semantic_snapshot.cursor.visible = false;
    term::Terminal_text_style decorated = term::make_default_terminal_text_style();
    term::set_terminal_style_attribute(decorated, term::Terminal_style_attribute::UNDERLINE);
    semantic_snapshot.styles.push_back(decorated);
    semantic_snapshot.hyperlinks.push_back({17U, QByteArrayLiteral("uri:https://example.test"),
        QByteArrayLiteral("https://example.test")});
    semantic_snapshot.cells.push_back({.position = {0, 0}, .text = QStringLiteral("\u250c")});
    semantic_snapshot.cells.push_back({
        .position     = {0, 1},
        .text         = QStringLiteral("\u2500"),
        .hyperlink_id = 17U,
    });
    semantic_snapshot.cells.push_back({
        .position = {0, 2},
        .text     = QStringLiteral("\u2510"),
        .style_id = 1U,
    });
    semantic_snapshot.cells.push_back({
        .position      = {0, 4},
        .text          = QStringLiteral("\u2588"),
        .display_width = 2,
    });

    const term::Terminal_render_frame semantic_frame =
        build(semantic_snapshot, render_options);
    ok &= check(semantic_frame.stats.text_cells_rendered_as_graphic == 4 &&
        semantic_frame.stats.packed_graphic_candidates_classified == 4 &&
        semantic_frame.stats.packed_text_disabled_cells_skipped == 0 &&
        semantic_frame.stats.packed_graphic_cells == 1 &&
        semantic_frame.packed_graphic_codepoints.size() == 1U &&
        semantic_frame.packed_graphic_codepoints.front() == 0x250cU,
        "disabled-sidecar packed graphics reject hyperlink, decorated, and wide cells");

    term::Terminal_render_snapshot style_snapshot = empty_snapshot({1, 4});
    style_snapshot.cursor.visible = false;
    term::Terminal_text_style accent = term::make_default_terminal_text_style();
    accent.foreground = term::make_rgb_terminal_color_ref(0xff20d0f0U);
    style_snapshot.styles.push_back(accent);
    style_snapshot.cells.push_back({.position = {0, 0}, .text = QStringLiteral("\u250c")});
    style_snapshot.cells.push_back({.position = {0, 1}, .text = QStringLiteral("\u2500")});
    style_snapshot.cells.push_back({
        .position = {0, 2},
        .text     = QStringLiteral("\u2510"),
        .style_id = 1U,
    });

    const term::Terminal_render_frame style_frame =
        build(style_snapshot, render_options);
    ok &= check(style_frame.packed_graphic_spans.size() == 2U &&
        style_frame.packed_graphic_codepoints.size() == 3U &&
        style_frame.packed_graphic_spans[0].first_column == 0 &&
        style_frame.packed_graphic_spans[0].column_count == 2 &&
        style_frame.packed_graphic_spans[1].first_column == 2 &&
        style_frame.packed_graphic_spans[1].style_id == 1U,
        "disabled-sidecar packed graphics split adjacent spans at style boundaries");

    const auto rects_match = [](
        const std::vector<term::Terminal_render_rect>& left,
        const std::vector<term::Terminal_render_rect>& right) {
        if (left.size() != right.size()) {
            return false;
        }

        for (std::size_t index = 0U; index < left.size(); ++index) {
            if (left[index].rect      != right[index].rect      ||
                left[index].color     != right[index].color     ||
                left[index].antialias != right[index].antialias)
            {
                return false;
            }
        }

        return true;
    };
    const auto arcs_match = [](
        const std::vector<term::Terminal_render_arc>& left,
        const std::vector<term::Terminal_render_arc>& right) {
        if (left.size() != right.size()) {
            return false;
        }

        for (std::size_t index = 0U; index < left.size(); ++index) {
            if (left[index].kind   != right[index].kind  ||
                left[index].rect   != right[index].rect  ||
                left[index].color  != right[index].color ||
                left[index].stroke != right[index].stroke)
            {
                return false;
            }
        }

        return true;
    };
    const auto packed_graphics_match = [](
        const term::Terminal_render_frame& left,
        const term::Terminal_render_frame& right) {
        if (left.packed_rows.size()              != right.packed_rows.size() ||
            left.packed_graphic_spans.size()     != right.packed_graphic_spans.size() ||
            left.packed_graphic_codepoints       != right.packed_graphic_codepoints)
        {
            return false;
        }

        for (std::size_t index = 0U; index < left.packed_rows.size(); ++index) {
            const term::terminal_packed_render_row_t& left_row = left.packed_rows[index];
            const term::terminal_packed_render_row_t& right_row = right.packed_rows[index];
            if (left_row.active_buffer       != right_row.active_buffer      ||
                left_row.viewport_row        != right_row.viewport_row       ||
                left_row.logical_row         != right_row.logical_row        ||
                left_row.first_graphic_span  != right_row.first_graphic_span ||
                left_row.graphic_span_count  != right_row.graphic_span_count)
            {
                return false;
            }
        }

        for (std::size_t index = 0U; index < left.packed_graphic_spans.size(); ++index) {
            const term::terminal_packed_graphic_span_t& left_span =
                left.packed_graphic_spans[index];
            const term::terminal_packed_graphic_span_t& right_span =
                right.packed_graphic_spans[index];
            if (left_span.first_column     != right_span.first_column     ||
                left_span.column_count     != right_span.column_count     ||
                left_span.style_id         != right_span.style_id         ||
                left_span.foreground_rgba  != right_span.foreground_rgba  ||
                left_span.background_rgba  != right_span.background_rgba  ||
                left_span.codepoint_offset != right_span.codepoint_offset ||
                left_span.codepoint_count  != right_span.codepoint_count)
            {
                return false;
            }
        }

        return true;
    };

    term::Terminal_render_snapshot parity_snapshot = empty_snapshot({3, 8});
    parity_snapshot.cursor = {{2, 3}, term::Terminal_cursor_shape::BLOCK, true, false};
    parity_snapshot.styles.push_back(accent);
    parity_snapshot.cells.push_back({.position = {2, 3}, .text = QStringLiteral("\u2588")});
    parity_snapshot.cells.push_back({.position = {2, 0}, .text = QStringLiteral("\u2598")});
    parity_snapshot.cells.push_back({.position = {1, 1}, .text = QStringLiteral("\u256d")});
    parity_snapshot.cells.push_back({
        .position = {0, 2},
        .text     = QStringLiteral("\u2588"),
        .style_id = 1U,
    });
    parity_snapshot.cells.push_back({.position = {0, 0}, .text = QStringLiteral("\u250c")});
    parity_snapshot.cells.push_back({.position = {0, 1}, .text = QStringLiteral("\u2500")});

    term::Terminal_render_options enabled_options = options();
    enabled_options.packed_text_sidecars_enabled = true;
    const term::Terminal_render_frame enabled_frame =
        build(parity_snapshot, enabled_options);
    const term::Terminal_render_frame disabled_frame =
        build(parity_snapshot, render_options);
    ok &= check(!snapshot_cells_are_row_major_column_sorted(parity_snapshot) &&
        packed_graphics_match(enabled_frame, disabled_frame) &&
        rects_match(enabled_frame.graphic_rects, disabled_frame.graphic_rects) &&
        rects_match(
            enabled_frame.cursor_graphic_rects,
            disabled_frame.cursor_graphic_rects) &&
        arcs_match(enabled_frame.graphic_arcs, disabled_frame.graphic_arcs) &&
        enabled_frame.stats.text_cells_rendered_as_graphic ==
            disabled_frame.stats.text_cells_rendered_as_graphic,
        "disabled-sidecar graphic output matches enabled sidecars on unsorted input");

    term::Terminal_render_snapshot cursor_snapshot = empty_snapshot({2, 8});
    cursor_snapshot.cursor = {{0, 1}, term::Terminal_cursor_shape::BLOCK, true, false};
    cursor_snapshot.cells.push_back({.position = {0, 1}, .text = QStringLiteral("\u2588")});
    cursor_snapshot.cells.push_back({.position = {1, 1}, .text = QStringLiteral("\u2598")});

    const term::Terminal_render_frame cursor_frame =
        build(cursor_snapshot, render_options);
    ok &= check(cursor_frame.stats.packed_pass_input_cells == 0 &&
        cursor_frame.stats.packed_graphic_candidates_classified == 1 &&
        cursor_frame.stats.packed_graphic_cells == 1 &&
        !has_packed_graphic_codepoint(cursor_frame, 0x2588U) &&
        has_packed_graphic_codepoint(cursor_frame, 0x2598U),
        "disabled-sidecar packed graphics still exclude block-cursor-covered cells");
    ok &= check(has_graphic_rect(cursor_frame, QRectF(10.0, 0.0, 10.0, 20.0)) &&
        cursor_frame.cursor_graphic_rects.size() == 1U,
        "block-cursor-covered hard graphics keep the direct and cursor overlay routes");

    term::Terminal_render_snapshot mixed_graphics_snapshot = empty_snapshot({1, 4});
    mixed_graphics_snapshot.cursor.visible = false;
    mixed_graphics_snapshot.cells.push_back(
        {.position = {0, 0}, .text = QStringLiteral("\u2500")});
    mixed_graphics_snapshot.cells.push_back(
        {.position = {0, 1}, .text = QStringLiteral("\u256d")});

    const term::Terminal_render_frame mixed_graphics_frame =
        build(mixed_graphics_snapshot, render_options);
    ok &= check(!mixed_graphics_frame.graphic_rects.empty() &&
        !mixed_graphics_frame.graphic_arcs.empty() &&
        has_packed_graphic_codepoint(mixed_graphics_frame, 0x2500U) &&
        has_packed_graphic_codepoint(mixed_graphics_frame, 0x256dU),
        "box and arc graphics keep direct geometry plus packed source codepoints");

    term::Terminal_render_snapshot ime_snapshot = empty_snapshot({1, 4});
    ime_snapshot.cursor = {{0, 0}, term::Terminal_cursor_shape::BLOCK, true, false};
    ime_snapshot.ime_preedit.active = true;
    ime_snapshot.ime_preedit.text = QStringLiteral("ab");
    ime_snapshot.cells.push_back({.position = {0, 0}, .text = QStringLiteral("\u2588")});
    ime_snapshot.cells.push_back({.position = {0, 1}, .text = QStringLiteral("\u2598")});
    ime_snapshot.cells.push_back({.position = {0, 3}, .text = QStringLiteral("\u2588")});

    const term::Terminal_render_frame ime_frame =
        build(ime_snapshot, render_options);
    ok &= check(ime_frame.stats.packed_graphic_cells == 1 &&
        ime_frame.stats.packed_graphic_candidates_classified == 1 &&
        !has_packed_graphic_codepoint(ime_frame, 0x2598U) &&
        has_packed_graphic_codepoint(ime_frame, 0x2588U),
        "disabled-sidecar packed graphics still exclude IME-covered cells");
    return ok;
}

bool test_packed_text_excludes_cursor_and_ime_cells()
{
    bool ok = true;
    term::Terminal_render_snapshot cursor_snapshot = empty_snapshot({1, 5});
    cursor_snapshot.cursor = {{0, 1}, term::Terminal_cursor_shape::BLOCK, true, false};
    cursor_snapshot.cells.push_back({.position = {0, 0}, .text = QStringLiteral("A")});
    cursor_snapshot.cells.push_back({.position = {0, 1}, .text = QStringLiteral("B")});
    cursor_snapshot.cells.push_back({.position = {0, 2}, .text = QStringLiteral("C")});

    const term::Terminal_render_frame cursor_frame = build(cursor_snapshot);
    ok &= check(cursor_frame.stats.simple_content.eligible_cells == 3 &&
        cursor_frame.stats.simple_content.eligible_after_all_gates_cells == 2 &&
        cursor_frame.stats.simple_content.route_fast_text_cells == 3 &&
        cursor_frame.stats.packed_text_cells == 2 &&
        packed_text_payload(cursor_frame) == QStringLiteral("AC"),
        "packed text excludes one otherwise-strict ASCII cell covered by block cursor");
    ok &= check(cursor_frame.stats.cell_pass_classification_calls == 3 &&
        cursor_frame.stats.packed_pass_classification_calls == 2,
        "block cursor coverage is visible as a packed-pass classification skip");
    ok &= check(cursor_frame.cursor_text_runs.size() == 1U &&
        cursor_frame.cursor_text_runs.front().text == QStringLiteral("B"),
        "block cursor inverse text still comes from visual text runs");

    term::Terminal_render_snapshot ime_snapshot = empty_snapshot({1, 5});
    ime_snapshot.cursor.visible     = false;
    ime_snapshot.cursor.position    = {0, 1};
    ime_snapshot.ime_preedit.active = true;
    ime_snapshot.ime_preedit.text   = QStringLiteral("xy");
    ime_snapshot.cells.push_back({.position = {0, 0}, .text = QStringLiteral("A")});
    ime_snapshot.cells.push_back({.position = {0, 1}, .text = QStringLiteral("B")});
    ime_snapshot.cells.push_back({.position = {0, 2}, .text = QStringLiteral("C")});
    ime_snapshot.cells.push_back({.position = {0, 3}, .text = QStringLiteral("D")});

    const term::Terminal_render_frame ime_frame = build(ime_snapshot);
    ok &= check(ime_frame.stats.simple_content.eligible_cells == 4 &&
        ime_frame.stats.simple_content.eligible_after_all_gates_cells == 2 &&
        ime_frame.stats.simple_content.route_fast_text_cells == 4 &&
        ime_frame.stats.packed_text_cells == 2 &&
        packed_text_payload(ime_frame) == QStringLiteral("AD"),
        "packed text excludes otherwise-strict ASCII cells covered by IME preedit");
    ok &= check(ime_frame.stats.cell_pass_classification_calls == 4 &&
        ime_frame.stats.packed_pass_classification_calls == 2,
        "IME coverage is visible as packed-pass classification skips");
    ok &= check(!ime_frame.text_runs.empty() &&
        ime_frame.text_runs.back().text == QStringLiteral("xy"),
        "IME visual text still uses the existing text-run route");

    term::Terminal_render_snapshot cjk_ime_snapshot = empty_snapshot({1, 5});
    cjk_ime_snapshot.cursor.visible     = false;
    cjk_ime_snapshot.cursor.position    = {0, 1};
    cjk_ime_snapshot.ime_preedit.active = true;
    cjk_ime_snapshot.ime_preedit.text   = QStringLiteral("\u754c");
    cjk_ime_snapshot.cells.push_back({.position = {0, 0}, .text = QStringLiteral("A")});
    cjk_ime_snapshot.cells.push_back({.position = {0, 1}, .text = QStringLiteral("B")});
    cjk_ime_snapshot.cells.push_back({.position = {0, 2}, .text = QStringLiteral("C")});
    cjk_ime_snapshot.cells.push_back({.position = {0, 3}, .text = QStringLiteral("D")});

    const term::Terminal_render_frame cjk_ime_frame = build(cjk_ime_snapshot);
    ok &= check(cjk_ime_frame.stats.simple_content.eligible_cells == 4 &&
        cjk_ime_frame.stats.simple_content.eligible_after_all_gates_cells == 2 &&
        cjk_ime_frame.stats.simple_content.route_fast_text_cells == 4 &&
        cjk_ime_frame.stats.packed_text_cells == 2 &&
        packed_text_payload(cjk_ime_frame) == QStringLiteral("AD"),
        "packed text excludes cells covered by CJK preedit over ASCII content");
    ok &= check(cjk_ime_frame.stats.cell_pass_classification_calls == 4 &&
        cjk_ime_frame.stats.packed_pass_classification_calls == 2,
        "CJK IME coverage is visible as packed-pass classification skips");
    ok &= check(!cjk_ime_frame.text_runs.empty() &&
        cjk_ime_frame.text_runs.back().text == QStringLiteral("\u754c") &&
        cjk_ime_frame.text_runs.back().rect.width() == metrics().width * 2.0,
        "CJK preedit remains canonical overlay text instead of packed text ownership");
    return ok;
}

bool test_viewport_empty_and_dirty_ranges()
{
    bool ok = true;
    const term::Terminal_render_frame empty = term::build_terminal_render_frame(
        nullptr,
        QSizeF(160.0, 100.0),
        metrics(),
        options(),
        true);
    ok &= check(empty.background_rects.size() == 1U &&
        empty.text_runs.empty() &&
        empty.cursors.empty(),
        "missing snapshot produces empty background frame");

    term::Terminal_render_snapshot snapshot = empty_snapshot({3, 10});
    snapshot.viewport.scrollback_rows   = 5;
    snapshot.viewport.offset_from_tail  = 2;
    snapshot.dirty_row_ranges           = {{1, 2}};
    const term::Terminal_render_frame frame = build(snapshot);
    ok                                 &= check(frame.viewport.scrollback_rows == 5 &&
        frame.viewport.offset_from_tail == 2,
        "frame preserves scrolled viewport metadata");
    ok                                 &= check(frame.packed_rows.size() == 3U &&
        frame.packed_rows[0].active_buffer == term::Terminal_buffer_id::PRIMARY &&
        frame.packed_rows[0].logical_row == 3,
        "packed row identity follows primary scrollback viewport state");
    ok                                 &= check(frame.dirty_row_ranges.size() == 1U &&
        frame.dirty_row_ranges.front().first_row == 1 &&
        frame.dirty_row_ranges.front().row_count == 2,
        "frame preserves dirty row ranges");

    snapshot.viewport.active_buffer    = term::Terminal_buffer_id::ALTERNATE;
    snapshot.viewport.scrollback_rows  = 0;
    snapshot.viewport.offset_from_tail = 0;
    const term::Terminal_render_frame alternate_frame = build(snapshot);
    ok &= check(alternate_frame.viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE,
        "frame preserves alternate-screen viewport metadata");
    ok &= check(alternate_frame.packed_rows.size() == 3U &&
        alternate_frame.packed_rows[0].active_buffer ==
            term::Terminal_buffer_id::ALTERNATE &&
        alternate_frame.packed_rows[0].logical_row == 0,
        "packed row identity separates alternate-screen rows from primary rows");
    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_plain_text_color_inverse_and_wide_skip();
    ok &= test_terminal_graphics_use_grid_rects();
    ok &= test_rounded_box_corners_use_arc_primitives();
    ok &= test_block_cursor_over_graphic_has_overlay_rects();
    ok &= test_mixed_unicode_row_geometry();
    ok &= test_single_cell_unicode_text_is_not_clipped_to_cell();
    ok &= test_direct_ascii_text_runs_coalesce_contiguous_plain_cells();
    ok &= test_direct_ascii_text_runs_split_at_guard_boundaries();
    ok &= test_direct_ascii_text_runs_split_at_max_coalesced_length();
    ok &= test_inverse_and_reverse_video_xor();
    ok &= test_default_and_reverse_full_grid_background();
    ok &= test_styled_blank_cells_emit_background_rects();
    ok &= test_selection_cursor_blink_and_shapes();
    ok &= test_decorations_preedit_hyperlink_and_bell();
    ok &= test_malformed_snapshot_and_preedit_width();
    ok &= test_preedit_override_takes_precedence_over_snapshot();
    ok &= test_frame_stats_classify_rendered_cells();
    ok &= test_simple_content_classifier_and_stats();
    ok &= test_frame_stats_count_distinct_rows_and_styles_by_identity();
    ok &= test_classify_terminal_simple_content_cell_unicode_property();
    ok &= test_fragmented_dirty_ranges_drive_simple_content_membership();
    ok &= test_cell_pass_and_packed_pass_classification_agree();
    ok &= test_snapshot_cells_are_row_major_column_sorted();
    ok &= test_packed_text_rows_spans_and_order();
    ok &= test_packed_graphic_source_codepoint_spans();
    ok &= test_packed_graphics_exclude_unrepresented_cells();
    ok &= test_disabled_packed_text_sidecars_skip_scan_and_keep_graphics();
    ok &= test_packed_text_excludes_cursor_and_ime_cells();
    ok &= test_viewport_empty_and_dirty_ranges();
    return ok ? 0 : 1;
}
