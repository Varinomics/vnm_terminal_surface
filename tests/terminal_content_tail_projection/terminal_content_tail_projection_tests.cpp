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
#include <optional>
#include <string>
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
    const term::Terminal_screen_model& model, int offset, bool companion_presentation)
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
    request.companion_presentation           = companion_presentation;
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
        term::Terminal_viewport_state viewport;
        viewport.visible_rows    = sample.rows;
        viewport.scrollback_rows = sample.history;
        viewport.presentation    = term::make_presentation_mapping(
            term::Terminal_buffer_id::PRIMARY, true, sample.rows, sample.history, 0,
            sample.bottom, sample.prefix);
        ok &= check(term::presentation_max_offset_from_tail(viewport) == sample.history,
            "the Companion stream keeps the full native scroll extent");
        for (int offset = 0; offset <= sample.history; ++offset) {
            viewport.offset_from_tail = offset;
            viewport.follow_tail      = offset == 0;
            const int first = sample.history - offset;
            for (int row = 0; row < sample.rows; ++row) {
                const int expected = stream[static_cast<std::size_t>(first + row)];
                const int actual = term::presentation_requested_logical_row(viewport, row);
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
                ok &= check(term::presentation_display_row_for_logical_row(viewport, logical) == expected,
                    "inverse mapping preserves native row identity even outside the viewport");
            }
            ok &= check(term::presentation_requested_logical_row(viewport, -1) == -1 &&
                    term::presentation_requested_logical_row(viewport, sample.rows) == -1,
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
        auto request = request_for(model, offset, true);
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
        const auto display = term::make_presented_snapshot(native);
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
            const auto reference = model.render_snapshot(request_for(model, reference_offset, true));
            const int reference_row = logical - history + reference_offset;
            ok &= check(row_cells_match(*display, row, reference, reference_row) &&
                    display->visible_line_provenance[static_cast<std::size_t>(row)] ==
                        reference.visible_line_provenance[static_cast<std::size_t>(reference_row)] &&
                    images_match(term::render_snapshot_row_image(*display, row),
                        term::render_snapshot_row_image(reference, reference_row)),
                "display rows preserve native text, styles, provenance and exact image slices");
        }
        const int expected_cursor = term::presentation_display_row_for_logical_row(
            native->viewport, history + cursor_before.row).value_or(-1);
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
    const auto native = std::make_shared<const term::Terminal_render_snapshot>(
        resized.render_snapshot(request_for(resized, 0, true)));
    const auto display = term::make_presented_snapshot(native);
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
    const auto cleared = std::make_shared<const term::Terminal_render_snapshot>(
        model.render_snapshot(request_for(model, 0, true)));
    bool ok = check(cleared->resize_displaced_scrollback_rows == 0 &&
            cleared->native_content_bottom_row_exclusive == 1,
        "a model clear revokes the eligible resize prefix and retains the native cursor row");
    const auto cleared_display = term::make_presented_snapshot(cleared);
    ok &= check(cleared_display->display_row_logical_rows == std::vector<int>{-1, -1, -1, -1, -1, 3},
        "clear removes obsolete prefix content from the displayed tail");

    model.ingest(QByteArrayLiteral("\x1b[?1049halternate"));
    const auto alternate = std::make_shared<const term::Terminal_render_snapshot>(
        model.render_snapshot(request_for(model, 0, true)));
    ok &= check(term::make_presented_snapshot(alternate) == alternate,
        "alternate-screen presentation keeps the exact native snapshot");
    model.ingest(QByteArrayLiteral("\x1b[?1049l\x1b[6;1H"));
    const auto full = std::make_shared<const term::Terminal_render_snapshot>(
        model.render_snapshot(request_for(model, 0, true)));
    ok &= check(full->native_content_bottom_row_exclusive == 6 &&
            term::make_presented_snapshot(full) == full,
        "a visible native cursor on the last row leaves no tail to relocate");

    auto unavailable = std::make_shared<term::Terminal_render_snapshot>(*full);
    unavailable->native_content_bottom_row_exclusive = 0;
    ok &= check(term::make_presented_snapshot(unavailable) == unavailable,
        "a snapshot without native content extent retains its original presentation");
    return ok;
}

// The oracle for the remaining tests is the mapping record and producer of
// the batch-1 design (DESIGN.md 5.3, 5.7): the policy table, the requested
// positions q = S - P - o + d for AUTOMATIC, the policy maximum, and
// availability against the frame's own content basis.

