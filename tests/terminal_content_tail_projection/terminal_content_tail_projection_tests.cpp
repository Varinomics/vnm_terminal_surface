#include "vnm_terminal/internal/terminal_content_tail_projection.h"

#include "helpers/test_check.h"
#include "vnm_terminal/internal/terminal_screen_model.h"
#include "vnm_terminal/internal/viewport_contract.h"

#include <QByteArray>
#include <QString>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

// The oracle is the adopted stream permutation: older history, unwritten
// padding, resize-displaced history, then native content. Its length stays H+R.
std::vector<int> expected_display_stream(int rows, int history, int bottom, int prefix)
{
    std::vector<int> stream;
    for (int row = 0; row < history - prefix; ++row) {
        stream.push_back(row);
    }
    stream.insert(stream.end(), static_cast<std::size_t>(rows - bottom), -1);
    for (int row = history - prefix; row < history + bottom; ++row) {
        stream.push_back(row);
    }
    return stream;
}

term::Terminal_screen_model make_model(int rows)
{
    term::Terminal_screen_model_config config;
    config.grid_size        = {rows, 20};
    config.scrollback_limit = 32;
    config.cell_pixel_size  = term::terminal_cell_pixel_size_t{10, 20};
    return term::Terminal_screen_model(config);
}

term::Terminal_render_snapshot_request request_for(
    const term::Terminal_screen_model& model, int offset)
{
    term::Terminal_render_snapshot_request request;
    request.sequence                         = 157U;
    request.publication_generation           = 29U;
    request.processed_backend_callback_epoch = 83U;
    request.row_origin_generation            = 11U;
    request.viewport.active_buffer           = model.active_buffer_id();
    request.viewport.visible_rows            = model.grid_size().rows;
    request.viewport.scrollback_rows         = model.scrollback_size();
    request.viewport.offset_from_tail        = offset;
    request.viewport.follow_tail             = offset == 0;
    return request;
}

QString cell_text(const term::Terminal_render_cell& cell)
{
    QString text;
    cell.text.append_to(text);
    return text;
}

std::vector<const term::Terminal_render_cell*> row_cells(
    const term::Terminal_render_snapshot& snapshot, int row)
{
    std::vector<const term::Terminal_render_cell*> cells;
    for (const auto& cell : snapshot.cells) {
        if (cell.position.row == row) {
            cells.push_back(&cell);
        }
    }
    return cells;
}

bool row_cells_match(
    const term::Terminal_render_snapshot& left,  int left_row,
    const term::Terminal_render_snapshot& right, int right_row)
{
    const auto left_cells  = row_cells(left, left_row);
    const auto right_cells = row_cells(right, right_row);
    if (left_cells.size() != right_cells.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left_cells.size(); ++index) {
        const auto& a = *left_cells[index];
        const auto& b = *right_cells[index];
        if (a.position.column != b.position.column || cell_text(a) != cell_text(b) ||
            a.display_width != b.display_width || a.wide_continuation != b.wide_continuation ||
            a.hyperlink_id != b.hyperlink_id || a.text_category != b.text_category ||
            left.styles[a.style_id] != right.styles[b.style_id])
        {
            return false;
        }
    }
    return true;
}

bool images_match(
    const std::shared_ptr<const term::Terminal_image_slice>& left,
    const std::shared_ptr<const term::Terminal_image_slice>& right)
{
    if (!left || !right) {
        return left == right;
    }
    return left->revision == right->revision && left->first_column == right->first_column &&
        left->cell_pixel_size == right->cell_pixel_size && left->pixels == right->pixels;
}

bool native_snapshot_matches(
    const term::Terminal_render_snapshot& left,
    const term::Terminal_render_snapshot& right)
{
    bool matches = term::grid_sizes_match(left.grid_size, right.grid_size) &&
        term::viewport_mappings_match(left.viewport, right.viewport) &&
        left.viewport.follow_tail == right.viewport.follow_tail &&
        left.cursor.position == right.cursor.position &&
        left.cursor.visible == right.cursor.visible && left.cursor.shape == right.cursor.shape &&
        left.cursor.blink_enabled == right.cursor.blink_enabled && left.styles == right.styles &&
        left.visible_line_provenance == right.visible_line_provenance &&
        left.visible_row_images == right.visible_row_images &&
        left.metadata.sequence == right.metadata.sequence &&
        left.metadata.publication_generation == right.metadata.publication_generation &&
        left.metadata.processed_backend_callback_epoch == right.metadata.processed_backend_callback_epoch &&
        left.metadata.row_origin_generation == right.metadata.row_origin_generation &&
        left.native_content_bottom_row_exclusive == right.native_content_bottom_row_exclusive &&
        left.resize_displaced_scrollback_rows == right.resize_displaced_scrollback_rows &&
        left.leading_row_count == right.leading_row_count &&
        left.leading_rows_snapshot == right.leading_rows_snapshot &&
        left.touch_selection == right.touch_selection && left.display_source == right.display_source &&
        left.display_row_logical_rows == right.display_row_logical_rows;
    for (int row = 0; row < left.grid_size.rows; ++row) {
        matches = row_cells_match(left, row, right, row) && matches;
    }
    return matches;
}

