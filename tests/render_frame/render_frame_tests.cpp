#include "vnm_terminal/internal/qsg_terminal_render_frame.h"
#include "helpers/test_check.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
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

term::Terminal_render_frame build_with_metrics(
    const term::Terminal_render_snapshot&  snapshot,
    term::terminal_cell_metrics_t          cell_metrics,
    term::Terminal_render_options          render_options = options())
{
    return
        term::build_terminal_render_frame(
            &snapshot,
            QSizeF(160.0, 100.0),
            cell_metrics,
            render_options,
            true);
}

term::Terminal_render_cell_text source_cell_text(
    const QString&  text,
    int             display_width,
    bool            wide_continuation = false)
{
    return term::Terminal_render_cell_text::from_source_cell(
        text,
        display_width,
        wide_continuation);
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

bool row_descriptors_equal(
    const term::Terminal_render_row_descriptor& left,
    const term::Terminal_render_row_descriptor& right)
{
    return
        left.row == right.row &&
        left.content_identity_key == right.content_identity_key &&
        left.text_key == right.text_key &&
        left.background_key == right.background_key &&
        left.graphic_key == right.graphic_key &&
        left.decoration_key == right.decoration_key &&
        left.cursor_inverse_text_key == right.cursor_inverse_text_key &&
        left.selection_key == right.selection_key &&
        left.ime_preedit_key == right.ime_preedit_key &&
        left.hyperlink_underline_key == right.hyperlink_underline_key;
}

bool row_descriptor_vectors_equal(
    const std::vector<term::Terminal_render_row_descriptor>& left,
    const std::vector<term::Terminal_render_row_descriptor>& right)
{
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!row_descriptors_equal(left[index], right[index])) {
            return false;
        }
    }

    return true;
}

bool layer_descriptors_equal(
    const term::Terminal_render_layer_descriptors& left,
    const term::Terminal_render_layer_descriptors& right)
{
    return
        left.text_key == right.text_key &&
        left.background_key == right.background_key &&
        left.graphic_key == right.graphic_key &&
        left.decoration_key == right.decoration_key &&
        left.cursor_inverse_text_key == right.cursor_inverse_text_key &&
        left.selection_key == right.selection_key &&
        left.ime_preedit_key == right.ime_preedit_key &&
        left.visual_bell_key == right.visual_bell_key &&
        left.hyperlink_underline_key == right.hyperlink_underline_key &&
        left.style_color_key == right.style_color_key &&
        left.reverse_video_key == right.reverse_video_key &&
        left.render_options_key == right.render_options_key &&
        left.cell_metrics_key == right.cell_metrics_key;
}

const term::Terminal_render_row_descriptor* descriptor_for_row(
    const term::Terminal_render_frame& frame,
    int                                row)
{
    for (const term::Terminal_render_row_descriptor& descriptor :
        frame.row_descriptors)
    {
        if (descriptor.row == row) {
            return &descriptor;
        }
    }

    return nullptr;
}

bool has_decoration(
    const term::Terminal_render_frame&       frame,
    term::Terminal_render_decoration_kind    kind)
{
    for (const term::Terminal_render_decoration& decoration : frame.decorations) {
        if (decoration.kind == kind) {
            return true;
        }
    }

    return false;
}