term::Terminal_screen_model make_sized_model(int rows, int columns, int scrollback_limit = 32)
{
    term::Terminal_screen_model_config config;
    config.grid_size        = {rows, columns};
    config.scrollback_limit = scrollback_limit;
    config.cell_pixel_size  = term::terminal_cell_pixel_size_t{10, 20};
    return term::Terminal_screen_model(config);
}

std::vector<int> presented_logical_rows(const term::Terminal_render_snapshot& displayed)
{
    if (displayed.display_source) {
        return displayed.display_row_logical_rows;
    }
    std::vector<int> rows;
    const int first = term::render_snapshot_first_visible_logical_row(displayed);
    for (int row = 0; row < displayed.grid_size.rows; ++row) {
        rows.push_back(first + row);
    }
    return rows;
}

std::string describe_rows(const std::vector<int>& rows)
{
    std::string text = "[";
    for (std::size_t index = 0; index < rows.size(); ++index) {
        text += (index == 0 ? "" : ", ") + std::to_string(rows[index]);
    }
    return text + "]";
}

QString displayed_row_text(const term::Terminal_render_snapshot& snapshot, int row)
{
    const term::Terminal_render_snapshot_row_content_view rows(snapshot);
    return rows.row_at(row).text(0, snapshot.grid_size.columns, true).trimmed();
}

bool expect_presented(
    const std::shared_ptr<const term::Terminal_render_snapshot>& displayed,
    const std::vector<int>&                                      expected_rows,
    const std::vector<QString>&                                  expected_texts,
    const std::string&                                           step)
{
    if (!check(displayed != nullptr, "the producer presents the frame")) {
        std::cerr << step << ": no presented frame\n";
        return false;
    }
    bool ok = true;
    const auto actual = presented_logical_rows(*displayed);
    if (actual != expected_rows) {
        std::cerr << step << ": expected rows " << describe_rows(expected_rows)
                  << " actual " << describe_rows(actual) << '\n';
        ok = false;
    }
    for (int row = 0; row < static_cast<int>(expected_texts.size()); ++row) {
        const QString text = displayed_row_text(*displayed, row);
        if (text != expected_texts[static_cast<std::size_t>(row)]) {
            std::cerr << step << ": display row " << row << " expected \""
                      << expected_texts[static_cast<std::size_t>(row)].toStdString()
                      << "\" actual \"" << text.toStdString() << "\"\n";
            ok = false;
        }
    }
    return check(ok, "presented rows and their text follow the policy's requested positions");
}

std::vector<int> requested_rows(term::Terminal_viewport_state viewport, int offset)
{
    viewport.offset_from_tail = offset;
    viewport.follow_tail      = offset == 0;
    std::vector<int> rows;
    for (int row = 0; row < viewport.visible_rows; ++row) {
        rows.push_back(term::presentation_requested_logical_row(viewport, row));
    }
    return rows;
}

bool test_policy_inputs()
{
    using Policy = term::Terminal_presentation_policy;
    const auto primary = term::Terminal_buffer_id::PRIMARY;
    bool ok = true;

    const auto native = term::make_presentation_mapping(primary, false, 6, 10, 0, 2, 3);
    ok &= check(native.policy == Policy::NATIVE && native.hidden_rows == 0,
        "without vacancy or the Companion toggle the native mapping applies");

    const auto automatic = term::make_presentation_mapping(primary, false, 6, 10, 4, 2, 3);
    ok &= check(automatic.policy == Policy::AUTOMATIC && automatic.hidden_rows == 4,
        "resize vacancy selects the automatic mapping with P = V");

    const auto companion = term::make_presentation_mapping(primary, true, 6, 10, 0, 2, 3);
    ok &= check(companion.policy == Policy::COMPANION && companion.hidden_rows == 4 &&
            companion.insertion_row == 7,
        "the Companion toggle relocates the unwritten tail before S - K");

    const auto companion_vacancy = term::make_presentation_mapping(primary, true, 6, 10, 3, 6, 3);
    ok &= check(companion_vacancy.policy == Policy::COMPANION && companion_vacancy.hidden_rows == 3,
        "Companion relocates at least the resize vacancy when occupied cells reach the bottom");

    const auto alternate = term::make_presentation_mapping(
        term::Terminal_buffer_id::ALTERNATE, true, 6, 0, 4, 2, 0);
    ok &= check(alternate.policy == Policy::NATIVE && alternate.hidden_rows == 0,
        "the alternate screen always uses the native mapping");
    return ok;
}