bool test_stream_mapping_and_inverse()
{
    struct mapping_case_t {
        int rows;
        int history;
        int bottom;
        int prefix;
    };
    constexpr mapping_case_t cases[] = {
        {6, 0, 2, 0},
        {6, 3, 2, 3},
        {6, 9, 2, 8},
        {6, 9, 5, 2},
        {6, 9, 6, 4},
        {2, 4, 1, 4},
    };
    bool ok = true;
    for (const auto& sample : cases) {
        const auto stream = expected_display_stream(
            sample.rows, sample.history, sample.bottom, sample.prefix);
        ok &= check(stream.size() == static_cast<std::size_t>(sample.history + sample.rows),
            "relocating the unwritten tail preserves total scroll extent");
        term::Terminal_render_snapshot native;
        native.grid_size                           = {sample.rows, 20};
        native.viewport.visible_rows               = sample.rows;
        native.viewport.scrollback_rows            = sample.history;
        native.native_content_bottom_row_exclusive = sample.bottom;
        native.resize_displaced_scrollback_rows    = sample.prefix;
        for (int offset = 0; offset <= sample.history; ++offset) {
            native.viewport.offset_from_tail = offset;
            native.viewport.follow_tail      = offset == 0;
            const int first = sample.history - offset;
            for (int row = 0; row < sample.rows; ++row) {
                const int expected = stream[static_cast<std::size_t>(first + row)];
                const int actual = term::content_tail_display_row_to_logical_row(native, row);
                if (!check(actual == expected,
                        "every line-scrolled display row names the permuted native row or padding"))
                {
                    std::cerr << "R=" << sample.rows << " H=" << sample.history
                              << " B=" << sample.bottom << " K=" << sample.prefix
                              << " O=" << offset << " row=" << row
                              << " expected=" << expected << " actual=" << actual << '\n';
                    ok = false;
                }
            }
            for (int logical = 0; logical < sample.history + sample.bottom; ++logical) {
                const auto found = std::find(stream.begin(), stream.end(), logical);
                const int expected = static_cast<int>(found - stream.begin()) - first;
                ok &= check(term::content_tail_logical_row_to_display_row(native, logical) == expected,
                    "inverse mapping preserves native row identity even outside the viewport");
            }
            ok &= check(term::content_tail_display_row_to_logical_row(native, -1) == -1 &&
                    term::content_tail_display_row_to_logical_row(native, sample.rows) == -1,
                "rows outside the display grid have no selectable native row");
        }
    }
    return ok;
}