bool has_graphic_arc(
    const term::Terminal_render_frame& frame,
    term::Terminal_render_arc_kind     kind,
    const QRectF&                      rect)
{
    for (const term::Terminal_render_arc& arc : frame.graphic_arcs) {
        if (arc.kind == kind && arc.rect == rect) {
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

term::Terminal_render_snapshot_row_payload_ref row_payload_ref(
    std::shared_ptr<const term::Terminal_render_snapshot_row_payload_owner> owner,
    std::size_t                                                            row)
{
    term::Terminal_render_snapshot_row_payload_ref ref;
    ref.owner         = std::move(owner);
    ref.payload_index = row;
    return ref;
}

term::Terminal_render_snapshot lazy_snapshot_from_flat(
    const term::Terminal_render_snapshot& snapshot)
{
    const term::Terminal_render_snapshot_row_payload_namespace metadata_namespace = {
        31U,
        41U,
    };
    const std::shared_ptr<const term::Terminal_render_snapshot_row_payload_owner> owner =
        term::render_snapshot_row_payload_owner_from_snapshot(
            snapshot,
            metadata_namespace,
            snapshot.metadata.sequence);

    auto payloads = std::make_shared<term::Terminal_render_snapshot_lazy_payloads>();
    payloads->receiving_namespace = metadata_namespace;
    payloads->rows.reserve(static_cast<std::size_t>(snapshot.grid_size.rows));
    for (int row = 0; row < snapshot.grid_size.rows; ++row) {
        const std::optional<term::Terminal_render_snapshot_lazy_row_payload> payload =
            term::borrowed_render_snapshot_lazy_row_payload(
                row_payload_ref(owner, static_cast<std::size_t>(row)),
                metadata_namespace,
                {},
                {});
        if (payload.has_value()) {
            payloads->rows.push_back(*payload);
        }
    }

    term::Terminal_render_snapshot lazy = snapshot;
    lazy.cells.clear();
    lazy.lazy_row_payloads = std::move(payloads);
    return lazy;
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

bool test_terminal_graphics_use_grid_primitives()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({3, 8});
    snapshot.cursor.visible = false;

    const auto add_graphic_cell = [&](
        int             row,
        int             column,
        const QString&  text) {
        snapshot.cells.push_back({
            {row, column},
            source_cell_text(text, 1),
            0U,
            1,
            false,
            0U,
        });
    };

    add_graphic_cell(0, 0, QStringLiteral("\u250c"));
    add_graphic_cell(0, 1, QStringLiteral("\u2500"));
    add_graphic_cell(0, 2, QStringLiteral("\u2510"));
    add_graphic_cell(1, 0, QStringLiteral("\u2502"));
    add_graphic_cell(1, 1, QStringLiteral("\u2588"));
    add_graphic_cell(1, 2, QStringLiteral("\u2598"));
    add_graphic_cell(1, 3, QStringLiteral("\u2592"));
    add_graphic_cell(1, 4, QStringLiteral("\u2801"));
    snapshot.cells.push_back({{2, 0}, QStringLiteral("A"), 0U, 1, false, 0U});

    const term::Terminal_render_frame frame = build(snapshot);
    ok &= check(std::all_of(
        snapshot.cells.begin(),
        snapshot.cells.begin() + 8,
        [](const term::Terminal_render_cell& cell) {
            return
                cell.text.is_inline_single_bmp() &&
                cell.text.fallback_qstring_or_null() == nullptr;
        }), "terminal graphic fixture stores source graphics as inline BMP");
    ok &= check(frame.text_runs.size() == 2U &&
        run_at(frame, 1, 4) != nullptr &&
        run_at(frame, 2, 0) != nullptr,
        "Braille and ASCII remain on text runs");
    ok &= check(has_graphic_rect(frame, QRectF(10.0, 9.5, 10.0, 1.0)),
        "horizontal box drawing uses grid-aligned graphic rectangles");
    ok &= check(has_graphic_rect(frame, QRectF(10.0, 20.0, 10.0, 20.0)),
        "full block fills the entire cell");
    ok &= check(has_graphic_rect(frame, QRectF(20.0, 20.0, 5.0, 10.0)),
        "quadrant block uses cell-grid geometry");
    ok &= check(has_graphic_rect(frame, QRectF(30.0, 20.0, 10.0, 20.0)),
        "shade block fills the full cell with alpha geometry");
    ok &= check(frame.stats.text_cells_rendered_as_text == 2 &&
        frame.stats.graphic_rects_emitted >= 6              &&
        frame.stats.graphic_arcs_emitted == 0,
        "terminal graphics are counted as primitive output, not text glyphs");
    return ok;
}

bool test_full_block_graphic_rects_coalesce_contiguous_cells()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({1, 5});
    snapshot.cursor.visible = false;
    snapshot.cells.push_back({
        {0, 0},
        source_cell_text(QStringLiteral("\u2588"), 1),
        0U,
        1,
        false,
        0U,
    });
    snapshot.cells.push_back({
        {0, 1},
        source_cell_text(QStringLiteral("\u2588"), 1),
        0U,
        1,
        false,
        0U,
    });
    snapshot.cells.push_back({
        {0, 3},
        source_cell_text(QStringLiteral("\u2588"), 1),
        0U,
        1,
        false,
        0U,
    });

    const term::Terminal_render_frame frame = build(snapshot);
    ok &= check(frame.graphic_rects.size() == 2U,
        "adjacent full block elements coalesce into row spans");
    ok &= check(has_graphic_rect(frame, QRectF(0.0, 0.0, 20.0, 20.0)),
        "coalesced full block span covers adjacent cells");
    ok &= check(has_graphic_rect(frame, QRectF(30.0, 0.0, 10.0, 20.0)),
        "full block coalescing preserves gaps");
    ok &= check(frame.text_runs.empty(),
        "coalesced full block elements bypass text runs");
    return ok;
}

bool test_decorated_full_block_graphic_emits_decorations_without_text_run()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({1, 1});
    snapshot.cursor.visible = false;

    term::Terminal_text_style decorated = term::make_default_terminal_text_style();
    term::set_terminal_style_attribute(decorated, term::Terminal_style_attribute::UNDERLINE);
    term::set_terminal_style_attribute(decorated, term::Terminal_style_attribute::STRIKE);
    snapshot.styles.push_back(decorated);
    snapshot.hyperlinks.push_back({
        17U,
        QByteArrayLiteral("uri:https://example.test"),
        QByteArrayLiteral("https://example.test"),
    });
    snapshot.cells.push_back({
        {0, 0},
        source_cell_text(QStringLiteral("\u2588"), 1),
        17U,
        1,
        false,
        1U,
    });

    term::Terminal_render_options render_options = options();
    render_options.underline_hyperlinks = true;
    const term::Terminal_render_frame frame = build(snapshot, render_options);

    ok &= check(frame.graphic_rects.size() == 1U &&
        has_graphic_rect(frame, QRectF(0.0, 0.0, 10.0, 20.0)),
        "decorated full block cell still emits primitive graphic geometry");
    ok &= check(frame.text_runs.empty(),
        "decorated full block primitive bypasses text-run emission");
    ok &= check(frame.decorations.size() == 3U &&
        has_decoration(frame, term::Terminal_render_decoration_kind::UNDERLINE) &&
        has_decoration(frame, term::Terminal_render_decoration_kind::STRIKE)    &&
        has_decoration(
            frame,
            term::Terminal_render_decoration_kind::HYPERLINK_UNDERLINE),
        "decorated full block primitive emits underline, strike, and hyperlink underline");
    ok &= check(frame.stats.text_cells_rendered_as_text == 0 &&
        frame.stats.text_cells_with_decorations == 1         &&
        frame.stats.decoration_rects_emitted == 3,
        "decorated full block primitive updates decoration stats without text stats");
    return ok;
}

bool test_stale_inline_bmp_category_uses_text_fallback()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({1, 2});
    snapshot.cells.push_back({
        {0, 0},
        source_cell_text(QStringLiteral("\u2588"), 1),
        0U,
        1,
        false,
        0U,
        term::Terminal_render_cell_text_category::PRINTABLE_ASCII,
    });

    const term::terminal_simple_content_classification_t classification =
        term::classify_terminal_simple_content_cell(
            snapshot.cells.front(),
            snapshot.grid_size,
            snapshot.styles.size(),
            false,
            true);
    ok &= check(classification.route == term::Terminal_simple_content_route::FALLBACK &&
        classification.rejection_reason ==
            term::Terminal_simple_content_rejection_reason::INVALID_TEXT_ENCODING,
        "stale inline-BMP cached category is rejected before text routing");

    const term::Terminal_render_frame frame = build(snapshot);
    ok &= check(frame.graphic_rects.empty() &&
        frame.graphic_arcs.empty()          &&
        frame.text_runs.size() == 1U        &&
        frame.text_runs.front().text == QStringLiteral("\u2588"),
        "stale inline-BMP graphic cell renders through the text fallback path");
    return ok;
}