bool test_automatic_requested_positions()
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows    = 6;
    viewport.scrollback_rows = 10;
    viewport.presentation    = term::make_presentation_mapping(
        term::Terminal_buffer_id::PRIMARY, false, 6, 10, 4, 2, 0);
    bool ok = check(term::presentation_max_offset_from_tail(viewport) == 6,
        "the automatic maximum is S - P");
    ok &= check(requested_rows(viewport, 0) == std::vector<int>({6, 7, 8, 9, 10, 11}),
        "the tail window ends at the last native row above the hidden vacancy");
    ok &= check(requested_rows(viewport, 6) == std::vector<int>({0, 1, 2, 3, 4, 5}),
        "the top bound starts at the first history row");
    ok &= check(requested_rows(viewport, 9) == std::vector<int>({0, 1, 2, 3, 4, 5}),
        "an offset beyond the policy maximum is clamped to it");

    viewport.offset_from_tail = 0;
    ok &= check(term::presentation_display_row_for_logical_row(viewport, 11) == 5 &&
            !term::presentation_display_row_for_logical_row(viewport, 12).has_value() &&
            !term::presentation_display_row_for_logical_row(viewport, 15).has_value(),
        "hidden vacancy rows have no display position");
    ok &= check(term::presentation_display_row_for_logical_row(viewport, 3) == -3,
        "rows above the window keep their relative display position");
    ok &= check(term::presentation_offset_for_logical_row(viewport, 3, 0) == 3 &&
            term::presentation_offset_for_logical_row(viewport, 3, 2) == 5 &&
            term::presentation_offset_for_logical_row(viewport, 11, 5) == 0 &&
            term::presentation_offset_for_logical_row(viewport, 0, 3) == 6,
        "the anchor offset shows a row at its display position and clamps to the bounds");
    ok &= check(term::presentation_leading_row_count(viewport) == 4,
        "the window acquires the hidden count of rows above the native view");

    term::Terminal_viewport_state short_history = viewport;
    short_history.scrollback_rows = 2;
    short_history.presentation    = term::make_presentation_mapping(
        term::Terminal_buffer_id::PRIMARY, false, 6, 2, 4, 2, 0);
    ok &= check(term::presentation_max_offset_from_tail(short_history) == 0,
        "too little history leaves no scroll range");
    ok &= check(requested_rows(short_history, 0) == std::vector<int>({-1, -1, 0, 1, 2, 3}),
        "structural unused space sits above the first native row");
    ok &= check(term::presentation_leading_row_count(short_history) == 2,
        "only native rows are acquired");
    return ok;
}

// Five output lines and a prompt at 2x20, so four rows are history, then a
// growth to six rows that leaves four vacant rows below the prompt.
term::Terminal_screen_model history_prompt_model()
{
    auto model = make_model(2);
    model.ingest(QByteArrayLiteral("l0\r\nl1\r\nl2\r\nl3\r\nl4\r\n$ "));
    model.resize({6, 20});
    return model;
}