bool verify_model_projection(term::Terminal_screen_model& model, int history, int prefix, int bottom)
{
    const int rows = model.grid_size().rows;
    const auto stream = expected_display_stream(rows, history, bottom, prefix);
    const auto cursor_before = model.cursor_position();
    const QString text_before = model.visible_text();
    const term::Terminal_selection_range all_content{
        {0, 0}, {history + bottom - 1, 20}, term::Terminal_selection_mode::NORMAL};
    const auto selection_before = model.selected_text(all_content);
    bool ok = check(model.scrollback_size() == history && model.resize_displaced_scrollback_rows() == prefix,
        "the model fixture retains the expected native history and resize prefix");
    ok &= check(selection_before.code == term::Terminal_selection_result_code::OK,
        "native selection extraction covers the retained history and written content");

    term::Terminal_viewport_controller viewport;
    viewport.set_visible_rows(rows);
    viewport.set_scrollback_rows(history);
    const auto page = viewport.scroll_pages(1);
    ok &= check(page.applied_line_delta == std::min(rows, history) &&
            viewport.state().visible_rows == rows && viewport.max_offset_from_tail() == history,
        "display relocation leaves native page size and scroll limits intact");

    for (int offset = 0; offset <= history; ++offset) {
        auto request = request_for(model, offset);
        request.selections.push_back({all_content,
            model.selection_line_leases(term::Terminal_buffer_id::PRIMARY, all_content)});
        const auto native = std::make_shared<const term::Terminal_render_snapshot>(model.render_snapshot(request));
        const auto before = *native;
        if (!check(native->native_content_bottom_row_exclusive == bottom &&
                native->resize_displaced_scrollback_rows == prefix,
                "content bottom and raw eligible prefix do not depend on the viewed history offset"))
        {
            return false;
        }
        const auto display = term::make_content_tail_display_snapshot(native);
        if (!check(display && display->display_source == native &&
                display->display_row_logical_rows.size() == static_cast<std::size_t>(rows),
                "a display snapshot retains its exact native publication and row mapping"))
        {
            return false;
        }
        ok &= check(native_snapshot_matches(*native, before),
            "building display data leaves native cells, cursor, viewport, provenance and publication intact");
        ok &= check(term::validate_render_snapshot(*display).status == term::Terminal_render_snapshot_status::OK,
            "the derived display retains valid renderer data and row provenance");
        ok &= check(term::grid_sizes_match(display->grid_size, native->grid_size) &&
                term::viewport_mappings_match(display->viewport, native->viewport) &&
                display->metadata.sequence == native->metadata.sequence &&
                display->metadata.publication_generation == native->metadata.publication_generation &&
                display->metadata.row_origin_generation == native->metadata.row_origin_generation,
            "display coordinates keep the native grid, scroll extent and publication identity");

        for (int row = 0; row < rows; ++row) {
            const int logical = stream[static_cast<std::size_t>(history - offset + row)];
            ok &= check(display->display_row_logical_rows[static_cast<std::size_t>(row)] == logical,
                "actual model rows follow the same display-stream permutation");
            const auto span = std::find_if(display->selection_spans.begin(), display->selection_spans.end(),
                [row](const auto& candidate) { return candidate.row == row; });
            if (logical < 0) {
                const auto& provenance = display->visible_line_provenance[static_cast<std::size_t>(row)];
                ok &= check(row_cells(*display, row).empty() &&
                        !term::render_snapshot_row_image(*display, row) &&
                        provenance.retained_line_id == 0U && provenance.content_generation == 0U &&
                        span == display->selection_spans.end(),
                    "padding has no cells, image, selection highlight or retained row identity");
                continue;
            }
            ok &= check(span != display->selection_spans.end() &&
                    span->source_range == all_content && span->first_column == 0 && span->column_count == 20,
                "selection spans follow displayed rows while preserving their native range and columns");
            const int reference_offset = std::clamp(history - logical, 0, history);
            const auto reference = model.render_snapshot(request_for(model, reference_offset));
            const int reference_row = logical - history + reference_offset;
            ok &= check(row_cells_match(*display, row, reference, reference_row) &&
                    display->visible_line_provenance[static_cast<std::size_t>(row)] ==
                        reference.visible_line_provenance[static_cast<std::size_t>(reference_row)] &&
                    images_match(term::render_snapshot_row_image(*display, row),
                        term::render_snapshot_row_image(reference, reference_row)),
                "display rows preserve native text, styles, provenance and exact image slices");
        }
        const int expected_cursor = term::content_tail_logical_row_to_display_row(
            *native, history + cursor_before.row);
        ok &= check(display->cursor.position.row == expected_cursor &&
                display->cursor.position.column == native->cursor.position.column,
            "the displayed cursor follows its native row without changing its terminal column");
    }
    const auto selection_after = model.selected_text(all_content);
    ok &= check(model.cursor_position() == cursor_before && model.visible_text() == text_before &&
            model.grid_size().rows == rows && model.scrollback_size() == history &&
            model.resize_displaced_scrollback_rows() == prefix &&
            selection_after.code == selection_before.code && selection_after.text == selection_before.text,
        "display projection and browsing do not mutate native terminal content or selection extraction");
    return ok;
}