bool test_invalid_graphic_cells_stay_on_text_route()
{
    bool ok = true;

    enum class text_storage_t
    {
        FALLBACK_QSTRING,
        INLINE_SINGLE_BMP,
    };

    struct rejected_graphic_case_t
    {
        const char*     message        = "";
        text_storage_t  storage        = text_storage_t::FALLBACK_QSTRING;
        int             display_width  = 1;
        term::Terminal_simple_content_rejection_reason
                        expected_reason =
                            term::Terminal_simple_content_rejection_reason::INVALID_TEXT_WIDTH;
    };

    const std::vector<rejected_graphic_case_t> cases = {
        {
            "fallback QString graphic with invalid text width stays textual",
            text_storage_t::FALLBACK_QSTRING,
            2,
            term::Terminal_simple_content_rejection_reason::INVALID_TEXT_WIDTH,
        },
        {
            "inline BMP graphic with invalid text width stays textual",
            text_storage_t::INLINE_SINGLE_BMP,
            2,
            term::Terminal_simple_content_rejection_reason::INVALID_TEXT_WIDTH,
        },
    };

    const QString graphic_text = QStringLiteral("\u2500");
    for (const rejected_graphic_case_t& entry : cases) {
        term::Terminal_render_snapshot snapshot = empty_snapshot({1, 4});
        snapshot.cursor.visible = false;

        term::Terminal_render_cell cell;
        cell.position      = {0, 0};
        cell.text          = entry.storage == text_storage_t::INLINE_SINGLE_BMP
            ? source_cell_text(graphic_text, entry.display_width)
            : term::Terminal_render_cell_text::fallback(graphic_text);
        cell.display_width = entry.display_width;
        snapshot.cells.push_back(cell);

        const term::terminal_simple_content_classification_t classification =
            term::classify_terminal_simple_content_cell(
                snapshot.cells.front(),
                snapshot.grid_size,
                snapshot.styles.size(),
                false,
                true);
        ok &= check(!classification.fast_text_eligible &&
            classification.route == term::Terminal_simple_content_route::QT_TEXT_LAYOUT &&
            classification.rejection_reason == entry.expected_reason,
            entry.message);

        const term::Terminal_render_frame frame = build(snapshot);
        ok &= check(frame.graphic_rects.empty()                   &&
            frame.graphic_arcs.empty(),
            "rejected text-like symbol emits no direct graphic geometry");
        ok &= check(frame.text_runs.size() == 1U &&
            frame.text_runs.front().text == graphic_text,
            "rejected graphic cell remains available to Qt text layout");
    }

    return ok;
}

bool test_rounded_box_corners_use_arc_primitives()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({1, 4});
    snapshot.cursor.visible = false;
    snapshot.cells.push_back({{0, 0}, QStringLiteral("\u256d"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 1}, QStringLiteral("\u256e"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 2}, QStringLiteral("\u256f"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 3}, QStringLiteral("\u2570"), 0U, 1, false, 0U});

    const term::Terminal_render_frame frame = build(snapshot);
    ok &= check(frame.text_runs.empty(),
        "rounded box corners bypass shaped text runs");
    ok &= check(frame.graphic_rects.empty(),
        "rounded box corners do not fall back to square corner rectangles");
    ok &= check(frame.graphic_arcs.size() == 4U,
        "rounded box corners produce arc primitives");
    ok &= check(has_graphic_arc(
        frame,
        term::Terminal_render_arc_kind::DOWN_RIGHT,
        QRectF(0.0, 0.0, 10.0, 20.0)),
        "rounded down-right corner produces a down-right arc");
    ok &= check(has_graphic_arc(
        frame,
        term::Terminal_render_arc_kind::DOWN_LEFT,
        QRectF(10.0, 0.0, 10.0, 20.0)),
        "rounded down-left corner produces a down-left arc");
    ok &= check(has_graphic_arc(
        frame,
        term::Terminal_render_arc_kind::UP_LEFT,
        QRectF(20.0, 0.0, 10.0, 20.0)),
        "rounded up-left corner produces an up-left arc");
    ok &= check(has_graphic_arc(
        frame,
        term::Terminal_render_arc_kind::UP_RIGHT,
        QRectF(30.0, 0.0, 10.0, 20.0)),
        "rounded up-right corner produces an up-right arc");
    return ok;
}

bool test_block_cursor_over_text_like_symbol_uses_cursor_text()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({2, 4});
    snapshot.cursor = {{0, 1}, term::Terminal_cursor_shape::BLOCK, true, false};
    snapshot.cells.push_back({{0, 1}, QStringLiteral("\u2500"), 0U, 1, false, 0U});

    const term::Terminal_render_frame frame = build(snapshot);
    ok &= check(frame.text_runs.size() == 1U &&
        frame.text_runs.front().text == QStringLiteral("\u2500"),
        "cursor-covered text-like symbol remains in main text runs");
    ok &= check(frame.cursors.size() == 1U &&
        frame.cursors.front().kind == term::Terminal_cursor_shape::BLOCK,
        "block cursor over text-like symbol uses one normal cursor rect");
    ok &= check(frame.graphic_rects.empty() &&
        frame.graphic_arcs.empty(),
        "block cursor over text-like symbol emits no graphic overlay primitives");
    ok &= check(frame.cursor_text_runs.size() == 1U &&
        frame.cursor_text_runs.front().text == QStringLiteral("\u2500") &&
        frame.cursor_text_runs.front().clip_rect == frame.cursors.front().rect,
        "block cursor inverse text comes from the shaped text run");
    return ok;
}