bool test_automatic_model_presentation()
{
    auto model = history_prompt_model();
    bool ok = check(model.resize_vacancy_rows() == 4 && model.scrollback_size() == 4,
        "the fixture leaves four vacant rows below the prompt and four history rows");
    const auto native = std::make_shared<const term::Terminal_render_snapshot>(
        model.render_snapshot(request_for(model, 0, false)));
    ok &= check(native->viewport.presentation.policy == term::Terminal_presentation_policy::AUTOMATIC &&
            native->viewport.presentation.hidden_rows == 4 && native->resize_vacancy_rows == 4 &&
            native->leading_row_count == 4 && native->leading_rows_snapshot != nullptr,
        "the model publishes the mapping inputs and acquires the rows above its native view");
    const auto displayed = term::make_presented_snapshot(native);
    ok &= expect_presented(displayed, {0, 1, 2, 3, 4, 5},
        {QStringLiteral("l0"), QStringLiteral("l1"), QStringLiteral("l2"),
         QStringLiteral("l3"), QStringLiteral("l4"), QStringLiteral("$")},
        "automatic tail");
    if (displayed) {
        ok &= check(displayed->display_source == native &&
                term::validate_render_snapshot(*displayed).status == term::Terminal_render_snapshot_status::OK,
            "the presented frame keeps its native source and validates");
        ok &= check(displayed->cursor.visible && displayed->cursor.position.row == 5 &&
                displayed->cursor.position.column == 2,
            "the cursor follows its native row");
        ok &= check(displayed->visible_line_provenance.size() == 6U &&
                displayed->visible_line_provenance[0].logical_row == 0 &&
                displayed->visible_line_provenance[5].logical_row == 5,
            "presented rows carry their native identities");
    }
    ok &= check(term::presentation_max_offset_from_tail(native->viewport) == 0,
        "the written rows exactly fill the presented range");

    model.ingest(QByteArrayLiteral("x\r\n"));
    const auto consumed = std::make_shared<const term::Terminal_render_snapshot>(
        model.render_snapshot(request_for(model, 0, false)));
    ok &= expect_presented(term::make_presented_snapshot(consumed), {1, 2, 3, 4, 5, 6},
        {QStringLiteral("l1"), QStringLiteral("l2"), QStringLiteral("l3"),
         QStringLiteral("l4"), QStringLiteral("$ x"), QString()},
        "output consumes vacancy");

    // The mixed row of the join-split sequence stays visible.
    auto mixed = make_sized_model(2, 8);
    mixed.ingest(QByteArrayLiteral("a\r\nb"));
    mixed.resize({6, 8});
    mixed.ingest(QByteArrayLiteral("\x1b[3;1;6;8;7$r"));
    mixed.resize({6, 4});
    mixed.ingest(QByteArrayLiteral("\x1b[3;1HX\x1b[2;1H"));
    mixed.resize({6, 8});
    mixed.resize({6, 4});
    const auto mixed_native = std::make_shared<const term::Terminal_render_snapshot>(
        mixed.render_snapshot(request_for(mixed, 0, false)));
    ok &= expect_presented(term::make_presented_snapshot(mixed_native), {-1, -1, -1, 0, 1, 2},
        {QString(), QString(), QString(), QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("X")},
        "the mixed row after the second narrowing");
    return ok;
}

bool test_short_or_missing_history()
{
    bool ok = true;

    auto empty_history = make_sized_model(2, 8);
    empty_history.ingest(QByteArrayLiteral("a\r\nb"));
    empty_history.resize({6, 8});
    const auto empty_native = std::make_shared<const term::Terminal_render_snapshot>(
        empty_history.render_snapshot(request_for(empty_history, 0, false)));
    ok &= expect_presented(term::make_presented_snapshot(empty_native), {-1, -1, -1, -1, 0, 1},
        {QString(), QString(), QString(), QString(), QStringLiteral("a"), QStringLiteral("b")},
        "growth without history");
    ok &= check(term::presentation_max_offset_from_tail(empty_native->viewport) == 0,
        "growth without history has no scroll range");

    auto disabled = make_sized_model(2, 8, 0);
    disabled.ingest(QByteArrayLiteral("l0\r\nl1\r\nl2\r\n$ "));
    disabled.resize({6, 8});
    const auto disabled_native = std::make_shared<const term::Terminal_render_snapshot>(
        disabled.render_snapshot(request_for(disabled, 0, false)));
    ok &= expect_presented(term::make_presented_snapshot(disabled_native), {-1, -1, -1, -1, 0, 1},
        {QString(), QString(), QString(), QString(), QStringLiteral("l2"), QStringLiteral("$")},
        "history disabled");

    auto too_little = make_sized_model(2, 8);
    too_little.ingest(QByteArrayLiteral("l0\r\nl1\r\nl2\r\n$ "));
    too_little.resize({6, 8});
    const auto too_little_native = std::make_shared<const term::Terminal_render_snapshot>(
        too_little.render_snapshot(request_for(too_little, 0, false)));
    ok &= expect_presented(term::make_presented_snapshot(too_little_native), {-1, -1, 0, 1, 2, 3},
        {QString(), QString(), QStringLiteral("l0"), QStringLiteral("l1"),
         QStringLiteral("l2"), QStringLiteral("$")},
        "too little history");

    auto past_content = history_prompt_model();
    past_content.resize({12, 20});
    const auto past_native = std::make_shared<const term::Terminal_render_snapshot>(
        past_content.render_snapshot(request_for(past_content, 0, false)));
    ok &= expect_presented(term::make_presented_snapshot(past_native),
        {-1, -1, -1, -1, -1, -1, 0, 1, 2, 3, 4, 5},
        {QString(), QString(), QString(), QString(), QString(), QString(),
         QStringLiteral("l0"), QStringLiteral("l1"), QStringLiteral("l2"),
         QStringLiteral("l3"), QStringLiteral("l4"), QStringLiteral("$")},
        "growth past the content");

    auto cleared_history = history_prompt_model();
    cleared_history.ingest(QByteArrayLiteral("\x1b[3J"));
    const auto cleared_native = std::make_shared<const term::Terminal_render_snapshot>(
        cleared_history.render_snapshot(request_for(cleared_history, 0, false)));
    ok &= expect_presented(term::make_presented_snapshot(cleared_native), {-1, -1, -1, -1, 0, 1},
        {QString(), QString(), QString(), QString(), QStringLiteral("l4"), QStringLiteral("$")},
        "ED 3 alone after growth");
    return ok;
}