bool test_model_tail_and_resize_prefix()
{
    auto plain = make_model(6);
    plain.ingest(QByteArrayLiteral("alpha\r\nbeta"));
    bool ok = verify_model_projection(plain, 0, 0, 2);

    auto resized = make_model(6);
    resized.ingest(QByteArrayLiteral(
        "\x1b[32mheader alpha\x1b[0m\r\nsecond beta\r\nthird gamma\r\nfourth delta\r\nprompt> "));
    resized.resize({2, 20});
    resized.resize({6, 20});
    ok &= verify_model_projection(resized, 3, 3, 2);
    const auto native = std::make_shared<const term::Terminal_render_snapshot>(resized.render_snapshot(159U));
    const auto display = term::make_content_tail_display_snapshot(native);
    ok &= check(display->display_row_logical_rows == std::vector<int>{-1, 0, 1, 2, 3, 4},
        "a short restored prefix follows remaining top padding and ends at the written native tail");

    auto long_prefix = make_model(10);
    for (int row = 0; row < 14; ++row) {
        QByteArray line;
        if (row > 0) {
            line.append("\r\n");
        }
        line.append("line-");
        line.append(QByteArray::number(row));
        long_prefix.ingest(line);
    }
    long_prefix.resize({2, 20});
    long_prefix.resize({6, 20});
    ok &= verify_model_projection(long_prefix, 12, 8, 2);
    return ok;
}

bool test_image_content_bottom_and_projection()
{
    auto model = make_model(6);
    model.ingest(QByteArrayLiteral(
        "header\r\nsecond\r\nthird\r\nfourth\r\nprompt> "
        "\x1b[2;10H\x1bP9;1q#1;2;100;0;0#1!10~\x1b\\\x1b[5;8H"));
    model.resize({2, 20});
    model.resize({6, 20});
    bool ok = verify_model_projection(model, 3, 3, 2);
    model.ingest(QByteArrayLiteral(
        "\x1b[4;1H\x1bP9;1q#1;2;100;0;0#1!10~\x1b\\\x1b[1;1H\x1b[?25l"));
    const auto native = model.render_snapshot(163U);
    ok &= check(term::render_snapshot_row_image(native, 3) != nullptr &&
            native.native_content_bottom_row_exclusive == 4,
        "an image below text and a hidden cursor still contributes to the native content bottom");
    ok &= verify_model_projection(model, 3, 3, 4);
    return ok;
}

bool test_inactive_and_model_transitions()
{
    auto model = make_model(6);
    model.ingest(QByteArrayLiteral("one\r\ntwo\r\nthree\r\nfour\r\nfive"));
    model.resize({2, 20});
    model.resize({6, 20});
    model.ingest(QByteArrayLiteral("\x1b[2J\x1b[H"));
    const auto cleared = std::make_shared<const term::Terminal_render_snapshot>(model.render_snapshot(167U));
    bool ok = check(cleared->resize_displaced_scrollback_rows == 0 &&
            cleared->native_content_bottom_row_exclusive == 1,
        "a model clear revokes the eligible resize prefix and retains the native cursor row");
    const auto cleared_display = term::make_content_tail_display_snapshot(cleared);
    ok &= check(cleared_display->display_row_logical_rows == std::vector<int>{-1, -1, -1, -1, -1, 3},
        "clear removes obsolete prefix content from the displayed tail");

    model.ingest(QByteArrayLiteral("\x1b[?1049halternate"));
    const auto alternate = std::make_shared<const term::Terminal_render_snapshot>(model.render_snapshot(173U));
    ok &= check(term::make_content_tail_display_snapshot(alternate) == alternate,
        "alternate-screen presentation keeps the exact native snapshot");
    model.ingest(QByteArrayLiteral("\x1b[?1049l\x1b[6;1H"));
    const auto full = std::make_shared<const term::Terminal_render_snapshot>(model.render_snapshot(179U));
    ok &= check(full->native_content_bottom_row_exclusive == 6 &&
            term::make_content_tail_display_snapshot(full) == full,
        "a visible native cursor on the last row leaves no tail to relocate");

    auto unavailable = std::make_shared<term::Terminal_render_snapshot>(*full);
    unavailable->native_content_bottom_row_exclusive = 0;
    ok &= check(term::make_content_tail_display_snapshot(unavailable) == unavailable,
        "a snapshot without native content extent retains its original presentation");
    return ok;
}

} // namespace

int main()
{
    bool ok = true;
    ok &= test_stream_mapping_and_inverse();
    ok &= test_model_tail_and_resize_prefix();
    ok &= test_image_content_bottom_and_projection();
    ok &= test_inactive_and_model_transitions();
    return ok ? 0 : 1;
}