bool test_terminal_graphics_use_geometry_with_selection_and_ime_text()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({2, 6});
    snapshot.cursor.visible                 = false;
    snapshot.cursor.position                = {1, 1};
    snapshot.selection_spans.push_back({
        {{0, 0}, {0, 3}, term::Terminal_selection_mode::NORMAL},
        0,
        0,
        3,
    });
    snapshot.ime_preedit.active          = true;
    snapshot.ime_preedit.text            = QStringLiteral("\u2801");
    snapshot.ime_preedit.cursor_position = 1;
    snapshot.cells.push_back({{0, 0}, QStringLiteral("\u250c"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 1}, QStringLiteral("\u2500"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 2}, QStringLiteral("\u2510"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{1, 1}, QStringLiteral("\u2588"), 0U, 1, false, 0U});

    const term::Terminal_render_frame frame = build(snapshot);
    ok &= check(frame.text_runs.size() == 2U &&
        run_at(frame, 1, 1) != nullptr &&
        frame.text_runs.back().text == QStringLiteral("\u2801"),
        "IME-covered terminal graphic remains on shaped text runs");
    ok &= check(frame.selection_rects.size() == 2U &&
        frame.selection_rects[0].rect == QRectF(0.0, 0.0, 30.0, 20.0) &&
        frame.selection_rects[1].rect == QRectF(10.0, 20.0, 10.0, 20.0),
        "selection and IME background rectangles cover terminal graphic cells");
    ok &= check(!frame.graphic_rects.empty() &&
        frame.stats.graphic_arcs_emitted == 0,
        "unprotected selected box drawing uses graphic geometry");
    return ok;
}

bool test_mixed_unicode_row_geometry()
{
    bool ok = true;
    term::Terminal_render_snapshot snapshot = empty_snapshot({2, 14});
    snapshot.cells.push_back({{0, 0}, QStringLiteral("A"), 0U, 1, false, 0U});
    snapshot.cells.push_back({{0, 1}, source_cell_text(QStringLiteral("\u754c"), 2), 0U, 2, false, 0U});
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
    ok &= check(snapshot.cells[1].text.is_inline_single_bmp() &&
        snapshot.cells[1].text.fallback_qstring_or_null() == nullptr,
        "mixed row CJK fixture stores the leading wide cell as inline BMP");
    ok &= check(frame.text_runs.size() == 9U,
        "mixed row skips wide continuations but keeps base clusters");
    ok &= check(run_at(frame, 0, 0) != nullptr &&
        run_at(frame, 0, 0)->rect == QRectF(0.0, 0.0, 10.0, 20.0),
        "ASCII run stays in column 0");
    ok &= check(run_at(frame, 0, 1) != nullptr &&
        run_at(frame, 0, 1)->text == QStringLiteral("\u754c") &&
        run_at(frame, 0, 1)->rect == QRectF(10.0, 0.0, 20.0, 20.0),
        "inline BMP CJK wide run stays on the text route and spans columns 1-2");
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

bool test_coalesced_ascii_text_runs_coalesce_contiguous_plain_cells()
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
        "coalesced ASCII still counts the rendered source cells");
    ok &= check(frame.stats.compact_ascii_cells_seen == 3 &&
        frame.stats.compact_ascii_text_direct_appends == 3 &&
        frame.stats.compact_ascii_qstring_materializations == 0,
        "compact ASCII frame path appends code units without per-cell materialization");
    return ok;
}

bool test_coalesced_ascii_text_runs_split_at_guard_boundaries()
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
        "coalesced ASCII splits at foreground/background/style boundaries");

    const std::vector<const term::Terminal_render_text_run*> decoration_runs =
        text_runs_for_row(frame, 1);
    ok &= check(decoration_runs.size() == 3U &&
        decoration_runs[1]->text == QStringLiteral("E") &&
        decoration_runs[1]->underline,
        "coalesced ASCII splits at decoration boundaries");

    const std::vector<const term::Terminal_render_text_run*> hyperlink_runs =
        text_runs_for_row(frame, 2);
    ok &= check(hyperlink_runs.size() == 3U &&
        hyperlink_runs[1]->text == QStringLiteral("H") &&
        hyperlink_runs[1]->hyperlink_id == 17U,
        "coalesced ASCII splits at hyperlink boundaries");

    const std::vector<const term::Terminal_render_text_run*> gap_runs =
        text_runs_for_row(frame, 3);
    ok &= check(gap_runs.size() == 2U &&
        gap_runs[0]->text == QStringLiteral("J") &&
        gap_runs[1]->column == 2 &&
        gap_runs[1]->text == QStringLiteral("K"),
        "coalesced ASCII splits across column gaps");

    const std::vector<const term::Terminal_render_text_run*> cursor_runs =
        text_runs_for_row(frame, 4);
    ok &= check(cursor_runs.size() == 3U &&
        cursor_runs[0]->text == QStringLiteral("L") &&
        cursor_runs[1]->text == QStringLiteral("M") &&
        cursor_runs[2]->text == QStringLiteral("N"),
        "coalesced ASCII splits around a block-cursor-covered cell");
    ok &= check(frame.cursor_text_runs.size() == 1U &&
        frame.cursor_text_runs.front().text == QStringLiteral("M"),
        "block cursor overlay copies only the protected standalone cell");
    return ok;
}

bool test_coalesced_ascii_text_runs_split_at_max_coalesced_length()
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
        "coalesced ASCII caps the maximum text-run length");
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