bool test_companion_padding_covers_vacancy()
{
    bool ok = true;
    for (const QByteArray& styling : {
            QByteArrayLiteral("\x1b[3;1;6;8;7$r"),
            QByteArrayLiteral("\x1b[7m\x1b[J\x1b[0m\x1b[K")})
    {
        auto model = make_sized_model(2, 8);
        model.ingest(QByteArrayLiteral("a\r\nb"));
        model.resize({6, 8});
        model.ingest(styling);
        const auto native = std::make_shared<const term::Terminal_render_snapshot>(
            model.render_snapshot(request_for(model, 0, true)));
        ok &= check(native->native_content_bottom_row_exclusive == 6 && native->resize_vacancy_rows == 4,
            "styled vacancy is occupied to the bottom row yet stays vacant");
        ok &= check(native->viewport.presentation.policy == term::Terminal_presentation_policy::COMPANION &&
                native->viewport.presentation.hidden_rows == 4,
            "Companion's padding covers the resize vacancy");
        ok &= expect_presented(term::make_presented_snapshot(native), {-1, -1, -1, -1, 0, 1},
            {QString(), QString(), QString(), QString(), QStringLiteral("a"), QStringLiteral("b")},
            "Companion with styled vacancy");
    }
    return ok;
}

void add_row_text(term::Terminal_render_snapshot& snapshot, int row, const QString& text)
{
    for (int column = 0; column < text.size(); ++column) {
        const QString character(text[column]);
        term::Terminal_render_cell cell;
        cell.position      = {row, column};
        cell.text          = term::Terminal_render_cell_text::from_source_cell(character, 1, false);
        cell.display_width = 1;
        cell.text_category = cell.text.category();
        snapshot.cells.push_back(std::move(cell));
    }
}

term::Terminal_render_snapshot frozen_frame(int scrollback_rows, int offset, int vacancy)
{
    term::Terminal_viewport_state viewport;
    viewport.visible_rows     = 6;
    viewport.scrollback_rows  = scrollback_rows;
    viewport.offset_from_tail = offset;
    viewport.follow_tail      = offset == 0;
    auto snapshot = term::make_empty_render_snapshot({6, 8}, viewport, 31U);
    snapshot.purpose                 = term::Terminal_render_snapshot_purpose::GEOMETRY_DERIVED;
    snapshot.frozen_basis            = true;
    snapshot.uncovered_trailing_rows = vacancy;
    snapshot.resize_vacancy_rows     = vacancy;
    snapshot.viewport.presentation = term::make_presentation_mapping(
        term::Terminal_buffer_id::PRIMARY, false, 6, scrollback_rows, vacancy, 6 - vacancy, 0);
    const int first = scrollback_rows - offset;
    for (int row = 0; row < 6 - vacancy; ++row) {
        add_row_text(snapshot, row, QStringLiteral("r%1").arg(first + row));
    }
    snapshot.cursor.visible = false;
    return snapshot;
}

bool test_availability()
{
    bool ok = true;

    // S=100, captured rows 100-101, held 2 -> 6 growth with no prefix.
    const auto worked = std::make_shared<const term::Terminal_render_snapshot>(frozen_frame(100, 0, 4));
    ok &= expect_presented(term::make_presented_snapshot(worked), {-1, -1, -1, -1, 100, 101},
        {QString(), QString(), QString(), QString(), QStringLiteral("r100"), QStringLiteral("r101")},
        "frozen growth without a captured prefix");
    ok &= check(term::presentation_max_offset_from_tail(worked->viewport) == 96 &&
            worked->viewport.scrollback_rows == 100,
        "unavailable frozen rows change neither the maximum nor native S");

    auto partial = frozen_frame(100, 0, 4);
    auto leading = frozen_frame(100, 2, 4);
    partial.leading_row_count     = 2;
    partial.leading_rows_snapshot = std::make_shared<const term::Terminal_render_snapshot>(leading);
    ok &= expect_presented(
        term::make_presented_snapshot(std::make_shared<const term::Terminal_render_snapshot>(partial)),
        {-1, -1, 98, 99, 100, 101},
        {QString(), QString(), QStringLiteral("r98"), QStringLiteral("r99"),
         QStringLiteral("r100"), QStringLiteral("r101")},
        "frozen growth with a partial captured prefix");

    auto live = frozen_frame(100, 0, 4);
    live.purpose      = term::Terminal_render_snapshot_purpose::CONTENT;
    live.frozen_basis = false;
    ok &= check(term::make_presented_snapshot(std::make_shared<const term::Terminal_render_snapshot>(live)) == nullptr,
        "a live frame missing requested rows is invalid and never falls back to native rows");

    term::Terminal_viewport_state held;
    held.visible_rows     = 6;
    held.scrollback_rows  = 100;
    held.offset_from_tail = 10;
    held.presentation     = term::make_presentation_mapping(
        term::Terminal_buffer_id::PRIMARY, false, 6, 100, 4, 2, 0);
    ok &= check(term::presentation_window_resolvable(held, 90, 6, true),
        "a frozen window missing only its leading rows resolves them as unused slots");
    ok &= check(!term::presentation_window_resolvable(held, 90, 6, false),
        "a live window missing leading rows does not resolve");
    ok &= check(!term::presentation_window_resolvable(held, 86, 2, true),
        "a frozen window missing rows below its coverage is refused");
    ok &= check(term::presentation_window_resolvable(held, 80, 20, false),
        "a window inside its coverage resolves");
    return ok;
}

// Availability is resolved before a mapping that presents the native rows as
// they are may hand back the native frame: a frame missing one of its own
// rows is not presentable under NATIVE or an identity Companion mapping.
bool test_identity_presentation_checks_coverage()
{
    bool ok = true;
    auto native = frozen_frame(100, 0, 0);
    ok &= check(native.viewport.presentation.policy == term::Terminal_presentation_policy::NATIVE &&
            term::make_presented_snapshot(std::make_shared<const term::Terminal_render_snapshot>(native)) != nullptr,
        "a complete native frame presents as it is");
    native.uncovered_trailing_rows = 1;
    ok &= check(term::make_presented_snapshot(std::make_shared<const term::Terminal_render_snapshot>(native)) == nullptr,
        "a native frame missing its last row is not presentable");
    native.frozen_basis = false;
    ok &= check(term::make_presented_snapshot(std::make_shared<const term::Terminal_render_snapshot>(native)) == nullptr,
        "a live native frame missing its last row is not presentable");

    auto companion = frozen_frame(100, 3, 0);
    companion.viewport.presentation = term::make_presentation_mapping(
        term::Terminal_buffer_id::PRIMARY, true, 6, 100, 0, 6, 0);
    ok &= check(companion.viewport.presentation.policy == term::Terminal_presentation_policy::COMPANION &&
            companion.viewport.presentation.hidden_rows == 0 &&
            term::make_presented_snapshot(std::make_shared<const term::Terminal_render_snapshot>(companion)) != nullptr,
        "a complete identity Companion frame presents as it is");
    companion.uncovered_trailing_rows = 1;
    ok &= check(term::make_presented_snapshot(std::make_shared<const term::Terminal_render_snapshot>(companion)) == nullptr,
        "an identity Companion frame missing its last row is not presentable");
    return ok;
}

std::shared_ptr<const term::Terminal_render_snapshot> presented_now(
    term::Terminal_screen_model& model,
    bool                         companion_presentation = false)
{
    return term::make_presented_snapshot(std::make_shared<const term::Terminal_render_snapshot>(
        model.render_snapshot(request_for(model, 0, companion_presentation))));
}