bool test_zero_grid_and_preedit_width()
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
        frame.cursors.empty() &&
        frame.row_descriptors.empty(),
        "zero-grid active preedit creates no primitives");

    term::Terminal_render_snapshot snapshot = empty_snapshot({2, 8});
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
    ok &= check(frame.stats.cells_considered == 5,
        "frame stats count row-view cells considered");
    ok &= check(frame.stats.cells_skipped_wide_continuation == 1 &&
        frame.stats.cells_skipped_invalid == 0 &&
        frame.stats.cells_rendered == 4,
        "frame stats distinguish skipped row-view cells and rendered cells");
    ok &= check(frame.stats.text_cells_rendered_as_text == 3 &&
        frame.graphic_rects.size() == 1U &&
        frame.graphic_arcs.empty(),
        "frame stats count block elements as graphic primitives");
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
        frame.stats.graphic_rects_emitted == 1 &&
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

    term::Terminal_render_cell inline_cjk_cell = ascii_cell;
    inline_cjk_cell.text          = source_cell_text(QStringLiteral("\u754c"), 2);
    inline_cjk_cell.display_width = 2;
    const term::terminal_simple_content_classification_t inline_cjk_classification =
        term::classify_terminal_simple_content_cell(
            inline_cjk_cell,
            grid_size,
            2U,
            false,
            false);
    ok &= check(!inline_cjk_classification.fast_text_eligible &&
        inline_cjk_classification.route ==
            term::Terminal_simple_content_route::QT_TEXT_LAYOUT &&
        inline_cjk_classification.rejection_reason ==
            term::Terminal_simple_content_rejection_reason::MULTI_CELL_TEXT,
        "simple-content classifier keeps inline BMP CJK on the Qt text route");

    term::Terminal_render_cell graphic_cell = ascii_cell;
    graphic_cell.text = source_cell_text(QStringLiteral("\u2588"), 1);
    const term::terminal_simple_content_classification_t graphic_classification =
        term::classify_terminal_simple_content_cell(
            graphic_cell,
            grid_size,
            2U,
            false,
            false);
    ok &= check(!graphic_classification.fast_text_eligible &&
        graphic_classification.route ==
            term::Terminal_simple_content_route::QT_TEXT_LAYOUT &&
        graphic_classification.rejection_reason ==
            term::Terminal_simple_content_rejection_reason::NON_ASCII_TEXT,
        "simple-content classifier keeps block elements on the Qt text route");

    term::Terminal_render_cell stale_graphic_cell = graphic_cell;
    stale_graphic_cell.text_category =
        term::Terminal_render_cell_text_category::PRINTABLE_ASCII;
    const term::terminal_simple_content_classification_t stale_graphic_classification =
        term::classify_terminal_simple_content_cell(
            stale_graphic_cell,
            grid_size,
            2U,
            false,
            false);
    ok &= check(!stale_graphic_classification.fast_text_eligible &&
        stale_graphic_classification.route ==
            term::Terminal_simple_content_route::FALLBACK &&
        stale_graphic_classification.rejection_reason ==
            term::Terminal_simple_content_rejection_reason::INVALID_TEXT_ENCODING,
        "simple-content classifier fails closed on stale inline BMP categories");

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
        simple_stats.route_qt_text_layout_cells == 3 &&
        simple_stats.route_none_cells == 1,
        "simple-content frame stats count canonical routes");
    ok &= check(simple_stats.rejection_none_cells == 3 &&
        simple_stats.rejection_non_ascii_text_cells == 2 &&
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

    const term::Terminal_render_frame frame = build(snapshot, options());
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
    ok &= check(frame.stats.dirty_row_lookup_count == 4,
        "fragmented dirty row membership preserves logical lookup probe count");
    ok &= check(frame.stats.cell_pass_classification_calls == 4,
        "fragmented dirty row membership exposes cell-pass classification calls");

    return ok;
}