bool test_residual_absolute_rows_and_status_redraw()
{
    bool ok = true;

    // Absolute-row output after growth lands V rows lower on the display;
    // placement on the bottom row retires the vacancy and the view shifts.
    auto model = make_sized_model(2, 8);
    model.ingest(QByteArrayLiteral("a\r\nb"));
    model.resize({6, 8});
    model.ingest(QByteArrayLiteral("\x1b[1;1HTOP\x1b[2;2H"));
    ok &= expect_presented(presented_now(model), {-1, -1, -1, -1, 0, 1},
        {QString(), QString(), QString(), QString(), QStringLiteral("TOP"), QStringLiteral("b")},
        "absolute-row output above the vacancy");
    model.ingest(QByteArrayLiteral("\x1b[6;1HSTATUS\x1b[2;2H"));
    ok &= expect_presented(presented_now(model), {0, 1, 2, 3, 4, 5},
        {QStringLiteral("TOP"), QStringLiteral("b"), QString(), QString(), QString(), QStringLiteral("STATUS")},
        "placement on the bottom row retires the vacancy");

    // A bottom-status redraw: save the cursor, write the last row, restore.
    auto status = make_sized_model(2, 8);
    status.ingest(QByteArrayLiteral("a\r\n$ "));
    status.resize({6, 8});
    status.ingest(QByteArrayLiteral("\x1b" "7\x1b[6;1H[50%]\x1b" "8"));
    ok &= check(status.resize_vacancy_rows() == 0,
        "a status line drawn on the bottom row retires the vacancy above it");
    ok &= expect_presented(presented_now(status), {0, 1, 2, 3, 4, 5},
        {QStringLiteral("a"), QStringLiteral("$"), QString(), QString(), QString(), QStringLiteral("[50%]")},
        "the status redraw shows the blank rows inside the occupied extent");

    // The alternate screen is always native; leaving it restores the
    // primary's provenance and presentation.
    auto alternate = make_sized_model(2, 8);
    alternate.ingest(QByteArrayLiteral("a\r\nb"));
    alternate.resize({6, 8});
    alternate.ingest(QByteArrayLiteral("\x1b[?1049h\x1b[6;1Hfull"));
    const auto alternate_native = std::make_shared<const term::Terminal_render_snapshot>(
        alternate.render_snapshot(request_for(alternate, 0, false)));
    ok &= check(alternate_native->viewport.presentation.policy == term::Terminal_presentation_policy::NATIVE &&
            term::make_presented_snapshot(alternate_native) == alternate_native,
        "the alternate screen presents its native rows");
    alternate.ingest(QByteArrayLiteral("\x1b[?1049l"));
    ok &= expect_presented(presented_now(alternate), {-1, -1, -1, -1, 0, 1},
        {QString(), QString(), QString(), QString(), QStringLiteral("a"), QStringLiteral("b")},
        "leaving the alternate screen restores the primary presentation");
    return ok;
}

bool test_mapping_change_identity()
{
    auto model = history_prompt_model();
    const auto before = model.render_snapshot(request_for(model, 0, false));
    model.ingest(QByteArrayLiteral("x\r\n"));
    auto after = model.render_snapshot(request_for(model, 0, false));
    bool ok = check(term::viewport_mappings_match(before.viewport, after.viewport) &&
            term::grid_sizes_match(before.grid_size, after.grid_size) &&
            before.viewport.presentation != after.viewport.presentation,
        "output consuming vacancy changes the mapping record at the same native viewport");
    ok &= check(!term::snapshots_share_row_identity_space(before, after),
        "a mapping change is a different display identity space");
    after.dirty_row_ranges = {{2, 1}};
    const auto coalesced = term::snapshot_with_coalesced_dirty_rows(before, after);
    ok &= check(coalesced.dirty_row_ranges.size() == 1U &&
            coalesced.dirty_row_ranges.front().first_row == 0 &&
            coalesced.dirty_row_ranges.front().row_count == after.grid_size.rows,
        "dirty rows never coalesce across a mapping change");
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
    ok &= test_policy_inputs();
    ok &= test_automatic_requested_positions();
    ok &= test_automatic_model_presentation();
    ok &= test_short_or_missing_history();
    ok &= test_companion_padding_covers_vacancy();
    ok &= test_availability();
    ok &= test_identity_presentation_checks_coverage();
    ok &= test_residual_absolute_rows_and_status_redraw();
    ok &= test_mapping_change_identity();
    return ok ? 0 : 1;
}