bool test_cell_pass_simple_content_classification()
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
        int            expected_dirty_fast   = 0;
        int            expected_clean_fast   = 0;
    };

    const std::vector<agreement_case_t> cases = {
        {
            "dirty ASCII fast-text cell classifies as fast text",
            QStringLiteral("A"),
            decoration_t::NONE,
            0U,
            true,
            term::Terminal_simple_content_route::FAST_TEXT,
            term::Terminal_simple_content_rejection_reason::NONE,
            1,
            0,
        },
        {
            "clean ASCII fast-text cell classifies as fast text",
            QStringLiteral("B"),
            decoration_t::NONE,
            0U,
            false,
            term::Terminal_simple_content_route::FAST_TEXT,
            term::Terminal_simple_content_rejection_reason::NONE,
            0,
            1,
        },
        {
            "non-ASCII text cell stays off the fast-text route",
            QStringLiteral("\u03c0"),
            decoration_t::NONE,
            0U,
            true,
            term::Terminal_simple_content_route::QT_TEXT_LAYOUT,
            term::Terminal_simple_content_rejection_reason::NON_ASCII_TEXT,
        },
        {
            "block-element cell agrees on text route",
            QStringLiteral("\u2588"),
            decoration_t::NONE,
            0U,
            true,
            term::Terminal_simple_content_route::QT_TEXT_LAYOUT,
            term::Terminal_simple_content_rejection_reason::NON_ASCII_TEXT,
        },
        {
            "underlined ASCII cell stays off the fast-text route",
            QStringLiteral("D"),
            decoration_t::UNDERLINE,
            0U,
            true,
            term::Terminal_simple_content_route::QT_TEXT_LAYOUT,
            term::Terminal_simple_content_rejection_reason::DECORATION,
        },
        {
            "strike-only ASCII cell stays off the fast-text route",
            QStringLiteral("E"),
            decoration_t::STRIKE,
            0U,
            true,
            term::Terminal_simple_content_route::QT_TEXT_LAYOUT,
            term::Terminal_simple_content_rejection_reason::DECORATION,
        },
        {
            "hyperlink ASCII cell stays off the fast-text route",
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
            stats.rejection_hyperlink_cells;
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
        ok &= check(frame.stats.cell_pass_classification_calls == 1,
            "single-cell case classifies once in the cell pass");
        ok &= check(simple_stats.cells_considered == 1 &&
            route_total(simple_stats) == 1 &&
            route_count(simple_stats, classification.route) == 1 &&
            rejection_total(simple_stats) == 1 &&
            rejection_count(simple_stats, classification.rejection_reason) == 1,
            "single-cell case records the direct route and rejection");
        ok &= check(simple_stats.dirty_eligible_cells == entry.expected_dirty_fast &&
            simple_stats.clean_eligible_cells == entry.expected_clean_fast,
            "single-cell case preserves dirty-row eligibility");
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

    term::Terminal_render_snapshot unsorted_snapshot = snapshot;
    std::swap(unsorted_snapshot.cells[0], unsorted_snapshot.cells[1]);
    ok &= check(!snapshot_cells_are_row_major_column_sorted(unsorted_snapshot),
        "snapshot cell order fixture detects unsorted columns");

    return ok;
}

bool test_row_view_frame_matches_flat_materialization()
{
    bool ok = true;

    term::Terminal_render_snapshot snapshot = empty_snapshot({3, 6});
    snapshot.cursor.visible = false;
    snapshot.dirty_row_ranges = {{0, 3}};
    snapshot.styles.push_back(term::make_default_terminal_text_style());
    snapshot.cells.push_back({.position = {0, 0}, .text = QStringLiteral("A")});
    snapshot.cells.push_back({.position = {0, 1}, .text = QStringLiteral("B")});
    snapshot.cells.push_back({
        .position = {1, 0},
        .text     = source_cell_text(QStringLiteral("\u2588"), 1),
        .style_id = 1U,
    });
    snapshot.cells.push_back({
        .position     = {2, 0},
        .text         = QStringLiteral("H"),
        .hyperlink_id = 5U,
    });
    snapshot.selection_spans.push_back({
        {{2, 0}, {2, 1}, term::Terminal_selection_mode::NORMAL},
        2,
        0,
        1,
    });

    const term::Terminal_render_snapshot lazy_snapshot =
        lazy_snapshot_from_flat(snapshot);
    const term::Terminal_render_snapshot_row_content_view lazy_rows(lazy_snapshot);
    term::Terminal_render_snapshot materialized_snapshot = lazy_snapshot;
    materialized_snapshot.cells = lazy_rows.materialize_flat_cells(
        term::Terminal_render_snapshot_materialization_reason::ROW_VIEW_PARITY_TEST);
    materialized_snapshot.lazy_row_payloads.reset();

    term::Terminal_render_options render_options = options();
    render_options.underline_hyperlinks = true;
    const term::Terminal_render_frame row_view_frame =
        build(lazy_snapshot, render_options);
    const term::Terminal_render_frame flat_frame =
        build(materialized_snapshot, render_options);

    ok &= check(
        row_view_frame.stats.cells_considered == flat_frame.stats.cells_considered &&
            row_view_frame.stats.cell_pass_input_cells ==
                flat_frame.stats.cell_pass_input_cells,
        "row-content frame and explicit materialized frame consider the same full rows");
    ok &= check(
        row_view_frame.text_runs.size() == flat_frame.text_runs.size() &&
            row_view_frame.graphic_rects.size() == flat_frame.graphic_rects.size() &&
            row_view_frame.selection_rects.size() == flat_frame.selection_rects.size() &&
            row_view_frame.decorations.size() == flat_frame.decorations.size(),
        "row-content frame emits the same primitive counts as flat materialization");
    ok &= check(
        layer_descriptors_equal(
            row_view_frame.layer_descriptors,
            flat_frame.layer_descriptors),
        "row-content frame layer descriptors match explicit flat materialization");
    ok &= check(
        row_view_frame.row_descriptors.size() ==
                static_cast<std::size_t>(snapshot.grid_size.rows) &&
            row_descriptor_vectors_equal(
                row_view_frame.row_descriptors,
                flat_frame.row_descriptors),
        "row-content frame row descriptors match explicit flat materialization");
    return ok;
}

bool test_descriptor_keys_are_mutation_sensitive()
{
    bool ok = true;

    term::Terminal_render_snapshot base = empty_snapshot({3, 6});
    base.cursor.visible   = false;
    base.dirty_row_ranges = {{0, 3}};
    base.cells.push_back({
        .position = {1, 1},
        .text     = QStringLiteral("A"),
    });
    base.cells.push_back({
        .position     = {2, 1},
        .text         = QStringLiteral("H"),
        .hyperlink_id = 7U,
    });

    const term::Terminal_render_frame base_frame = build(base);
    const term::Terminal_render_row_descriptor* const base_row_1 =
        descriptor_for_row(base_frame, 1);
    const term::Terminal_render_row_descriptor* const base_row_2 =
        descriptor_for_row(base_frame, 2);
    ok &= check(base_row_1 != nullptr && base_row_2 != nullptr,
        "descriptor sensitivity fixture builds base row descriptors");
    if (base_row_1 == nullptr || base_row_2 == nullptr) {
        return false;
    }

    term::Terminal_render_snapshot text = base;
    text.cells[0].text = QStringLiteral("B");
    const term::Terminal_render_frame text_frame = build(text);
    const term::Terminal_render_row_descriptor* const text_row =
        descriptor_for_row(text_frame, 1);
    ok &= check(
        text_frame.layer_descriptors.text_key !=
                base_frame.layer_descriptors.text_key &&
            text_row != nullptr &&
            text_row->content_identity_key != base_row_1->content_identity_key &&
            text_row->text_key != base_row_1->text_key,
        "text mutations change content identity and text row descriptor keys");

    term::Terminal_render_snapshot background = base;
    background.color_state.default_background_rgba = 0xff143c78U;
    const term::Terminal_render_frame background_frame = build(background);
    const term::Terminal_render_row_descriptor* const background_row =
        descriptor_for_row(background_frame, 1);
    ok &= check(
        background_frame.layer_descriptors.background_key !=
                base_frame.layer_descriptors.background_key &&
            background_row != nullptr &&
            background_row->background_key != base_row_1->background_key,
        "background mutations change layer and row descriptor keys");

    term::Terminal_render_snapshot graphic_rect = base;
    graphic_rect.cells[0].text = source_cell_text(QStringLiteral("\u2588"), 1);
    const term::Terminal_render_frame graphic_rect_frame = build(graphic_rect);
    const term::Terminal_render_row_descriptor* const graphic_rect_row =
        descriptor_for_row(graphic_rect_frame, 1);
    ok &= check(
        graphic_rect_frame.layer_descriptors.graphic_key !=
                base_frame.layer_descriptors.graphic_key &&
            graphic_rect_row != nullptr &&
            graphic_rect_row->graphic_key != base_row_1->graphic_key,
        "graphic rect mutations change layer and row descriptor keys");

    term::Terminal_render_snapshot graphic_arc = base;
    graphic_arc.cells.clear();
    graphic_arc.cells.push_back({{1, 0}, QStringLiteral("\u256d"), 0U, 1, false, 0U});
    graphic_arc.cells.push_back({{1, 1}, QStringLiteral("\u256e"), 0U, 1, false, 0U});
    graphic_arc.cells.push_back({{1, 2}, QStringLiteral("\u256f"), 0U, 1, false, 0U});
    graphic_arc.cells.push_back({{1, 3}, QStringLiteral("\u2570"), 0U, 1, false, 0U});
    const term::Terminal_render_frame graphic_arc_frame = build(graphic_arc);
    const term::Terminal_render_row_descriptor* const graphic_arc_row =
        descriptor_for_row(graphic_arc_frame, 1);
    ok &= check(
        graphic_arc_frame.graphic_rects.empty() &&
            graphic_arc_frame.layer_descriptors.graphic_key !=
                base_frame.layer_descriptors.graphic_key &&
            graphic_arc_row != nullptr &&
            graphic_arc_row->graphic_key != base_row_1->graphic_key,
        "arc-only graphic mutations change layer and row descriptor keys");

    term::Terminal_render_snapshot decoration = base;
    term::Terminal_text_style decorated = term::make_default_terminal_text_style();
    term::set_terminal_style_attribute(
        decorated,
        term::Terminal_style_attribute::UNDERLINE);
    decoration.styles.push_back(decorated);
    decoration.cells[0].style_id = 1U;
    const term::Terminal_render_frame decoration_frame = build(decoration);
    const term::Terminal_render_row_descriptor* const decoration_row =
        descriptor_for_row(decoration_frame, 1);
    ok &= check(
        decoration_frame.layer_descriptors.decoration_key !=
                base_frame.layer_descriptors.decoration_key &&
            decoration_row != nullptr &&
            decoration_row->decoration_key != base_row_1->decoration_key,
        "decoration mutations change layer and row descriptor keys");

    term::Terminal_render_snapshot cursor = base;
    cursor.cursor.visible       = true;
    cursor.cursor.blink_enabled = false;
    cursor.cursor.shape         = term::Terminal_cursor_shape::BLOCK;
    cursor.cursor.position      = {1, 1};
    const term::Terminal_render_frame cursor_frame = build(cursor);
    const term::Terminal_render_row_descriptor* const cursor_row =
        descriptor_for_row(cursor_frame, 1);
    ok &= check(
        cursor_frame.layer_descriptors.cursor_inverse_text_key !=
                base_frame.layer_descriptors.cursor_inverse_text_key &&
            cursor_row != nullptr &&
            cursor_row->cursor_inverse_text_key !=
                base_row_1->cursor_inverse_text_key,
        "cursor inverse-text mutations change layer and row descriptor keys");

    term::Terminal_render_snapshot selection = base;
    selection.selection_spans.push_back({
        {{1, 1}, {1, 2}, term::Terminal_selection_mode::NORMAL},
        1,
        1,
        1,
    });
    const term::Terminal_render_frame selection_frame = build(selection);
    const term::Terminal_render_row_descriptor* const selection_row =
        descriptor_for_row(selection_frame, 1);
    ok &= check(
        selection_frame.layer_descriptors.selection_key !=
                base_frame.layer_descriptors.selection_key &&
            selection_row != nullptr &&
            selection_row->selection_key != base_row_1->selection_key,
        "selection mutations change layer and row descriptor keys");

    term::Terminal_render_snapshot preedit = base;
    preedit.cursor.position                = {1, 2};
    preedit.ime_preedit.active             = true;
    preedit.ime_preedit.text               = QStringLiteral("xy");
    preedit.ime_preedit.cursor_position    = 1;
    const term::Terminal_render_frame preedit_frame = build(preedit);
    const term::Terminal_render_row_descriptor* const preedit_row =
        descriptor_for_row(preedit_frame, 1);
    ok &= check(
        preedit_frame.layer_descriptors.ime_preedit_key !=
                base_frame.layer_descriptors.ime_preedit_key &&
            preedit_row != nullptr &&
            preedit_row->ime_preedit_key != base_row_1->ime_preedit_key,
        "IME preedit mutations change layer and row descriptor keys");

    term::Terminal_render_snapshot visual_bell = base;
    visual_bell.metadata.visual_bell_active = true;
    const term::Terminal_render_frame visual_bell_frame = build(visual_bell);
    ok &= check(
        visual_bell_frame.layer_descriptors.visual_bell_key !=
            base_frame.layer_descriptors.visual_bell_key,
        "visual bell mutations change the layer descriptor key");

    term::Terminal_render_snapshot style_color = base;
    style_color.color_state.default_foreground_rgba = 0xff33c7ffU;
    const term::Terminal_render_frame style_color_frame = build(style_color);
    ok &= check(
        style_color_frame.layer_descriptors.style_color_key !=
            base_frame.layer_descriptors.style_color_key,
        "style and color mutations change the layer descriptor key");

    term::Terminal_render_snapshot reverse_video = base;
    reverse_video.modes.reverse_video = true;
    const term::Terminal_render_frame reverse_video_frame = build(reverse_video);
    ok &= check(
        reverse_video_frame.layer_descriptors.reverse_video_key !=
            base_frame.layer_descriptors.reverse_video_key,
        "reverse-video mutations change the layer descriptor key");

    term::Terminal_render_options render_options = options();
    render_options.cursor_color = QColor(64, 128, 192);
    const term::Terminal_render_frame options_frame =
        build(base, render_options);
    ok &= check(
        options_frame.layer_descriptors.render_options_key !=
            base_frame.layer_descriptors.render_options_key,
        "render option mutations change the layer descriptor key");

    term::terminal_cell_metrics_t changed_metrics = metrics();
    changed_metrics.width = 11.0;
    const term::Terminal_render_frame metrics_frame =
        build_with_metrics(base, changed_metrics);
    ok &= check(
        metrics_frame.layer_descriptors.cell_metrics_key !=
            base_frame.layer_descriptors.cell_metrics_key,
        "metric mutations change the layer descriptor key");

    term::Terminal_render_options hyperlink_options = options();
    hyperlink_options.underline_hyperlinks = true;
    const term::Terminal_render_frame hyperlink_frame =
        build(base, hyperlink_options);
    const term::Terminal_render_row_descriptor* const hyperlink_row =
        descriptor_for_row(hyperlink_frame, 2);
    ok &= check(
        hyperlink_frame.layer_descriptors.hyperlink_underline_key !=
                base_frame.layer_descriptors.hyperlink_underline_key &&
            hyperlink_row != nullptr &&
            hyperlink_row->hyperlink_underline_key !=
                base_row_2->hyperlink_underline_key,
        "hyperlink underline policy changes layer and row descriptor keys");
    return ok;
}

bool test_sparse_lazy_frame_scales_with_dirty_rows()
{
    bool ok = true;

    term::Terminal_render_snapshot snapshot = empty_snapshot({4, 5});
    snapshot.cursor.visible = false;
    snapshot.dirty_row_ranges = {{2, 1}};
    for (int row = 0; row < snapshot.grid_size.rows; ++row) {
        for (int column = 0; column < snapshot.grid_size.columns; ++column) {
            const QChar ch(ushort('A' + row));
            snapshot.cells.push_back({
                .position = {row, column},
                .text     = QString(ch),
            });
        }
    }

    const term::Terminal_render_snapshot lazy_snapshot =
        lazy_snapshot_from_flat(snapshot);
    const term::Terminal_render_frame frame = build(lazy_snapshot);
    ok &= check(frame.stats.cell_pass_input_cells == snapshot.grid_size.columns &&
            frame.stats.cells_considered == snapshot.grid_size.columns,
        "sparse lazy frame cell counters scale with dirty rows");
    ok &= check(frame.stats.row_descriptors_built == 1 &&
            frame.row_descriptors.size() == 1U &&
            frame.row_descriptors.front().row == 2,
        "sparse lazy frame builds row descriptors only for dirty/promoted rows");
    ok &= check(frame.text_runs.size() == 1U &&
            frame.text_runs.front().row == 2 &&
            frame.text_runs.front().text == QStringLiteral("CCCCC"),
        "sparse lazy frame emits dirty-row text through the canonical frame path");
    return ok;
}

bool test_sparse_lazy_frame_promotes_cursor_row()
{
    bool ok = true;

    term::Terminal_render_snapshot snapshot = empty_snapshot({4, 5});
    snapshot.dirty_row_ranges = {{2, 1}};
    snapshot.cursor.visible = true;
    snapshot.cursor.blink_enabled = false;
    snapshot.cursor.shape = term::Terminal_cursor_shape::BAR;
    snapshot.cursor.position = {1, 3};
    for (int row = 0; row < snapshot.grid_size.rows; ++row) {
        for (int column = 0; column < snapshot.grid_size.columns; ++column) {
            const QChar ch(ushort('A' + row));
            snapshot.cells.push_back({
                .position = {row, column},
                .text     = QString(ch),
            });
        }
    }

    const term::Terminal_render_snapshot lazy_snapshot =
        lazy_snapshot_from_flat(snapshot);
    const term::Terminal_render_frame frame = build(lazy_snapshot);
    ok &= check(frame.stats.row_descriptors_built == 2 &&
            frame.row_descriptors.size() == 2U &&
            frame.row_descriptors[0].row == 1 &&
            frame.row_descriptors[1].row == 2,
        "sparse lazy frame promotes a clean bar-cursor row beside dirty content");
    ok &= check(frame.stats.cell_pass_input_cells == snapshot.grid_size.columns * 2 &&
            frame.stats.cells_considered == snapshot.grid_size.columns * 2,
        "sparse lazy frame scans dirty rows plus promoted cursor rows");
    ok &= check(frame.text_runs.size() == 2U &&
            frame.text_runs[0].row == 1 &&
            frame.text_runs[1].row == 2,
        "sparse lazy frame populates promoted cursor row text for widened uploads");
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
    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_plain_text_color_inverse_and_wide_skip();
    ok &= test_terminal_graphics_use_grid_primitives();
    ok &= test_full_block_graphic_rects_coalesce_contiguous_cells();
    ok &= test_decorated_full_block_graphic_emits_decorations_without_text_run();
    ok &= test_stale_inline_bmp_category_uses_text_fallback();
    ok &= test_invalid_graphic_cells_stay_on_text_route();
    ok &= test_rounded_box_corners_use_arc_primitives();
    ok &= test_block_cursor_over_text_like_symbol_uses_cursor_text();
    ok &= test_terminal_graphics_use_geometry_with_selection_and_ime_text();
    ok &= test_mixed_unicode_row_geometry();
    ok &= test_single_cell_unicode_text_is_not_clipped_to_cell();
    ok &= test_coalesced_ascii_text_runs_coalesce_contiguous_plain_cells();
    ok &= test_coalesced_ascii_text_runs_split_at_guard_boundaries();
    ok &= test_coalesced_ascii_text_runs_split_at_max_coalesced_length();
    ok &= test_inverse_and_reverse_video_xor();
    ok &= test_default_and_reverse_full_grid_background();
    ok &= test_styled_blank_cells_emit_background_rects();
    ok &= test_selection_cursor_blink_and_shapes();
    ok &= test_decorations_preedit_hyperlink_and_bell();
    ok &= test_zero_grid_and_preedit_width();
    ok &= test_preedit_override_takes_precedence_over_snapshot();
    ok &= test_frame_stats_classify_rendered_cells();
    ok &= test_simple_content_classifier_and_stats();
    ok &= test_frame_stats_count_distinct_rows_and_styles_by_identity();
    ok &= test_classify_terminal_simple_content_cell_unicode_property();
    ok &= test_fragmented_dirty_ranges_drive_simple_content_membership();
    ok &= test_cell_pass_simple_content_classification();
    ok &= test_snapshot_cells_are_row_major_column_sorted();
    ok &= test_row_view_frame_matches_flat_materialization();
    ok &= test_descriptor_keys_are_mutation_sensitive();
    ok &= test_sparse_lazy_frame_scales_with_dirty_rows();
    ok &= test_sparse_lazy_frame_promotes_cursor_row();
    ok &= test_viewport_empty_and_dirty_ranges();
    return ok ? 0 : 1;
}
