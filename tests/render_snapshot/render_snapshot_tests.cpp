#include "helpers/test_check.h"
#include "vnm_terminal/internal/terminal_screen_model.h"
#include "vnm_terminal/internal/terminal_session.h"

#include <QByteArray>
#include <QDateTime>
#include <QSizeF>
#include <QString>
#include <QtGlobal>
#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

static_assert(std::is_constructible_v<
    term::Terminal_render_snapshot_row_content_view,
    const term::Terminal_render_snapshot&>);
static_assert(!std::is_constructible_v<
    term::Terminal_render_snapshot_row_content_view,
    term::Terminal_render_snapshot&&>);
static_assert(!std::is_constructible_v<
    term::Terminal_render_snapshot_row_content_view,
    const term::Terminal_render_snapshot&&>);

term::Terminal_screen_model make_model(term::terminal_grid_size_t grid_size)
{
    return term::Terminal_screen_model({grid_size, 16, 8});
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

term::Terminal_viewport_state tail_viewport(
    const term::Terminal_screen_model& model,
    int                                offset_from_tail = 0)
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
    return viewport;
}

term::Terminal_render_snapshot_request request_for_model(
    const term::Terminal_screen_model& model,
    std::uint64_t                      sequence,
    int                                offset_from_tail = 0)
{
    term::Terminal_render_snapshot_request request;
    request.sequence = sequence;
    request.viewport = tail_viewport(model, offset_from_tail);
    return request;
}

QString row_text(const term::Terminal_render_snapshot& snapshot, int row)
{
    QString text;
    for (const term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row == row && !cell.wide_continuation) {
            cell.text.append_to(text);
        }
    }
    return text;
}

std::vector<const term::Terminal_render_cell*> flat_cells_by_position_for_fixture(
    const term::Terminal_render_snapshot& snapshot)
{
    std::vector<const term::Terminal_render_cell*> cells_by_position(
        static_cast<std::size_t>(snapshot.grid_size.rows) *
            static_cast<std::size_t>(snapshot.grid_size.columns),
        nullptr);
    for (const term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.position.row    <  0                          ||
            cell.position.row    >= snapshot.grid_size.rows     ||
            cell.position.column <  0                          ||
            cell.position.column >= snapshot.grid_size.columns)
        {
            continue;
        }

        const std::size_t index =
            static_cast<std::size_t>(cell.position.row) *
                static_cast<std::size_t>(snapshot.grid_size.columns) +
            static_cast<std::size_t>(cell.position.column);
        cells_by_position[index] = &cell;
    }
    return cells_by_position;
}

QString selected_text_from_flat_fixture_row(
    const std::vector<const term::Terminal_render_cell*>& cells_by_position,
    term::terminal_grid_size_t                            grid_size,
    int                                                   viewport_row,
    int                                                   first_column,
    int                                                   end_column,
    bool                                                  trim_trailing_spaces)
{
    QString text;
    for (int column = first_column; column < end_column; ++column) {
        const std::size_t index =
            static_cast<std::size_t>(viewport_row) *
                static_cast<std::size_t>(grid_size.columns) +
            static_cast<std::size_t>(column);
        const term::Terminal_render_cell* cell = cells_by_position[index];
        if (cell == nullptr) {
            text += QLatin1Char(' ');
            continue;
        }

        if (cell->wide_continuation) {
            continue;
        }

        cell->text.append_to(text);
    }

    if (trim_trailing_spaces) {
        while (text.endsWith(QLatin1Char(' '))) {
            text.chop(1);
        }
    }
    return text;
}

term::Terminal_selection_result selected_text_from_flat_snapshot_baseline(
    const term::Terminal_render_snapshot& snapshot,
    const term::Terminal_selection_range& selection)
{
    if (snapshot.grid_size.rows <= 0 || snapshot.grid_size.columns <= 0 ||
        selection.mode == term::Terminal_selection_mode::NONE)
    {
        return {term::Terminal_selection_result_code::INVALID_RANGE, {}};
    }

    const term::terminal_grid_position_t start =
        term::normalized_selection_start(selection);
    const term::terminal_grid_position_t end =
        term::normalized_selection_end(selection);
    const int first_visible_logical_row =
        term::render_snapshot_first_visible_logical_row(snapshot);

    if (start.row    < first_visible_logical_row                            ||
        end.row      >= first_visible_logical_row + snapshot.grid_size.rows ||
        start.column < 0                                                    ||
        start.column > snapshot.grid_size.columns                           ||
        end.column   < 0                                                    ||
        end.column   > snapshot.grid_size.columns)
    {
        return {term::Terminal_selection_result_code::INVALID_RANGE, {}};
    }

    const std::vector<const term::Terminal_render_cell*> cells_by_position =
        flat_cells_by_position_for_fixture(snapshot);
    QString text;
    for (int logical_row = start.row; logical_row <= end.row; ++logical_row) {
        const int first_column = logical_row == start.row ? start.column : 0;
        const int end_column =
            logical_row == end.row ? end.column : snapshot.grid_size.columns;
        if (end_column < first_column) {
            return {term::Terminal_selection_result_code::INVALID_RANGE, {}};
        }

        if (logical_row > start.row) {
            text += QLatin1Char('\n');
        }

        text += selected_text_from_flat_fixture_row(
            cells_by_position,
            snapshot.grid_size,
            logical_row - first_visible_logical_row,
            first_column,
            end_column,
            end_column == snapshot.grid_size.columns);
    }

    return {term::Terminal_selection_result_code::OK, text};
}

const term::Terminal_render_cell* cell_with_text(
    const term::Terminal_render_snapshot&  snapshot,
    QString                                text)
{
    for (const term::Terminal_render_cell& cell : snapshot.cells) {
        if (cell.text == text) {
            return &cell;
        }
    }

    return nullptr;
}

const term::Terminal_render_cell* cell_at(
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

// Mirrors the row-major/column-ascending contract that validate_render_snapshot
// enforces (INVALID_CELL_ORDER): every cell after the first comes strictly after
// the previous one in row-major order, i.e. its row is greater, or the row is
// equal and the column is strictly greater.
bool snapshot_cells_are_row_major_column_ascending(
    const term::Terminal_render_snapshot& snapshot)
{
    for (std::size_t index = 1U; index < snapshot.cells.size(); ++index) {
        const term::Terminal_render_cell& previous = snapshot.cells[index - 1U];
        const term::Terminal_render_cell& current  = snapshot.cells[index];
        const bool strictly_after =
            current.position.row > previous.position.row ||
            (current.position.row    == previous.position.row &&
             current.position.column >  previous.position.column);
        if (!strictly_after) {
            return false;
        }
    }

    return true;
}

const term::Terminal_render_hyperlink_metadata* hyperlink_by_id(
    const term::Terminal_render_snapshot&  snapshot,
    std::uint64_t                          hyperlink_id)
{
    for (const term::Terminal_render_hyperlink_metadata& hyperlink : snapshot.hyperlinks) {
        if (hyperlink.hyperlink_id == hyperlink_id) {
            return &hyperlink;
        }
    }

    return nullptr;
}

term::Terminal_render_cell render_cell(
    int                         row,
    int                         column,
    QString                     text,
    term::Terminal_style_id     style_id = term::k_default_terminal_style_id,
    std::uint64_t               hyperlink_id = 0U)
{
    term::Terminal_render_cell cell;
    cell.position      = {row, column};
    cell.text          = term::Terminal_render_cell_text::from_source_cell(text, 1, false);
    cell.hyperlink_id  = hyperlink_id;
    cell.display_width = 1;
    cell.style_id      = style_id;
    cell.text_category = cell.text.category();
    return cell;
}

bool render_cells_match(
    const std::vector<term::Terminal_render_cell>& left,
    const std::vector<term::Terminal_render_cell>& right)
{
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t i = 0U; i < left.size(); ++i) {
        if (!term::render_snapshot_cells_equal(left[i], right[i])) {
            return false;
        }
    }
    return true;
}

bool check_visible_line_provenance_matches_model(
    const term::Terminal_screen_model&     model,
    const term::Terminal_render_snapshot&  snapshot,
    const char*                            label)
{
    bool ok = true;
    ok &= check(snapshot.visible_line_provenance.size() ==
        static_cast<std::size_t>(snapshot.grid_size.rows),
        label);
    if (snapshot.visible_line_provenance.size() !=
        static_cast<std::size_t>(snapshot.grid_size.rows))
    {
        return false;
    }

    const int first_visible_logical_row =
        term::render_snapshot_first_visible_logical_row(snapshot);
    for (int row = 0; row < snapshot.grid_size.rows; ++row) {
        const int logical_row = first_visible_logical_row + row;
        const term::Terminal_retained_line_provenance expected =
            model.retained_line_provenance_for_testing(
                snapshot.viewport.active_buffer,
                logical_row);
        const term::Terminal_render_line_provenance& actual =
            snapshot.visible_line_provenance[static_cast<std::size_t>(row)];
        ok &= check(actual.logical_row == static_cast<std::int64_t>(logical_row), label);
        ok &= check(actual.retained_line_id == expected.retained_line_id, label);
        ok &= check(actual.content_generation == expected.content_generation, label);
        ok &= check(actual.source == expected.source, label);
    }
    return ok;
}

term::Terminal_launch_config launch_config(term::terminal_grid_size_t grid_size)
{
    term::Terminal_launch_config config;
    config.argv              = {QStringLiteral("snapshot-test-backend")};
    config.working_directory = QStringLiteral("C:/workspace");
    config.initial_grid_size = grid_size;
    return config;
}

class Recording_backend final : public term::Terminal_backend
{
public:
    term::Terminal_backend_result start(
        const term::Terminal_launch_config&    config,
        term::Terminal_backend_callbacks       callbacks) override
    {
        const term::Terminal_backend_result callback_result =
            term::validate_backend_callbacks(callbacks);
        if (term::is_backend_rejection(callback_result)) {
            return callback_result;
        }

        const term::Terminal_backend_result config_result =
            term::validate_launch_config(config);
        if (term::is_backend_rejection(config_result)) {
            return config_result;
        }

        backend_callbacks = std::move(callbacks);
        running = true;
        return term::backend_accept();
    }

    term::Terminal_backend_result write(QByteArray bytes) override
    {
        writes.push_back(std::move(bytes));
        return term::backend_accept();
    }

    term::Terminal_backend_result resize(term::Terminal_backend_resize_request request) override
    {
        resize_requests.push_back(request);
        if (reject_next_resize) {
            reject_next_resize = false;
            return
                term::backend_reject(
                    term::Terminal_backend_error_code::RESIZE_FAILED,
                    QStringLiteral("recording backend rejected resize"));
        }

        return term::backend_accept();
    }

    term::Terminal_backend_result set_output_paused(bool paused) override
    {
        pause_requests.push_back(paused);
        return term::backend_accept();
    }

    term::Terminal_backend_result interrupt() override
    {
        running = false;
        return term::backend_accept();
    }

    term::Terminal_backend_result terminate() override
    {
        running = false;
        return term::backend_accept();
    }

    bool emit_output(QByteArray bytes)
    {
        if (!backend_callbacks.output_received) {
            return false;
        }

        backend_callbacks.output_received(std::move(bytes));
        return true;
    }

    bool                       running            = false;
    bool                       reject_next_resize = false;
    term::Terminal_backend_callbacks
                               backend_callbacks;
    std::vector<term::Terminal_backend_resize_request>
                               resize_requests;
    std::vector<QByteArray>    writes;
    std::vector<bool>          pause_requests;
};

std::unique_ptr<term::Terminal_session> make_session(Recording_backend*& backend)
{
    auto owned_backend = std::make_unique<Recording_backend>();
    backend = owned_backend.get();

    term::Terminal_session_config config;
    config.trace_notification_limit = 64U;
    config.trace_resize_limit       = 64U;

    return std::make_unique<term::Terminal_session>(
        std::move(owned_backend),
        config);
}

bool test_owned_styled_wide_hyperlink_scrollback_snapshot()
{
    bool ok = true;
    term::Terminal_screen_model model = make_model({2, 16});

    QByteArray payload =
        QByteArrayLiteral("\x1b]8;id=main;https://example.test\x1b\\")
        + QByteArrayLiteral("\x1b[31mLINK")
        + QStringLiteral("\u754c").toUtf8()
        + QByteArrayLiteral("\x1b[0m\x1b]8;;\x1b\\\r\nNEXT\r\nTAIL");
    model.ingest(payload);

    term::Terminal_render_snapshot_request request = request_for_model(model, 10U, 1);
    request.viewport_changed = true;
    const term::Terminal_render_snapshot snapshot = model.render_snapshot(request);

    ok &= check(snapshot.basis == term::Terminal_render_snapshot_basis::LIVE_CONTENT &&
        snapshot.purpose == term::Terminal_render_snapshot_purpose::CONTENT,
        "styled scrollback snapshot records live content basis and content purpose");
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "styled scrollback snapshot validates");
    ok &= check(row_text(snapshot, 0).contains(QStringLiteral("LINK")),
        "viewport snapshot includes scrollback row text");

    const term::Terminal_render_cell* link_cell =
        cell_with_text(snapshot, QStringLiteral("L"));
    ok &= check(link_cell != nullptr && link_cell->hyperlink_id != 0U,
        "scrollback cell retains hyperlink id");
    if (link_cell != nullptr) {
        const term::Terminal_render_hyperlink_metadata* hyperlink =
            hyperlink_by_id(snapshot, link_cell->hyperlink_id);
        ok &= check(hyperlink != nullptr &&
            hyperlink->uri == QByteArrayLiteral("https://example.test"),
            "snapshot owns hyperlink metadata");
        ok &= check(snapshot.styles[link_cell->style_id].foreground.kind ==
            term::Terminal_color_ref_kind::PALETTE_INDEX,
            "scrollback cell retains style id");
    }

    const term::Terminal_render_cell* wide_cell =
        cell_with_text(snapshot, QStringLiteral("\u754c"));
    ok &= check(wide_cell != nullptr && wide_cell->display_width == 2,
        "wide cell is copied into snapshot");

    model.ingest(QByteArrayLiteral("\x1b[2Jmutated"));
    ok &= check(row_text(snapshot, 0).contains(QStringLiteral("LINK")),
        "old snapshot remains immutable after model mutation");

    term::Terminal_render_snapshot_request retained_request =
        request_for_model(model, 11U, model.scrollback_size());
    const term::Terminal_render_snapshot retained_snapshot =
        model.render_snapshot(retained_request);
    const term::Terminal_render_cell* retained_link_cell =
        cell_with_text(retained_snapshot, QStringLiteral("L"));
    ok &= check(retained_link_cell != nullptr &&
        term::validate_render_snapshot(retained_snapshot).status ==
            term::Terminal_render_snapshot_status::OK,
        "scrollback hyperlink snapshot validates after active model mutation");
    if (retained_link_cell != nullptr) {
        const term::Terminal_render_hyperlink_metadata* retained_hyperlink =
            hyperlink_by_id(retained_snapshot, retained_link_cell->hyperlink_id);
        ok &= check(retained_hyperlink != nullptr &&
            retained_hyperlink->uri == QByteArrayLiteral("https://example.test"),
            "scrollback hyperlink metadata resolves from retained row after active model mutation");
    }
    return ok;
}

bool test_scrollback_wide_rows_are_repaired_on_resize()
{
    bool ok = true;
    term::Terminal_screen_model model = make_model({2, 6});
    model.ingest(
        QByteArrayLiteral("ABCD") +
        QStringLiteral("\u754c").toUtf8() +
        QByteArrayLiteral("\r\nNEXT\r\nTAIL"));

    ok &= check(model.scrollback_size() > 0, "wide-cell row reached scrollback");
    model.resize({2, 5});

    const term::Terminal_render_snapshot snapshot =
        model.render_snapshot(request_for_model(model, 40U, model.scrollback_size()));
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "scrollback rows are repaired after narrowing resize");
    return ok;
}

bool test_snapshot_rows_cover_primary_retained_and_alternate_sources()
{
    bool ok = true;
    term::Terminal_screen_model model = make_model({2, 8});
#if VNM_TERMINAL_PROFILING_ENABLED
    model.set_profile_stats_enabled(true);
#endif
    model.ingest(QByteArrayLiteral("ONE\r\nTWO\r\nTHREE"));

    ok &= check(model.scrollback_size() == 1, "row-source fixture creates one retained row");

    const term::Terminal_render_snapshot primary_snapshot =
        model.render_snapshot(request_for_model(model, 12U));
    ok &= check(row_text(primary_snapshot, 0) == QStringLiteral("TWO") &&
        row_text(primary_snapshot, 1) == QStringLiteral("THREE"),
        "tail snapshot uses resident primary active rows");
    ok &= check_visible_line_provenance_matches_model(
        model,
        primary_snapshot,
        "tail snapshot line provenance matches resident primary rows");
    ok &= check(term::validate_render_snapshot(primary_snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "row-source primary snapshot validates");

    const term::Terminal_render_snapshot retained_snapshot =
        model.render_snapshot(request_for_model(model, 13U, 1));
    ok &= check(row_text(retained_snapshot, 0) == QStringLiteral("ONE") &&
        row_text(retained_snapshot, 1) == QStringLiteral("TWO"),
        "scrolled snapshot uses retained history and resident primary rows");
    ok &= check_visible_line_provenance_matches_model(
        model,
        retained_snapshot,
        "scrolled snapshot line provenance matches retained and resident rows");
    ok &= check(term::validate_render_snapshot(retained_snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "row-source retained snapshot validates");

    model.ingest(QByteArrayLiteral("\x1b[?1049hALT"));
    const term::Terminal_render_snapshot alternate_snapshot =
        model.render_snapshot(request_for_model(model, 14U));
    ok &= check(alternate_snapshot.viewport.active_buffer ==
        term::Terminal_buffer_id::ALTERNATE,
        "alternate row-source snapshot reports alternate buffer");
    ok &= check(row_text(alternate_snapshot, 0) == QStringLiteral("ALT") &&
        row_text(alternate_snapshot, 1).isEmpty(),
        "alternate snapshot uses resident alternate rows");
    ok &= check_visible_line_provenance_matches_model(
        model,
        alternate_snapshot,
        "alternate snapshot line provenance matches resident alternate rows");
    ok &= check(term::validate_render_snapshot(alternate_snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "row-source alternate snapshot validates");

#if VNM_TERMINAL_PROFILING_ENABLED
    const term::Terminal_screen_model_profile_stats stats = model.profile_stats();
    ok &= check(stats.render_snapshots_constructed == 3U &&
        stats.render_snapshot_rows_visited == 6U &&
        stats.render_snapshot_rows_materialized == 6U,
        "row-source snapshots preserve historical visited/materialized profile counts");
    ok &= check(stats.render_snapshot_rows_borrowed == 5U &&
        stats.render_snapshot_rows_owned == 1U,
        "row-source snapshots split borrowed and owned row-cell sources");
    ok &= check(stats.render_snapshot_rows_built_from_model_storage == 6U &&
        stats.render_snapshot_model_row_accessor_borrows == 5U,
        "row-source snapshots expose explicit Batch 1 row-source counters");
    ok &= check(stats.render_snapshot_compact_ascii_text_cells == 17U &&
        stats.render_snapshot_compact_empty_text_cells == 0U &&
        stats.render_snapshot_fallback_qstring_copies == 0U,
        "row-source snapshots count compact printable ASCII cells without QString copies");
    ok &= check(stats.render_snapshot_fallback_text_code_units_copied == 0U &&
        stats.render_snapshot_fallback_printable_ascii_copies == 0U &&
        stats.render_snapshot_fallback_other_ascii_copies == 0U &&
        stats.render_snapshot_fallback_single_non_ascii_copies == 0U &&
        stats.render_snapshot_fallback_multi_text_copies == 0U,
        "row-source snapshots keep fallback copy categories empty for compact ASCII");
    ok &= check(stats.render_snapshot_unoccupied_cells_skipped == 31U &&
        stats.max_render_snapshot_fallback_text_units_per_cell == 0U,
        "row-source snapshots expose skipped cells and fallback copied text width");
#endif

    return ok;
}

bool test_snapshot_cells_cache_text_category()
{
    bool ok = true;
    term::Terminal_screen_model model = make_model({2, 8});
    model.ingest(
        QByteArrayLiteral("AZ") +
        QStringLiteral("\u754c").toUtf8());

    const term::Terminal_render_snapshot snapshot =
        model.render_snapshot(request_for_model(model, 18U));
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "text-category snapshot fixture validates");

    const term::Terminal_render_cell* ascii_cell = cell_at(snapshot, 0, 0);
    ok &= check(ascii_cell != nullptr &&
        ascii_cell->text_category ==
            term::Terminal_render_cell_text_category::PRINTABLE_ASCII,
        "printable ASCII snapshot cell carries cached category");
    ok &= check(ascii_cell != nullptr &&
        ascii_cell->text.storage() ==
            term::Terminal_render_cell_text_storage::INLINE_PRINTABLE_ASCII,
        "printable ASCII snapshot cell uses compact inline storage");
    ok &= check(ascii_cell != nullptr &&
        ascii_cell->text.fallback_qstring_or_null() == nullptr &&
        ascii_cell->text.single_printable_ascii_code_unit().has_value() &&
        ascii_cell->text.single_printable_ascii_code_unit().value() ==
            static_cast<ushort>('A'),
        "printable ASCII snapshot cell stores an inline code unit without fallback QString");

    const term::Terminal_render_cell* non_ascii_cell =
        cell_with_text(snapshot, QStringLiteral("\u754c"));
    ok &= check(non_ascii_cell != nullptr &&
        non_ascii_cell->text_category ==
            term::Terminal_render_cell_text_category::NON_ASCII,
        "non-ASCII snapshot cell carries cached category");
    ok &= check(non_ascii_cell != nullptr &&
        non_ascii_cell->text.storage() ==
            term::Terminal_render_cell_text_storage::INLINE_SINGLE_BMP,
        "non-ASCII snapshot cell uses inline BMP storage");
    ok &= check(non_ascii_cell != nullptr &&
        non_ascii_cell->text.fallback_qstring_or_null() == nullptr &&
        non_ascii_cell->text.single_bmp_code_unit().has_value() &&
        non_ascii_cell->text.single_bmp_code_unit().value() == 0x754cU,
        "non-ASCII snapshot cell stores an inline BMP code unit without fallback QString");

    const term::Terminal_render_cell* continuation_cell = non_ascii_cell != nullptr
        ? cell_at(snapshot, non_ascii_cell->position.row, non_ascii_cell->position.column + 1)
        : nullptr;
    ok &= check(continuation_cell != nullptr &&
        continuation_cell->wide_continuation &&
        continuation_cell->text_category ==
            term::Terminal_render_cell_text_category::EMPTY,
        "wide continuation snapshot cell carries empty cached category");
    ok &= check(continuation_cell != nullptr &&
        continuation_cell->text.storage() ==
            term::Terminal_render_cell_text_storage::EMPTY,
        "wide continuation snapshot cell uses empty compact storage");

    term::Terminal_render_snapshot unknown_category_snapshot = snapshot;
    bool unknown_category_written = false;
    for (term::Terminal_render_cell& cell : unknown_category_snapshot.cells) {
        if (cell.position.row == 0 && cell.position.column == 0) {
            cell.text_category = term::Terminal_render_cell_text_category::UNKNOWN;
            unknown_category_written = true;
            break;
        }
    }
    ok &= check(unknown_category_written &&
        term::validate_render_snapshot(unknown_category_snapshot).status ==
            term::Terminal_render_snapshot_status::OK,
        "unknown text category remains valid for fallback classification");

    term::Terminal_render_snapshot stale_category_snapshot = snapshot;
    bool stale_category_written = false;
    for (term::Terminal_render_cell& cell : stale_category_snapshot.cells) {
        if (non_ascii_cell != nullptr &&
            cell.position.row    == non_ascii_cell->position.row &&
            cell.position.column == non_ascii_cell->position.column)
        {
            cell.text_category = term::Terminal_render_cell_text_category::PRINTABLE_ASCII;
            stale_category_written = true;
            break;
        }
    }
    ok &= check(stale_category_written &&
        term::validate_render_snapshot(stale_category_snapshot).status ==
            term::Terminal_render_snapshot_status::INVALID_CELL_TEXT_CATEGORY,
        "snapshot validation rejects stale cached text category");

    return ok;
}

bool test_snapshot_profile_counts_inline_single_bmp_cells()
{
    bool ok = true;

#if VNM_TERMINAL_PROFILING_ENABLED
    term::Terminal_screen_model model = make_model({1, 8});
    model.set_profile_stats_enabled(true);
    model.ingest(QString::fromUcs4(U"\u2500\u2588\u754c\U0001F600").toUtf8());

    const term::Terminal_render_snapshot snapshot =
        model.render_snapshot(request_for_model(model, 19U));
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "inline-BMP profile snapshot validates");

    const term::Terminal_screen_model_profile_stats stats = model.profile_stats();
    ok &= check(stats.render_snapshot_inline_single_bmp_text_cells == 3U,
        "snapshot profile counts box, block, and CJK leading cells as inline BMP");
    ok &= check(stats.render_snapshot_fallback_qstring_copies == 1U &&
        stats.render_snapshot_fallback_single_non_ascii_copies == 1U,
        "snapshot profile leaves emoji on the fallback single-scalar path");
    ok &= check(stats.render_snapshot_compact_empty_text_cells == 2U &&
        stats.render_snapshot_fallback_text_code_units_copied == 2U &&
        stats.max_render_snapshot_fallback_text_units_per_cell == 2U,
        "snapshot profile separates wide continuations and emoji UTF-16 copies");
#endif

    return ok;
}

bool test_alternate_screen_hides_primary_scrollback()
{
    bool ok = true;
    term::Terminal_screen_model model = make_model({2, 10});
    model.ingest(QByteArrayLiteral("PRIMARY\r\nSCROLL\r\nTAIL"));
    ok &= check(model.scrollback_size() > 0, "primary scrollback exists before alternate");

    model.ingest(QByteArrayLiteral("\x1b[?1049hALT"));
    const term::Terminal_render_snapshot snapshot =
        model.render_snapshot(request_for_model(model, 20U));

    ok &= check(snapshot.viewport.active_buffer == term::Terminal_buffer_id::ALTERNATE,
        "alternate snapshot reports alternate buffer");
    ok &= check(snapshot.viewport.scrollback_rows == 0, "alternate snapshot has no scrollback");
    ok &= check(!row_text(snapshot, 0).contains(QStringLiteral("PRIMARY")) &&
        !row_text(snapshot, 1).contains(QStringLiteral("PRIMARY")),
        "alternate snapshot does not expose primary scrollback text");
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "alternate snapshot validates");
    return ok;
}

bool test_request_metadata_damage_selection_and_ime()
{
    bool ok = true;
    term::Terminal_screen_model model = make_model({6, 12});
    model.ingest(QByteArrayLiteral("abcdef"));

    term::Terminal_render_snapshot_request request = request_for_model(model, 30U);
    request.dirty_rows                       = {5, 1, 2, 2, 3};
    request.processed_backend_callback_epoch = 17U;
    request.cursor_shape                     = term::Terminal_cursor_shape::BAR;
    request.cursor_blink_enabled             = false;
    request.backend_geometry_in_sync         = false;
    request.visual_bell_active               = true;
    request.ime_preedit.text                 = QStringLiteral("ime");
    request.ime_preedit.cursor_position      = 2;
    request.ime_preedit.active               = true;
    const term::Terminal_retained_line_provenance first_line =
        model.retained_line_provenance_for_testing(term::Terminal_buffer_id::PRIMARY, 0);
    const term::Terminal_retained_line_provenance second_line =
        model.retained_line_provenance_for_testing(term::Terminal_buffer_id::PRIMARY, 1);
    request.selections.push_back({
        {{0, 1}, {1, 4}, term::Terminal_selection_mode::NORMAL},
        {
            term::terminal_selection_line_lease_from_retained_identity(
                0, first_line.retained_line_id, first_line.content_generation),
            term::terminal_selection_line_lease_from_retained_identity(
                1, second_line.retained_line_id, second_line.content_generation),
        },
    });

    const term::Terminal_render_snapshot snapshot = model.render_snapshot(request);
    ok &= check(snapshot.dirty_row_ranges.size() == 2U &&
        snapshot.dirty_row_ranges[0].first_row == 1 &&
        snapshot.dirty_row_ranges[0].row_count == 3 &&
        snapshot.dirty_row_ranges[1].first_row == 5 &&
        snapshot.dirty_row_ranges[1].row_count == 1,
        "dirty rows are compacted into contiguous ranges");
    ok &= check(snapshot.cursor.shape == term::Terminal_cursor_shape::BAR &&
        !snapshot.cursor.blink_enabled,
        "cursor shape and blink metadata are copied");
    ok &= check(snapshot.ime_preedit.text == QStringLiteral("ime") &&
        snapshot.ime_preedit.active,
        "IME preedit is copied");
    ok &= check(!snapshot.metadata.backend_geometry_in_sync &&
        snapshot.metadata.visual_bell_active,
        "backend sync and visual bell metadata are copied");
    ok &= check(snapshot.metadata.processed_backend_callback_epoch == 17U,
        "processed backend callback epoch metadata is copied");
    ok &= check(snapshot.selection_spans.size() == 2U &&
        snapshot.selection_spans[0].row == 0 &&
        snapshot.selection_spans[1].row == 1,
        "selection range is converted to grid-relative spans");

    term::Terminal_render_snapshot_request empty_line_request =
        request_for_model(model, 31U);
    empty_line_request.selections.push_back({
        {{0, 1}, {1, 4}, term::Terminal_selection_mode::NORMAL},
        {},
    });
    const term::Terminal_render_snapshot empty_line_snapshot =
        model.render_snapshot(empty_line_request);
    ok &= check(empty_line_snapshot.selection_spans.empty(),
        "selection request with empty retained-line proof emits no spans");

    term::Terminal_render_snapshot_request incomplete_line_request =
        request_for_model(model, 32U);
    incomplete_line_request.selections.push_back({
        {{0, 1}, {1, 4}, term::Terminal_selection_mode::NORMAL},
        {
            term::terminal_selection_line_lease_from_retained_identity(
                0, first_line.retained_line_id, first_line.content_generation),
        },
    });
    const term::Terminal_render_snapshot incomplete_line_snapshot =
        model.render_snapshot(incomplete_line_request);
    ok &= check(incomplete_line_snapshot.selection_spans.empty(),
        "selection request with incomplete retained-line proof emits no spans");

    term::Terminal_render_snapshot_request stale_line_request =
        request_for_model(model, 33U);
    stale_line_request.selections.push_back({
        {{0, 1}, {1, 4}, term::Terminal_selection_mode::NORMAL},
        {
            term::terminal_selection_line_lease_from_retained_identity(
                0, first_line.retained_line_id, first_line.content_generation + 1U),
            term::terminal_selection_line_lease_from_retained_identity(
                1, second_line.retained_line_id, second_line.content_generation),
        },
    });
    const term::Terminal_render_snapshot stale_line_snapshot =
        model.render_snapshot(stale_line_request);
    ok &= check(stale_line_snapshot.selection_spans.empty(),
        "selection request with stale retained-line proof emits no spans");

    request.viewport_changed = true;
    const term::Terminal_render_snapshot full_repaint = model.render_snapshot(request);
    ok &= check(full_repaint.dirty_row_ranges.size() == 1U &&
        full_repaint.dirty_row_ranges.front().first_row == 0 &&
        full_repaint.dirty_row_ranges.front().row_count == 6,
        "viewport change publishes full visible-row damage");
    ok &= check(term::validate_render_snapshot(full_repaint).status ==
        term::Terminal_render_snapshot_status::OK,
        "context-rich snapshot validates");
    return ok;
}

bool test_selection_request_rejects_retained_line_row_reorder()
{
    bool ok = true;
    term::Terminal_screen_model model = make_model({4, 12});
    model.ingest(QByteArrayLiteral("alpha\r\nbeta\r\ngamma"));

    const term::Terminal_retained_line_provenance beta_line =
        model.retained_line_provenance_for_testing(term::Terminal_buffer_id::PRIMARY, 1);
    term::Terminal_render_snapshot_request request = request_for_model(model, 40U);
    request.selections.push_back({
        {{1, 0}, {1, 4}, term::Terminal_selection_mode::NORMAL},
        {
            term::terminal_selection_line_lease_from_retained_identity(
                0, beta_line.retained_line_id, beta_line.content_generation),
        },
    });

    const term::Terminal_render_snapshot selected_snapshot =
        model.render_snapshot(request);
    ok &= check(selected_snapshot.selection_spans.size() == 1U &&
        selected_snapshot.selection_spans.front().row == 1,
        "selection request emits a span before row movement");

    model.ingest(QByteArrayLiteral("\x1b[1;1H\x1b[L"));
    const term::Terminal_render_snapshot moved_snapshot =
        model.render_snapshot(request);
    ok &= check(row_text(moved_snapshot, 2) == QStringLiteral("beta"),
        "insert-line fixture moves the retained selected text without mutating it");
    ok &= check(moved_snapshot.selection_spans.empty(),
        "selection request rejects retained-line descriptors after row movement");

    return ok;
}

bool test_dirty_rows_are_viewport_relative()
{
    bool ok = true;
    term::Terminal_screen_model model = make_model({3, 8});
    model.ingest(QByteArrayLiteral("one\r\ntwo\r\nthree\r\nfour"));

    ok &= check(model.scrollback_size() > 0, "dirty-row test has scrollback");
    term::Terminal_render_snapshot_request request = request_for_model(model, 50U, 1);
    request.dirty_rows = {0, 2};
    const term::Terminal_render_snapshot snapshot = model.render_snapshot(request);

    ok &= check(snapshot.dirty_row_ranges.size() == 1U &&
        snapshot.dirty_row_ranges.front().first_row == 1 &&
        snapshot.dirty_row_ranges.front().row_count == 1,
        "screen dirty row is translated to viewport-relative damage");
    return ok;
}

bool test_model_snapshots_publish_visible_line_provenance()
{
    bool ok = true;

    term::Terminal_screen_model scroll_model = make_model({3, 8});
    scroll_model.ingest(QByteArrayLiteral("one\r\ntwo\r\nthree\r\nfour"));

    ok &= check(scroll_model.scrollback_size() > 0,
        "line provenance scrollback fixture has retained history");
    const term::Terminal_render_snapshot tail_snapshot =
        scroll_model.render_snapshot(request_for_model(scroll_model, 60U));
    ok &= check(term::validate_render_snapshot(tail_snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "tail line provenance snapshot validates");
    ok &= check_visible_line_provenance_matches_model(
        scroll_model,
        tail_snapshot,
        "tail line provenance descriptors match visible logical rows");
    ok &= check(std::all_of(
            tail_snapshot.visible_line_provenance.begin(),
            tail_snapshot.visible_line_provenance.end(),
            [](const term::Terminal_render_line_provenance& provenance) {
                return provenance.source ==
                    term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE;
            }),
        "recovery-disabled ordinary output publishes terminal-storage provenance source");

    const term::Terminal_render_snapshot scrollback_snapshot =
        scroll_model.render_snapshot(request_for_model(scroll_model, 61U, 1));
    ok &= check(term::validate_render_snapshot(scrollback_snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "scrollback line provenance snapshot validates");
    ok &= check_visible_line_provenance_matches_model(
        scroll_model,
        scrollback_snapshot,
        "scrollback line provenance descriptors match visible logical rows");

    term::Terminal_screen_model model = make_model({3, 8});
    model.ingest(QByteArrayLiteral("alpha\r\nbeta"));
    const term::Terminal_render_snapshot before_cursor =
        model.render_snapshot(request_for_model(model, 70U));

    model.ingest(QByteArrayLiteral("\x1b[2;3H"));
    const term::Terminal_render_snapshot cursor_only =
        model.render_snapshot(request_for_model(model, 71U));
    ok &= check(cursor_only.visible_line_provenance ==
        before_cursor.visible_line_provenance,
        "cursor-only snapshot preserves visible line provenance");

    model.ingest(QByteArrayLiteral("\x1b[1;1HZ"));
    const term::Terminal_render_snapshot mutated =
        model.render_snapshot(request_for_model(model, 72U));
    ok &= check(
        mutated.visible_line_provenance[0].retained_line_id ==
            before_cursor.visible_line_provenance[0].retained_line_id &&
        mutated.visible_line_provenance[0].content_generation >
            before_cursor.visible_line_provenance[0].content_generation,
        "content mutation preserves retained line id and advances row generation");
    ok &= check(
        mutated.visible_line_provenance[1] == before_cursor.visible_line_provenance[1] &&
        mutated.visible_line_provenance[2] == before_cursor.visible_line_provenance[2],
        "unmodified visible rows keep line provenance descriptors");

    term::Terminal_screen_model_config config;
    config.grid_size                                = {4, 8};
    config.scrollback_limit                         = 8;
    config.tab_width                                = 8;
    config.recover_scrollback_from_primary_repaints = true;
    term::Terminal_screen_model recovered_model(config);
    recovered_model.ingest(visible_row_write_stream({
        QByteArrayLiteral("aa"),
        QByteArrayLiteral("bb"),
        QByteArrayLiteral("cc"),
        QByteArrayLiteral("dd"),
    }, false));
    const term::Terminal_screen_model_result recovery_result =
        recovered_model.ingest(visible_row_write_stream({
            QByteArrayLiteral("bb"),
            QByteArrayLiteral("cc"),
            QByteArrayLiteral("dd"),
            QByteArrayLiteral("ee"),
        }, true));
    ok &= check(recovery_result.recovery_proposals.size() == 1U &&
            recovered_model.scrollback_size() == 1,
        "recovered provenance fixture accepts one primary-repaint row");

    const term::Terminal_render_snapshot recovered_snapshot =
        recovered_model.render_snapshot(request_for_model(recovered_model, 73U, 1));
    ok &= check(term::validate_render_snapshot(recovered_snapshot).status ==
            term::Terminal_render_snapshot_status::OK,
        "recovered provenance snapshot validates");
    ok &= check_visible_line_provenance_matches_model(
        recovered_model,
        recovered_snapshot,
        "recovered provenance snapshot descriptors match visible logical rows");
    ok &= check(
        row_text(recovered_snapshot, 0) == QStringLiteral("aa") &&
            recovered_snapshot.visible_line_provenance[0].source ==
                term::Terminal_retained_line_provenance_source::RECOVERED_PRIMARY_REPAINT &&
            recovered_snapshot.visible_line_provenance[1].source ==
                term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        "recovered provenance source survives render snapshot construction");

    term::Terminal_render_line_provenance ordinary_source =
        recovered_snapshot.visible_line_provenance[0];
    ordinary_source.source = term::Terminal_retained_line_provenance_source::TERMINAL_STORAGE;
    ok &= check(
        !(ordinary_source == recovered_snapshot.visible_line_provenance[0]),
        "visible line provenance equality distinguishes recovered source");
    return ok;
}

bool test_row_content_stamps_track_writes_and_survive_scrollback()
{
    bool ok = true;

    const qint64 ingest_start_ms = QDateTime::currentMSecsSinceEpoch();
    term::Terminal_screen_model model = make_model({3, 8});
    model.ingest(QByteArrayLiteral("one\r\ntwo"));
    const qint64 ingest_end_ms = QDateTime::currentMSecsSinceEpoch();

    const term::Terminal_render_snapshot written =
        model.render_snapshot(request_for_model(model, 130U));
    const qint64 written_stamp_ms = written.visible_line_provenance[0].content_stamp_ms;
    ok &= check(
        written_stamp_ms >= ingest_start_ms &&
        written_stamp_ms <= ingest_end_ms,
        "written row carries a wall-clock stamp from the ingest window");
    ok &= check(written.visible_line_provenance[2].content_stamp_ms == 0,
        "never-written blank row carries the zero no-timestamp stamp");

    // Scroll the first row into retained history; reading it back from the
    // history ring must reproduce the original stamp byte for byte, proving
    // the stamp survives the row-record codec round trip.
    model.ingest(QByteArrayLiteral("\r\nthree\r\nfour"));
    ok &= check(model.scrollback_size() > 0,
        "row stamp scrollback fixture has retained history");

    const term::Terminal_render_snapshot scrollback = model.render_snapshot(
        request_for_model(model, 131U, model.scrollback_size()));
    ok &= check(row_text(scrollback, 0) == QStringLiteral("one"),
        "row stamp scrollback snapshot shows the retained first row");
    ok &= check(
        scrollback.visible_line_provenance[0].retained_line_id ==
            written.visible_line_provenance[0].retained_line_id,
        "retained first row keeps its retained line id in scrollback");
    ok &= check(
        scrollback.visible_line_provenance[0].content_stamp_ms == written_stamp_ms,
        "retained first row keeps its content stamp in scrollback");

    return ok;
}

// Busy-waits across the next wall-clock millisecond boundary so a follow-up
// write provably lands at a later stamp than `reference_ms`. Stamps have
// millisecond resolution, so without this a write in the same millisecond
// would be indistinguishable from the original one.
void wait_for_wall_clock_after_ms(qint64 reference_ms)
{
    while (QDateTime::currentMSecsSinceEpoch() <= reference_ms) {
    }
}

qint64 active_row_stamp_ms(const term::Terminal_screen_model& model, int logical_row)
{
    return model.retained_line_provenance_for_testing(
        term::Terminal_buffer_id::PRIMARY,
        logical_row).content_stamp_ms;
}

bool test_row_content_stamps_survive_grid_resize()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model({3, 8});
    model.ingest(QByteArrayLiteral("one\r\ntwo"));
    const qint64 first_stamp_ms  = active_row_stamp_ms(model, 0);
    const qint64 second_stamp_ms = active_row_stamp_ms(model, 1);
    ok &= check(first_stamp_ms > 0 && second_stamp_ms > 0,
        "resize stamp fixture rows carry write stamps");

    // Every step below first crosses a wall-clock millisecond boundary so an
    // unwanted restamp cannot hide behind the original stamp value.
    wait_for_wall_clock_after_ms(std::max(first_stamp_ms, second_stamp_ms));

    // The stamp means "when this content arrived as terminal output". A
    // same-width height change leaves every surviving row untouched.
    model.resize({4, 8});
    ok &= check(active_row_stamp_ms(model, 0) == first_stamp_ms &&
        active_row_stamp_ms(model, 1) == second_stamp_ms,
        "same-width height change keeps row content stamps");

    // A width change pads or trims rows with never-written blank cells; the
    // written content is unchanged, so the stamps must not refresh.
    model.resize({4, 12});
    ok &= check(active_row_stamp_ms(model, 0) == first_stamp_ms &&
        active_row_stamp_ms(model, 1) == second_stamp_ms,
        "width growth keeps row content stamps");

    model.resize({4, 8});
    ok &= check(active_row_stamp_ms(model, 0) == first_stamp_ms &&
        active_row_stamp_ms(model, 1) == second_stamp_ms,
        "blank-only width shrink keeps row content stamps");

    // Rewriting rows with identical cells is not new output. This is the
    // overwrite-style repaint a real ConPTY emits after every resize: hide
    // cursor, home, per row the text followed by erase-to-end-of-line, then
    // reposition and show the cursor.
    model.ingest(QByteArrayLiteral(
        "\x1b[?25l\x1b[Hone\x1b[K\r\ntwo\x1b[K\x1b[3;1H\x1b[?25h"));
    ok &= check(active_row_stamp_ms(model, 0) == first_stamp_ms &&
        active_row_stamp_ms(model, 1) == second_stamp_ms,
        "resize-style repaint of identical cells keeps the row content stamps");

    // Genuinely new output after the resizes gets a fresh stamp; neighbors
    // keep theirs.
    model.ingest(QByteArrayLiteral("\x1b[2;1HTWO"));
    const qint64 rewritten_stamp_ms = active_row_stamp_ms(model, 1);
    ok &= check(rewritten_stamp_ms > second_stamp_ms,
        "new output after resize carries a fresh content stamp");
    ok &= check(active_row_stamp_ms(model, 0) == first_stamp_ms,
        "new output on one row leaves sibling row stamps untouched");

    // A width shrink that truncates written cells is a real content change.
    wait_for_wall_clock_after_ms(rewritten_stamp_ms);
    model.resize({4, 2});
    ok &= check(active_row_stamp_ms(model, 0) > first_stamp_ms,
        "width shrink that truncates written cells refreshes the stamp");

    return ok;
}

bool test_visible_line_provenance_validation()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model({2, 8});
    model.ingest(QByteArrayLiteral("A"));
    const term::Terminal_render_snapshot valid =
        model.render_snapshot(request_for_model(model, 80U));
    ok &= check(term::validate_render_snapshot(valid).status ==
        term::Terminal_render_snapshot_status::OK,
        "line provenance validation accepts model snapshot");

    term::Terminal_render_snapshot missing_row = valid;
    missing_row.visible_line_provenance.pop_back();
    ok &= check(term::validate_render_snapshot(missing_row).status ==
        term::Terminal_render_snapshot_status::INVALID_LINE_PROVENANCE,
        "line provenance validation rejects missing visible row descriptor");

    term::Terminal_render_snapshot zero_id = valid;
    zero_id.visible_line_provenance[0].retained_line_id = 0U;
    ok &= check(term::validate_render_snapshot(zero_id).status ==
        term::Terminal_render_snapshot_status::INVALID_LINE_PROVENANCE,
        "line provenance validation rejects zero retained line id");

    term::Terminal_render_snapshot wrong_logical_row = valid;
    ++wrong_logical_row.visible_line_provenance[1].logical_row;
    ok &= check(term::validate_render_snapshot(wrong_logical_row).status ==
        term::Terminal_render_snapshot_status::INVALID_LINE_PROVENANCE,
        "line provenance validation rejects mismatched logical row");

    term::Terminal_render_snapshot invalid_source = valid;
    invalid_source.visible_line_provenance[0].source =
        static_cast<term::Terminal_retained_line_provenance_source>(100);
    ok &= check(term::validate_render_snapshot(invalid_source).status ==
        term::Terminal_render_snapshot_status::INVALID_LINE_PROVENANCE,
        "line provenance validation rejects invalid source");

    term::Terminal_render_snapshot empty_without_spans =
        term::make_empty_render_snapshot({2, 8}, tail_viewport(model), 81U);
    ok &= check(empty_without_spans.visible_line_provenance.empty() &&
        term::validate_render_snapshot(empty_without_spans).status ==
            term::Terminal_render_snapshot_status::OK,
        "empty snapshot may omit line provenance when it emits no spans");

    term::Terminal_render_snapshot empty_with_span = empty_without_spans;
    empty_with_span.selection_spans.push_back({
        {{0, 0}, {0, 1}, term::Terminal_selection_mode::NORMAL},
        0,
        0,
        1,
    });
    ok &= check(term::validate_render_snapshot(empty_with_span).status ==
        term::Terminal_render_snapshot_status::INVALID_LINE_PROVENANCE,
        "snapshot without line provenance cannot publish selection spans");
    return ok;
}

bool test_public_projection_scroll_snapshot_structural_validation()
{
    bool ok = true;

    // The render snapshot validator enforces structural snapshot invariants. It
    // intentionally does not interpret public-scroll diagnostic semantics; those
    // fields are covered by transcript and replay contract tests.
    term::Terminal_viewport_state viewport;
    viewport.active_buffer    = term::Terminal_buffer_id::PRIMARY;
    viewport.scrollback_rows  = 3;
    viewport.visible_rows     = 2;
    viewport.offset_from_tail = 1;
    viewport.follow_tail      = false;

    term::Terminal_render_snapshot valid =
        term::make_empty_render_snapshot({2, 8}, viewport, 90U);
    valid.basis   = term::Terminal_render_snapshot_basis::PUBLIC_PROJECTION;
    valid.purpose = term::Terminal_render_snapshot_purpose::SCROLL;
    valid.public_scroll_diagnostics.effective_policy =
        term::Terminal_synchronized_output_scroll_policy::IMMEDIATE_PUBLIC_PROJECTION;
    valid.public_scroll_diagnostics.public_projection_generation = 7U;
    valid.public_scroll_diagnostics.public_viewport_before = viewport;
    valid.public_scroll_diagnostics.public_viewport_after  = viewport;
    valid.public_scroll_diagnostics.visible_scroll_applied = true;
    valid.public_scroll_diagnostics.live_content_publication_blocked = true;
    valid.visible_line_provenance = {
        {2, 700U, 1U},
        {3, 701U, 1U},
    };
    valid.dirty_row_ranges = {{0, 2}};
    valid.cells.push_back({{0, 0}, QStringLiteral("A")});
    valid.cells.push_back({{1, 0}, QStringLiteral("B")});

    ok &= check(term::validate_render_snapshot(valid).status ==
        term::Terminal_render_snapshot_status::OK,
        "public projection scroll snapshot validates");

    term::Terminal_render_snapshot malformed = valid;
    malformed.visible_line_provenance[1].logical_row = 9;
    ok &= check(term::validate_render_snapshot(malformed).status ==
        term::Terminal_render_snapshot_status::INVALID_LINE_PROVENANCE,
        "public projection scroll snapshot rejects malformed line provenance");

    term::Terminal_render_snapshot invalid_cell = valid;
    invalid_cell.cells.front().position.column = valid.grid_size.columns;
    ok &= check(term::validate_render_snapshot(invalid_cell).status ==
        term::Terminal_render_snapshot_status::INVALID_CELL_POSITION,
        "public projection scroll snapshot rejects malformed cell position");

    term::Terminal_render_snapshot partial_dirty = valid;
    partial_dirty.dirty_row_ranges = {{1, 1}};
    ok &= check(term::validate_render_snapshot(partial_dirty).status ==
        term::Terminal_render_snapshot_status::INVALID_DIRTY_ROW_RANGE,
        "public projection scroll snapshot rejects partial dirty ranges");

    term::Terminal_render_snapshot missing_dirty = valid;
    missing_dirty.dirty_row_ranges.clear();
    ok &= check(term::validate_render_snapshot(missing_dirty).status ==
        term::Terminal_render_snapshot_status::INVALID_DIRTY_ROW_RANGE,
        "public projection scroll snapshot requires a full visible-viewport dirty range");

    term::Terminal_render_snapshot public_content = valid;
    public_content.purpose = term::Terminal_render_snapshot_purpose::CONTENT;
    ok &= check(term::validate_render_snapshot(public_content).status ==
        term::Terminal_render_snapshot_status::INVALID_SNAPSHOT_BASIS_PURPOSE,
        "public projection snapshot rejects non-scroll purpose");

    term::Terminal_render_snapshot live_scroll = valid;
    live_scroll.basis = term::Terminal_render_snapshot_basis::LIVE_CONTENT;
    ok &= check(term::validate_render_snapshot(live_scroll).status ==
        term::Terminal_render_snapshot_status::INVALID_SNAPSHOT_BASIS_PURPOSE,
        "live-content snapshot rejects scroll purpose");

    term::Terminal_render_snapshot alternate_public_scroll = valid;
    alternate_public_scroll.viewport.active_buffer = term::Terminal_buffer_id::ALTERNATE;
    alternate_public_scroll.viewport.scrollback_rows  = 0;
    alternate_public_scroll.viewport.offset_from_tail = 0;
    alternate_public_scroll.visible_line_provenance = {
        {0, 700U, 1U},
        {1, 701U, 1U},
    };
    ok &= check(term::validate_render_snapshot(alternate_public_scroll).status ==
        term::Terminal_render_snapshot_status::INVALID_VIEWPORT,
        "public projection scroll snapshot remains primary-buffer only");

    return ok;
}

bool test_session_snapshot_handles_and_synchronized_release()
{
    bool                                    ok      = true;
    Recording_backend*                      backend = nullptr;
    std::unique_ptr<term::Terminal_session> session = make_session(backend);

    ok &= check(session->start(launch_config({3, 12})).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "session starts");
    ok &= check(backend != nullptr &&
        backend->emit_output(QByteArrayLiteral("one")),
        "backend emits first output");

    const std::shared_ptr<const term::Terminal_render_snapshot> first =
        session->latest_render_snapshot_handle();
    ok &= check(first != nullptr && row_text(*first, 0).contains(QStringLiteral("one")),
        "session publishes immutable snapshot handle");
    const std::uint64_t first_generation = session->render_snapshot_generation();

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026hheld")),
        "backend emits synchronized-output hold");
    ok &= check(session->render_snapshot_generation() == first_generation,
        "held synchronized output does not publish a snapshot");
    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026l")),
        "backend releases synchronized output");
    ok &= check(session->render_snapshot_generation() == first_generation + 1U,
        "synchronized release publishes one coalesced snapshot");
    ok &= check(row_text(*first, 0).contains(QStringLiteral("one")),
        "old session snapshot handle remains immutable");

    const std::shared_ptr<const term::Terminal_render_snapshot> released =
        session->latest_render_snapshot_handle();
    ok &= check(released != nullptr &&
        (row_text(*released, 0) + row_text(*released, 1)).contains(QStringLiteral("held")),
        "released snapshot contains held text");

    ok &= check(backend->emit_output(QByteArrayLiteral("\a")),
        "backend emits bell");
    const std::shared_ptr<const term::Terminal_render_snapshot> bell =
        session->latest_render_snapshot_handle();
    ok &= check(bell != nullptr && bell->metadata.visual_bell_active,
        "bell publishes visual bell metadata without text mutation");
    ok &= check(bell != nullptr && bell->metadata.backend_geometry_in_sync,
        "snapshot carries backend geometry sync metadata");
    return ok;
}

bool test_snapshot_publication_generation_metadata()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model({2, 12});
    model.ingest(QByteArrayLiteral("publication"));

    term::Terminal_render_snapshot_request request = request_for_model(model, 711U);
    request.publication_generation = 19U;
    const term::Terminal_render_snapshot snapshot = model.render_snapshot(request);
    ok &= check(snapshot.metadata.sequence == 711U,
        "snapshot keeps content sequence metadata");
    ok &= check(snapshot.metadata.publication_generation == 19U,
        "snapshot carries render publication generation metadata");
    ok &= check(term::validate_render_snapshot(snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "snapshot with publication generation metadata validates");

    const term::Terminal_render_snapshot empty =
        term::make_empty_render_snapshot({2, 12}, tail_viewport(model), 712U, 20U);
    ok &= check(empty.metadata.sequence == 712U &&
            empty.metadata.publication_generation == 20U,
        "empty snapshot helper carries publication generation metadata");
    return ok;
}

bool test_session_install_does_not_advance_rendered_publication()
{
    bool                                    ok      = true;
    Recording_backend*                      backend = nullptr;
    std::unique_ptr<term::Terminal_session> session = make_session(backend);

#if VNM_TERMINAL_PROFILING_ENABLED
    session->set_profile_stats_enabled(true);
#endif

    ok &= check(session->start(launch_config({3, 12})).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "installed/rendered split session starts");
    ok &= check(backend != nullptr &&
        backend->emit_output(QByteArrayLiteral("\x1b[1;1Hone\x1b[3;1H")),
        "installed/rendered split publishes first snapshot");

    const std::shared_ptr<const term::Terminal_render_snapshot> first =
        session->latest_render_snapshot_handle();
    const std::uint64_t first_generation = session->render_snapshot_generation();
    ok &= check(first != nullptr &&
            first->metadata.publication_generation == first_generation,
        "first snapshot metadata matches session publication generation");
    ok &= check(first != nullptr && term::render_snapshot_row_is_dirty(*first, 0),
        "first snapshot marks the prior unrendered row dirty");

    session->mark_render_snapshot_installed(first_generation);
    ok &= check(session->installed_render_snapshot_generation() == first_generation,
        "bridge install advances installed publication generation");
    ok &= check(session->rendered_render_snapshot_generation() == 0U,
        "bridge install does not advance actually rendered publication generation");

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[2;1Htwo")),
        "installed/rendered split publishes second snapshot");
    const std::shared_ptr<const term::Terminal_render_snapshot> second =
        session->latest_render_snapshot_handle();
    ok &= check(second != nullptr &&
            second->metadata.publication_generation == first_generation + 1U,
        "second snapshot advances publication generation metadata");
    ok &= check(second != nullptr &&
            term::render_snapshot_row_is_dirty(*second, 0) &&
            term::render_snapshot_row_is_dirty(*second, 1),
        "second snapshot keeps prior unrendered and current dirty rows");
#if VNM_TERMINAL_PROFILING_ENABLED
    const term::Terminal_session_profile_stats stats = session->profile_stats();
    ok &= check(stats.dirty_coalescing_applied > 0U,
        "bridge install does not clear session-owned dirty coalescing basis");
#endif

    session->mark_render_publication_rendered(session->render_snapshot_generation());
    ok &= check(
        session->rendered_render_snapshot_generation() ==
            session->render_snapshot_generation(),
        "actual render completion advances rendered publication generation");
    return ok;
}

bool test_session_ime_overlay_does_not_clone_render_snapshot()
{
    bool                                    ok      = true;
    Recording_backend*                      backend = nullptr;
    std::unique_ptr<term::Terminal_session> session = make_session(backend);

    ok &= check(session->start(launch_config({3, 12})).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "IME overlay session starts");
    ok &= check(backend != nullptr &&
        backend->emit_output(QByteArrayLiteral("base")),
        "IME overlay session publishes initial model snapshot");

    const std::shared_ptr<const term::Terminal_render_snapshot> base_snapshot =
        session->latest_render_snapshot_handle();
    const std::uint64_t base_snapshot_generation = session->render_snapshot_generation();
    const std::uint64_t base_ime_generation      = session->ime_preedit_generation();
    (void)session->take_pending_notifications();

    session->set_ime_preedit(QStringLiteral("ime"), 2);
    term::Ime_preedit_state ime_preedit = session->ime_preedit_state();
    ok &= check(ime_preedit.active &&
        ime_preedit.text == QStringLiteral("ime") &&
        ime_preedit.cursor_position == 2,
        "IME overlay state is queryable from session");
    ok &= check(session->ime_preedit_generation() == base_ime_generation + 1U,
        "IME preedit update advances only IME generation");
    ok &= check(session->render_snapshot_generation() == base_snapshot_generation,
        "IME preedit update does not advance render snapshot generation");
    ok &= check(session->latest_render_snapshot_handle() == base_snapshot,
        "IME preedit update does not replace latest render snapshot handle");

    ok &= check(session->take_pending_notifications().empty(),
        "IME preedit update does not enqueue public session notifications");

    session->cancel_ime_preedit();
    ime_preedit  = session->ime_preedit_state();
    ok          &= check(!ime_preedit.active && ime_preedit.text.isEmpty(),
        "IME cancel clears session overlay state");
    ok          &= check(session->ime_preedit_generation() == base_ime_generation + 2U,
        "IME cancel advances IME generation");
    ok          &= check(session->render_snapshot_generation() == base_snapshot_generation,
        "IME cancel does not advance render snapshot generation");
    ok          &= check(session->latest_render_snapshot_handle() == base_snapshot,
        "IME cancel does not replace latest render snapshot handle");

    return ok;
}

bool test_backend_sync_metadata_publishes_after_same_grid_retry()
{
    bool                                    ok      = true;
    Recording_backend*                      backend = nullptr;
    std::unique_ptr<term::Terminal_session> session = make_session(backend);

    ok &= check(session->start(launch_config({3, 12})).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "retry session starts");
    ok &= check(backend != nullptr &&
        backend->emit_output(QByteArrayLiteral("ready")),
        "retry session publishes an initial snapshot");

    const std::uint64_t initial_generation = session->render_snapshot_generation();
    backend->reject_next_resize = true;
    ok &= check(session->resize(QSizeF(120.0, 80.0), {4, 12}).code ==
        term::Terminal_session_result_code::BACKEND_REJECTED,
        "first resize is rejected by backend");

    const std::shared_ptr<const term::Terminal_render_snapshot> failed =
        session->latest_render_snapshot_handle();
    ok &= check(failed != nullptr && !failed->metadata.backend_geometry_in_sync,
        "failed resize publishes out-of-sync metadata");
    const std::uint64_t failed_generation = session->render_snapshot_generation();
    ok &= check(failed_generation > initial_generation,
        "failed resize advances snapshot generation");

    ok &= check(session->resize(QSizeF(120.0, 80.0), {4, 12}).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "same-grid resize retry succeeds");
    const std::shared_ptr<const term::Terminal_render_snapshot> recovered =
        session->latest_render_snapshot_handle();
    ok &= check(recovered != nullptr && recovered->metadata.backend_geometry_in_sync,
        "same-grid retry publishes restored sync metadata");
    ok &= check(session->render_snapshot_generation() == failed_generation + 1U,
        "same-grid retry advances snapshot generation");
    return ok;
}

bool test_resize_metadata_publication_respects_synchronized_output()
{
    bool                                    ok      = true;
    Recording_backend*                      backend = nullptr;
    std::unique_ptr<term::Terminal_session> session = make_session(backend);
#if VNM_TERMINAL_PROFILING_ENABLED
    session->set_profile_stats_enabled(true);
#endif

    ok &= check(session->start(launch_config({3, 12})).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "synchronized resize session starts");
    ok &= check(backend != nullptr &&
        backend->emit_output(QByteArrayLiteral("base")),
        "synchronized resize session publishes an initial snapshot");

    const std::uint64_t initial_generation = session->render_snapshot_generation();
    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026h\r\nH")),
        "synchronized resize session enters hold");
    ok &= check(session->render_snapshot_generation() == initial_generation,
        "held output does not publish before resize");

    backend->reject_next_resize = true;
    ok &= check(session->resize(QSizeF(20.0, 80.0), {4, 2}).code ==
        term::Terminal_session_result_code::BACKEND_REJECTED,
        "held resize failure is reported");
    const std::shared_ptr<const term::Terminal_render_snapshot> failed =
        session->latest_render_snapshot_handle();
    ok &= check(session->render_snapshot_generation() == initial_generation + 1U,
        "held resize failure publishes a geometry-only snapshot");
    ok &= check(failed != nullptr &&
        failed->grid_size.rows == 4 &&
        failed->grid_size.columns == 2 &&
        failed->basis == term::Terminal_render_snapshot_basis::LIVE_CONTENT &&
        failed->purpose == term::Terminal_render_snapshot_purpose::GEOMETRY_DERIVED &&
        !failed->metadata.backend_geometry_in_sync &&
        row_text(*failed, 0) == QStringLiteral("ba") &&
        cell_with_text(*failed, QStringLiteral("H")) == nullptr,
        "held resize failure publishes clipped live-content geometry without hidden content");
    ok &= check(failed != nullptr &&
        term::validate_render_snapshot(*failed).status ==
            term::Terminal_render_snapshot_status::OK,
        "held resize failure geometry snapshot validates");
#if VNM_TERMINAL_PROFILING_ENABLED
    const term::Terminal_session_profile_stats failed_profile_stats =
        session->profile_stats();
    ok &= check(failed_profile_stats.geometry_derived_materialization_calls == 1U &&
        failed_profile_stats.geometry_derived_materialization_rows == 4U,
        "height-changing failed geometry snapshot counts adapted output rows");
#endif

    ok &= check(session->resize(QSizeF(120.0, 80.0), {4, 12}).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "held grow resize recovery succeeds");
    const std::shared_ptr<const term::Terminal_render_snapshot> recovered =
        session->latest_render_snapshot_handle();
    ok &= check(session->render_snapshot_generation() == initial_generation + 2U,
        "held resize recovery publishes a geometry-only snapshot");
    ok &= check(recovered != nullptr &&
        recovered->grid_size.rows == 4 &&
        recovered->grid_size.columns == 12 &&
        recovered->basis == term::Terminal_render_snapshot_basis::LIVE_CONTENT &&
        recovered->purpose == term::Terminal_render_snapshot_purpose::GEOMETRY_DERIVED &&
        recovered->metadata.backend_geometry_in_sync &&
        row_text(*recovered, 0) == QStringLiteral("base") &&
        cell_with_text(*recovered, QStringLiteral("H")) == nullptr,
        "held resize recovery restores safe public content baseline without hidden content");
    ok &= check(recovered != nullptr &&
        term::validate_render_snapshot(*recovered).status ==
            term::Terminal_render_snapshot_status::OK,
        "held resize recovery geometry snapshot validates");
#if VNM_TERMINAL_PROFILING_ENABLED
    const term::Terminal_session_profile_stats recovered_profile_stats =
        session->profile_stats();
    ok &= check(recovered_profile_stats.geometry_derived_materialization_calls == 2U &&
        recovered_profile_stats.geometry_derived_materialization_rows == 8U,
        "height-changing geometry snapshots accumulate adapted output rows");
#endif

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026l")),
        "synchronized resize session releases hold");
    const std::shared_ptr<const term::Terminal_render_snapshot> released =
        session->latest_render_snapshot_handle();
    ok &= check(session->render_snapshot_generation() == initial_generation + 3U,
        "release publishes accumulated content after held resize geometry snapshots");
    ok &= check(released != nullptr && released->metadata.backend_geometry_in_sync,
        "released coalesced snapshot carries current backend sync metadata");
    ok &= check(released != nullptr &&
        (row_text(*released, 0) + row_text(*released, 1)).contains(QStringLiteral("H")),
        "released coalesced snapshot includes held output");
    return ok;
}

bool test_synthetic_snapshots_preserve_or_suppress_line_provenance()
{
    bool ok = true;

    Recording_backend* backend = nullptr;
    std::unique_ptr<term::Terminal_session> session = make_session(backend);
    ok &= check(session->start(launch_config({3, 12})).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "synthetic provenance geometry session starts");
    ok &= check(backend != nullptr &&
        backend->emit_output(QByteArrayLiteral("base")),
        "synthetic provenance geometry session publishes base content");

    const std::shared_ptr<const term::Terminal_render_snapshot> base =
        session->latest_render_snapshot_handle();
    ok &= check(base != nullptr &&
        term::render_snapshot_visible_line_provenance_is_valid(*base),
        "base content snapshot has valid line provenance");
    const std::vector<term::Terminal_render_line_provenance> base_provenance =
        base != nullptr ? base->visible_line_provenance :
            std::vector<term::Terminal_render_line_provenance>{};
    const std::uint64_t base_generation = session->render_snapshot_generation();

    ok &= check(backend->emit_output(QByteArrayLiteral("\x1b[?2026hhidden")),
        "synthetic provenance geometry session enters synchronized output");
    ok &= check(session->render_snapshot_generation() == base_generation,
        "synchronized output entry publishes no hidden content snapshot");

    ok &= check(session->resize(QSizeF(120.0, 80.0), {3, 12}).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "same-grid geometry snapshot is accepted during synchronized output");
    const std::shared_ptr<const term::Terminal_render_snapshot> same_grid =
        session->latest_render_snapshot_handle();
    ok &= check(same_grid != nullptr &&
        same_grid->visible_line_provenance == base_provenance &&
        same_grid->selection_spans.empty() &&
        cell_with_text(*same_grid, QStringLiteral("h")) == nullptr,
        "same-grid geometry snapshot preserves provenance without exposing hidden content");
    ok &= check(same_grid != nullptr &&
        term::validate_render_snapshot(*same_grid).status ==
            term::Terminal_render_snapshot_status::OK,
        "same-grid geometry snapshot validates");

    ok &= check(session->resize(QSizeF(120.0, 100.0), {4, 12}).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "changed-grid geometry snapshot is accepted during synchronized output");
    const std::shared_ptr<const term::Terminal_render_snapshot> changed_grid =
        session->latest_render_snapshot_handle();
    ok &= check(changed_grid != nullptr &&
        changed_grid->visible_line_provenance.empty() &&
        changed_grid->basis == term::Terminal_render_snapshot_basis::LIVE_CONTENT &&
        changed_grid->purpose == term::Terminal_render_snapshot_purpose::GEOMETRY_DERIVED &&
        changed_grid->selection_spans.empty(),
        "changed-grid geometry snapshot suppresses unproven provenance and spans");
    ok &= check(changed_grid != nullptr &&
        term::validate_render_snapshot(*changed_grid).status ==
            term::Terminal_render_snapshot_status::OK,
        "changed-grid geometry snapshot validates without provenance");

    Recording_backend* selection_backend = nullptr;
    std::unique_ptr<term::Terminal_session> selection_session =
        make_session(selection_backend);
    ok &= check(selection_session->start(launch_config({3, 12})).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "selection-only provenance session starts");
    ok &= check(selection_backend != nullptr &&
        selection_backend->emit_output(QByteArrayLiteral("select")),
        "selection-only provenance session publishes selectable content");
    selection_session->set_selection_range({
        {0, 0},
        {0, 6},
        term::Terminal_selection_mode::NORMAL,
    });

    const std::shared_ptr<const term::Terminal_render_snapshot> selected =
        selection_session->latest_render_snapshot_handle();
    ok &= check(selected != nullptr &&
        !selected->selection_spans.empty() &&
        term::render_snapshot_visible_line_provenance_is_valid(*selected),
        "selected snapshot has spans and valid line provenance");
    const std::vector<term::Terminal_render_line_provenance> selected_provenance =
        selected != nullptr ? selected->visible_line_provenance :
            std::vector<term::Terminal_render_line_provenance>{};

    ok &= check(selection_backend->emit_output(QByteArrayLiteral("\x1b[?2026hhidden")),
        "selection-only provenance session enters synchronized output");
    selection_session->detach_selection_visual_attachment();
    const std::shared_ptr<const term::Terminal_render_snapshot> detached =
        selection_session->latest_render_snapshot_handle();
    ok &= check(detached != nullptr &&
        detached->basis == term::Terminal_render_snapshot_basis::LIVE_CONTENT &&
        detached->purpose == term::Terminal_render_snapshot_purpose::SELECTION_DERIVED &&
        detached->selection_spans.empty() &&
        detached->visible_line_provenance == selected_provenance &&
        cell_with_text(*detached, QStringLiteral("h")) == nullptr,
        "selection-only snapshot preserves previous provenance and suppresses spans");
    ok &= check(detached != nullptr &&
        term::validate_render_snapshot(*detached).status ==
            term::Terminal_render_snapshot_status::OK,
        "selection-only snapshot validates");
    return ok;
}

bool test_session_profile_snapshot_publication_counters()
{
    bool ok = true;

#if VNM_TERMINAL_PROFILING_ENABLED
    Recording_backend* backend = nullptr;
    std::unique_ptr<term::Terminal_session> session = make_session(backend);
    session->set_profile_stats_enabled(true);

    ok &= check(session->start(launch_config({3, 12})).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "profile counter session starts");
    ok &= check(backend != nullptr &&
        backend->emit_output(QByteArrayLiteral("one")),
        "profile counter session publishes first snapshot");
    ok &= check(backend->emit_output(QByteArrayLiteral("two")),
        "profile counter session publishes second snapshot");
    ok &= check(backend->emit_output(QByteArrayLiteral("three")),
        "profile counter session publishes third snapshot");

    const term::Terminal_session_profile_stats before_sync = session->profile_stats();
    ok &= check(before_sync.render_snapshot_publications == 3U,
        "profile counter session counted three publications");
    ok &= check(before_sync.snapshots_superseded_before_render == 2U,
        "profile counter session counted each unsynced replacement once");
    ok &= check(before_sync.max_unrendered_snapshot_generations == 3U,
        "profile counter session tracked the maximum unsynced backlog");
    ok &= check(before_sync.snapshots_consumed_by_bridge == 0U,
        "profile counter session has no bridge consumption before sync");

    session->mark_render_snapshot_installed(session->render_snapshot_generation());
    const term::Terminal_session_profile_stats after_sync = session->profile_stats();
    ok &= check(after_sync.snapshots_consumed_by_bridge == 1U,
        "profile counter session consumed only the latest installed snapshot");
    ok &= check(after_sync.snapshots_marked_rendered == 0U,
        "profile counter session does not count bridge install as actual render");

    session->mark_render_publication_rendered(session->render_snapshot_generation());
    const term::Terminal_session_profile_stats after_repeat_sync =
        session->profile_stats();
    ok &= check(after_repeat_sync.snapshots_consumed_by_bridge == 1U,
        "profile counter session render completion does not consume another snapshot");
    ok &= check(after_repeat_sync.snapshots_marked_rendered == 1U,
        "profile counter session records actual render completion separately");

    Recording_backend* late_backend = nullptr;
    std::unique_ptr<term::Terminal_session> late_profile_session =
        make_session(late_backend);
    ok &= check(late_profile_session->start(launch_config({3, 12})).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "late profile counter session starts");
    ok &= check(late_backend != nullptr &&
        late_backend->emit_output(QByteArray::fromHex(QByteArrayLiteral("f09f9982"))),
        "late profile counter session publishes fallback text content");
    const std::shared_ptr<const term::Terminal_render_snapshot> late_snapshot =
        late_profile_session->latest_render_snapshot_handle();
    ok &= check(late_snapshot != nullptr,
        "late profile counter session retained a snapshot before profiling");
    late_profile_session->set_profile_stats_enabled(true);
    const term::Terminal_session_profile_stats late_stats =
        late_profile_session->profile_stats();
    const std::uint64_t retained_cell_capacity_bytes =
        late_snapshot != nullptr
            ? static_cast<std::uint64_t>(late_snapshot->cells.capacity()) *
                static_cast<std::uint64_t>(sizeof(term::Terminal_render_cell))
            : 0U;
    std::uint64_t fallback_qstring_payload_bytes = 0U;
    if (late_snapshot != nullptr) {
        for (const term::Terminal_render_cell& cell : late_snapshot->cells) {
            const QString* fallback = cell.text.fallback_qstring_or_null();
            if (fallback != nullptr) {
                fallback_qstring_payload_bytes +=
                    static_cast<std::uint64_t>(sizeof(QString)) +
                    static_cast<std::uint64_t>(fallback->capacity()) *
                        static_cast<std::uint64_t>(sizeof(QChar));
            }
        }
    }
    ok &= check(late_stats.retained_snapshot_generation_count > 0U,
        "late profile enable refreshes retained snapshot generation count");
    ok &= check(fallback_qstring_payload_bytes > 0U,
        "late profile counter session retained fallback QString payload");
    ok &= check(late_stats.retained_snapshot_payload_bytes >=
            retained_cell_capacity_bytes + fallback_qstring_payload_bytes,
        "late profile enable refreshes retained snapshot payload bytes including fallback text");
    ok &= check(late_stats.max_retained_snapshot_payload_bytes >=
            late_stats.retained_snapshot_payload_bytes &&
        late_stats.max_retained_snapshot_generation_count >=
            late_stats.retained_snapshot_generation_count,
        "late profile enable refreshes retained snapshot max gauges");

    Recording_backend* selection_backend = nullptr;
    std::unique_ptr<term::Terminal_session> selection_session =
        make_session(selection_backend);
    selection_session->set_profile_stats_enabled(true);
    ok &= check(selection_session->start(launch_config({3, 12})).code ==
        term::Terminal_session_result_code::ACCEPTED,
        "selection profile counter session starts");
    ok &= check(selection_backend != nullptr &&
        selection_backend->emit_output(QByteArrayLiteral("select")),
        "selection profile counter session publishes content");
    selection_session->set_selection_range({
        {0, 0},
        {0, 6},
        term::Terminal_selection_mode::NORMAL,
    });

    const term::Terminal_session_profile_stats before_detach =
        selection_session->profile_stats();
    ok &= check(selection_backend->emit_output(QByteArrayLiteral("\x1b[?2026hhidden")),
        "selection profile counter session enters synchronized output");
    selection_session->detach_selection_visual_attachment();
    const term::Terminal_session_profile_stats after_detach =
        selection_session->profile_stats();

    ok &= check(after_detach.render_snapshot_requests ==
            before_detach.render_snapshot_requests + 1U,
        "selection-derived blocked snapshot counted a render snapshot request");
    ok &= check(after_detach.render_snapshots_constructed ==
            before_detach.render_snapshots_constructed + 1U,
        "selection-derived blocked snapshot counted a constructed snapshot");
    ok &= check(after_detach.render_snapshot_publications ==
            before_detach.render_snapshot_publications + 1U,
        "selection-derived blocked snapshot counted a publication");
    ok &= check(after_detach.full_snapshot_publications ==
            before_detach.full_snapshot_publications + 1U,
        "selection-derived blocked snapshot counted a full publication");
    ok &= check(after_detach.selection_snapshot_publications ==
            before_detach.selection_snapshot_publications + 1U,
        "selection-derived blocked snapshot counted a selection publication");
    ok &= check(after_detach.retained_snapshot_generation_count > 0U,
        "selection-derived blocked snapshot refreshed retained snapshot counters");
#endif

    return ok;
}

term::Terminal_render_snapshot coalescing_snapshot(
    int                            rows,
    int                            columns,
    std::uint64_t                  row_origin_generation,
    std::vector<term::Terminal_render_dirty_row_range> dirty_row_ranges)
{
    term::Terminal_render_snapshot snapshot;
    snapshot.grid_size.rows                  = rows;
    snapshot.grid_size.columns               = columns;
    snapshot.viewport.active_buffer          = term::Terminal_buffer_id::PRIMARY;
    snapshot.viewport.visible_rows           = rows;
    snapshot.viewport.scrollback_rows        = 0;
    snapshot.viewport.offset_from_tail       = 0;
    snapshot.metadata.row_origin_generation  = row_origin_generation;
    snapshot.dirty_row_ranges                = std::move(dirty_row_ranges);
    return snapshot;
}

bool dirty_ranges_equal(
    const std::vector<term::Terminal_render_dirty_row_range>& left,
    const std::vector<term::Terminal_render_dirty_row_range>& right)
{
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i].first_row != right[i].first_row ||
            left[i].row_count != right[i].row_count)
        {
            return false;
        }
    }

    return true;
}

bool check_invalid_row_content(
    const term::Terminal_render_snapshot_row_content& row,
    int                                               expected_row,
    const char*                                       label)
{
    bool ok = true;
    ok &= check(!row.valid(), label);
    ok &= check(row.row() == expected_row, label);
    ok &= check(row.cell_count() == 0U && row.begin() == row.end(), label);
    ok &= check(row.cell_at(0) == nullptr, label);
    ok &= check(row.provenance_or_null() == nullptr, label);
    ok &= check(!row.dirty(), label);
    ok &= check(row.text(0, row.column_count(), false).isEmpty(), label);
    return ok;
}

bool test_dirty_row_coalescing_unifies_on_stricter_row_identity()
{
    bool ok = true;

    // AGREE case: matching grid + viewport + row_origin_generation with distinct
    // dirty ranges coalesces to the union of both range sets.
    const term::Terminal_render_snapshot agree_previous =
        coalescing_snapshot(10, 80, 7U, {{1, 1}, {5, 1}});
    const term::Terminal_render_snapshot agree_current =
        coalescing_snapshot(10, 80, 7U, {{2, 1}, {8, 1}});
    const term::Terminal_render_snapshot agree_result =
        term::snapshot_with_coalesced_dirty_rows(agree_previous, agree_current);
    ok &= check(
        dirty_ranges_equal(agree_result.dirty_row_ranges, {{1, 2}, {5, 1}, {8, 1}}),
        "matching identity space coalesces dirty rows into their union");

    // DIVERGENT case (the behavior the unification changes): identical grid +
    // viewport but a different row_origin_generation now falls back to a single
    // full-viewport range under the stricter canonical predicate.
    const term::Terminal_render_snapshot diverging_previous =
        coalescing_snapshot(10, 80, 7U, {{1, 1}});
    const term::Terminal_render_snapshot diverging_current =
        coalescing_snapshot(10, 80, 8U, {{2, 1}});
    const term::Terminal_render_snapshot diverging_result =
        term::snapshot_with_coalesced_dirty_rows(diverging_previous, diverging_current);
    ok &= check(
        dirty_ranges_equal(diverging_result.dirty_row_ranges, {{0, 10}}),
        "differing row_origin_generation forces a full-viewport repaint");

    // Existing fallbacks remain: differing grid size and differing viewport each
    // still produce a full repaint.
    const term::Terminal_render_snapshot grid_previous =
        coalescing_snapshot(10, 80, 7U, {{1, 1}});
    const term::Terminal_render_snapshot grid_current =
        coalescing_snapshot(12, 80, 7U, {{2, 1}});
    const term::Terminal_render_snapshot grid_result =
        term::snapshot_with_coalesced_dirty_rows(grid_previous, grid_current);
    ok &= check(
        dirty_ranges_equal(grid_result.dirty_row_ranges, {{0, 12}}),
        "differing grid size forces a full-viewport repaint");

    term::Terminal_render_snapshot viewport_previous =
        coalescing_snapshot(10, 80, 7U, {{1, 1}});
    viewport_previous.viewport.offset_from_tail = 4;
    const term::Terminal_render_snapshot viewport_current =
        coalescing_snapshot(10, 80, 7U, {{2, 1}});
    const term::Terminal_render_snapshot viewport_result =
        term::snapshot_with_coalesced_dirty_rows(viewport_previous, viewport_current);
    ok &= check(
        dirty_ranges_equal(viewport_result.dirty_row_ranges, {{0, 10}}),
        "differing viewport mapping forces a full-viewport repaint");

    // coalesced_dirty_row_snapshot_handle parity: the shared_ptr wrapper yields
    // the same coalesced dirty-row ranges as the value core for both the agree
    // and the divergent inputs.
    const std::shared_ptr<const term::Terminal_render_snapshot> agree_handle =
        term::coalesced_dirty_row_snapshot_handle(agree_previous, agree_current);
    ok &= check(
        agree_handle != nullptr &&
        dirty_ranges_equal(agree_handle->dirty_row_ranges, agree_result.dirty_row_ranges),
        "handle wrapper matches the value core on the agree case");

    const std::shared_ptr<const term::Terminal_render_snapshot> diverging_handle =
        term::coalesced_dirty_row_snapshot_handle(diverging_previous, diverging_current);
    ok &= check(
        diverging_handle != nullptr &&
        dirty_ranges_equal(diverging_handle->dirty_row_ranges, diverging_result.dirty_row_ranges),
        "handle wrapper matches the value core on the divergent case");

    return ok;
}

bool test_row_content_view_matches_flat_snapshot_representation()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model({4, 12});
    model.ingest(
        QByteArrayLiteral("\x1b[?1000h")
        + QByteArrayLiteral("\x1b]8;id=main;https://example.test\x1b\\")
        + QByteArrayLiteral("\x1b[31mA\x1b[1;5HZ\x1b[0m")
        + QByteArrayLiteral("\x1b]8;;\x1b\\")
        + QByteArrayLiteral("\x1b[2;1H")
        + QStringLiteral("\u754c").toUtf8()
        + QByteArrayLiteral("B\x1b[3;3HC"));

    const term::Terminal_retained_line_provenance first_line =
        model.retained_line_provenance_for_testing(term::Terminal_buffer_id::PRIMARY, 0);
    const term::Terminal_retained_line_provenance second_line =
        model.retained_line_provenance_for_testing(term::Terminal_buffer_id::PRIMARY, 1);

    term::Terminal_render_snapshot_request request = request_for_model(model, 220U);
    request.row_origin_generation            = 77U;
    request.processed_backend_callback_epoch = 23U;
    request.dirty_rows                        = {0, 2};
    request.cursor_shape                      = term::Terminal_cursor_shape::UNDERLINE;
    request.cursor_blink_enabled              = false;
    request.backend_geometry_in_sync          = false;
    request.visual_bell_active                = true;
    request.mouse_reporting_mode_changed      = true;
    request.ime_preedit.text                  = QStringLiteral("ime");
    request.ime_preedit.cursor_position       = 2;
    request.ime_preedit.active                = true;
    request.selections.push_back({
        {{0, 0}, {1, 3}, term::Terminal_selection_mode::NORMAL},
        {
            term::terminal_selection_line_lease_from_retained_identity(
                0, first_line.retained_line_id, first_line.content_generation),
            term::terminal_selection_line_lease_from_retained_identity(
                1, second_line.retained_line_id, second_line.content_generation),
        },
    });

    const term::Terminal_render_snapshot snapshot = model.render_snapshot(request);
    const term::Terminal_render_snapshot_row_content_view rows(snapshot);
    ok &= check(term::validate_render_snapshot(snapshot).status ==
            term::Terminal_render_snapshot_status::OK,
        "row-content view validates the rich flat snapshot");
    const std::vector<term::Terminal_render_cell> direct_materialized =
        rows.materialize_flat_cells(
            term::Terminal_render_snapshot_materialization_reason::ROW_VIEW_PARITY_TEST);
    ok &= check(rows.row_count() == snapshot.grid_size.rows &&
        rows.column_count() == snapshot.grid_size.columns,
        "row-content view exposes snapshot grid dimensions");
    ok &= check(direct_materialized.size() == snapshot.cells.size() &&
        rows.materialized_flat_cells_match_snapshot(
            term::Terminal_render_snapshot_materialization_reason::ROW_VIEW_PARITY_TEST),
        "row-content view materializes the same row-major flat cells");

    std::size_t iterated_cell_count = 0U;
    for (const term::Terminal_render_snapshot_row_content row : rows) {
        iterated_cell_count += row.cell_count();
        for (const term::Terminal_render_cell& cell : row) {
            ok &= check(cell.position.row == row.row(),
                "row iterator yields cells from the row it names");
            ok &= check(rows.cell_at(row.row(), cell.position.column) == &cell,
                "row-content cell lookup returns the flat cell address");
        }
    }
    ok &= check(iterated_cell_count == snapshot.cells.size(),
        "row-content iteration covers every flat cell exactly once");

    ok &= check(rows.row_at(0).cell_count() == 2U &&
        rows.row_at(0).cell_at(0) != nullptr &&
        rows.row_at(0).cell_at(1) == nullptr &&
        rows.row_at(0).cell_at(-1) == nullptr &&
        rows.row_at(0).cell_at(snapshot.grid_size.columns) == nullptr &&
        rows.row_at(0).cell_at(4) != nullptr,
        "row-content cell lookup preserves sparse and out-of-range missing-cell positions");
    ok &= check(rows.row_at(0).text(0, 5, false) == QStringLiteral("A   Z"),
        "row-content text extraction treats missing cells as spaces");
    ok &= check(rows.row_at(0).text(0, snapshot.grid_size.columns, true) ==
            QStringLiteral("A   Z"),
        "row-content text extraction trims trailing missing-cell spaces");
    ok &= check(rows.row_at(3).cell_count() == 0U &&
        rows.row_at(3).text(0, snapshot.grid_size.columns, false) ==
            QString(snapshot.grid_size.columns, QLatin1Char(' ')),
        "row-content text extraction represents a blank row as spaces");
    ok &= check(rows.row_at(3).text(0, snapshot.grid_size.columns, true).isEmpty(),
        "row-content trimmed text extraction keeps blank rows empty");
    ok &= check(rows.row_at(1).cell_at(0) != nullptr &&
        rows.row_at(1).cell_at(1) != nullptr &&
        rows.row_at(1).cell_at(1)->wide_continuation,
        "row-content cell lookup exposes wide continuations");
    ok &= check(rows.row_at(1).text(0, 3, false) == QStringLiteral("\u754cB"),
        "row-content text extraction skips wide continuations");

    const std::vector<const term::Terminal_render_cell*> flat_cells_by_position =
        flat_cells_by_position_for_fixture(snapshot);
    for (int row = 0; row < snapshot.grid_size.rows; ++row) {
        const QString flat_text = selected_text_from_flat_fixture_row(
            flat_cells_by_position,
            snapshot.grid_size,
            row,
            0,
            snapshot.grid_size.columns,
            false);
        const QString row_view_text = term::selected_text_from_render_snapshot_row(
            rows.row_at(row),
            0,
            snapshot.grid_size.columns,
            false);
        ok &= check(row_view_text == flat_text,
            "row-content full-row text extraction matches flat helper");
    }

    const term::Terminal_selection_range selection = {
        {0, 0},
        {1, 3},
        term::Terminal_selection_mode::NORMAL,
    };
    const term::Terminal_selection_result flat_selection =
        selected_text_from_flat_snapshot_baseline(snapshot, selection);
    const term::Terminal_selection_result row_view_selection =
        term::selected_text_from_render_snapshot(snapshot, selection);
    ok &= check(flat_selection.code == term::Terminal_selection_result_code::OK &&
        row_view_selection.code == flat_selection.code &&
        row_view_selection.text == flat_selection.text &&
        row_view_selection.text == QStringLiteral("A   Z\n\u754cB"),
        "row-content selection extraction matches flat selection helper");

    term::Terminal_screen_model scroll_model = make_model({3, 12});
    scroll_model.ingest(QByteArrayLiteral("zero\r\none\r\ntwo\r\nthree\r\nfour"));
    ok &= check(scroll_model.scrollback_size() > 0,
        "scrolled row-content selection fixture has scrollback");
    const term::Terminal_render_snapshot scroll_snapshot =
        scroll_model.render_snapshot(request_for_model(scroll_model, 221U, 1));
    const int scrolled_first_visible_row =
        term::render_snapshot_first_visible_logical_row(scroll_snapshot);
    ok &= check(scrolled_first_visible_row > 0,
        "scrolled row-content selection fixture has a nonzero first visible logical row");
    const term::Terminal_selection_range scrolled_selection = {
        {scrolled_first_visible_row,     0},
        {scrolled_first_visible_row + 1, 4},
        term::Terminal_selection_mode::NORMAL,
    };
    const term::Terminal_selection_result scrolled_flat_selection =
        selected_text_from_flat_snapshot_baseline(scroll_snapshot, scrolled_selection);
    const term::Terminal_selection_result scrolled_row_view_selection =
        term::selected_text_from_render_snapshot(
            scroll_snapshot,
            scrolled_selection);
    ok &= check(scrolled_flat_selection.code == term::Terminal_selection_result_code::OK &&
        scrolled_row_view_selection.code == scrolled_flat_selection.code &&
        scrolled_row_view_selection.text == scrolled_flat_selection.text,
        "row-content selection extraction matches flat selection on a scrolled viewport");

    const term::Terminal_render_line_provenance* first_provenance =
        rows.provenance_at(0);
    const term::Terminal_render_line_provenance* second_provenance =
        rows.provenance_at(1);
    ok &= check(first_provenance == &snapshot.visible_line_provenance[0] &&
        second_provenance == &snapshot.visible_line_provenance[1],
        "row-content view exposes row provenance descriptors");
    ok &= check(first_provenance != nullptr &&
        first_provenance->logical_row        == 0 &&
        first_provenance->retained_line_id   == first_line.retained_line_id &&
        first_provenance->content_generation == first_line.content_generation &&
        first_provenance->content_stamp_ms   == first_line.content_stamp_ms,
        "row-content view exposes exact first-row provenance fields");
    ok &= check(second_provenance != nullptr &&
        second_provenance->logical_row        == 1 &&
        second_provenance->retained_line_id   == second_line.retained_line_id &&
        second_provenance->content_generation == second_line.content_generation &&
        second_provenance->content_stamp_ms   == second_line.content_stamp_ms,
        "row-content view exposes exact second-row provenance fields");
    ok &= check(rows.provenance_at(-1) == nullptr &&
        rows.provenance_at(rows.row_count()) == nullptr,
        "row-content provenance lookup rejects out-of-range rows");
    ok &= check(rows.row_at(0).dirty() && !rows.row_at(1).dirty() && rows.row_at(2).dirty(),
        "row-content view exposes dirty-row membership");
    ok &= check(rows.dirty_row_ranges().size() == 2U &&
        rows.dirty_row_ranges()[0].first_row == 0 &&
        rows.dirty_row_ranges()[0].row_count == 1 &&
        rows.dirty_row_ranges()[1].first_row == 2 &&
        rows.dirty_row_ranges()[1].row_count == 1,
        "row-content view exposes exact dirty ranges");
    ok &= check(rows.cell_at(-1, 0) == nullptr &&
        rows.cell_at(rows.row_count(), 0) == nullptr,
        "row-content cell lookup rejects out-of-range rows");
    ok &= check(rows.cursor().shape == term::Terminal_cursor_shape::UNDERLINE &&
        rows.cursor().position.row == 2 &&
        rows.cursor().position.column == 3 &&
        rows.cursor().visible &&
        !rows.cursor().blink_enabled,
        "row-content view exposes cursor metadata");
    ok &= check(rows.selection_spans().size() == 2U,
        "row-content view exposes selection metadata");
    if (rows.selection_spans().size() == 2U) {
        const term::Terminal_render_selection_span& first_span =
            rows.selection_spans()[0];
        const term::Terminal_render_selection_span& second_span =
            rows.selection_spans()[1];
        ok &= check(first_span.row == 0 &&
            first_span.first_column == 0 &&
            first_span.column_count == snapshot.grid_size.columns &&
            second_span.row == 1 &&
            second_span.first_column == 0 &&
            second_span.column_count == 3,
            "row-content view exposes exact selection span columns");
        ok &= check(first_span.source_range.start.row    == 0 &&
            first_span.source_range.start.column         == 0 &&
            first_span.source_range.end.row              == 1 &&
            first_span.source_range.end.column           == 3 &&
            first_span.source_range.mode == term::Terminal_selection_mode::NORMAL &&
            second_span.source_range.start.row           == 0 &&
            second_span.source_range.start.column        == 0 &&
            second_span.source_range.end.row             == 1 &&
            second_span.source_range.end.column          == 3 &&
            second_span.source_range.mode == term::Terminal_selection_mode::NORMAL,
            "row-content view exposes exact selection source ranges");
    }
    ok &= check(rows.ime_preedit().text == QStringLiteral("ime") &&
        rows.ime_preedit().cursor_position == 2 &&
        rows.ime_preedit().active,
        "row-content view exposes IME metadata");
    ok &= check(!rows.metadata().backend_geometry_in_sync &&
        rows.metadata().visual_bell_active &&
        rows.metadata().mouse_reporting_mode_changed &&
        rows.metadata().sequence == 220U &&
        rows.metadata().processed_backend_callback_epoch == 23U &&
        rows.metadata().row_origin_generation == 77U &&
        rows.modes().mouse_tracking == term::Terminal_mouse_tracking_mode::BUTTON,
        "row-content view exposes snapshot metadata");

    return ok;
}

bool test_row_content_view_malformed_lookup_behavior()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model({3, 12});
    model.ingest(
        QByteArrayLiteral("\x1b]8;id=main;https://example.test\x1b\\")
        + QByteArrayLiteral("A")
        + QStringLiteral("\u754c").toUtf8()
        + QByteArrayLiteral("B\x1b]8;;\x1b\\"));

    term::Terminal_render_snapshot_request request = request_for_model(model, 230U);
    const term::Terminal_retained_line_provenance first_line =
        model.retained_line_provenance_for_testing(term::Terminal_buffer_id::PRIMARY, 0);
    request.selections.push_back({
        {{0, 0}, {0, 3}, term::Terminal_selection_mode::NORMAL},
        {
            term::terminal_selection_line_lease_from_retained_identity(
                0, first_line.retained_line_id, first_line.content_generation),
        },
    });

    const term::Terminal_render_snapshot valid = model.render_snapshot(request);
    ok &= check(term::validate_render_snapshot(valid).status ==
            term::Terminal_render_snapshot_status::OK,
        "row-content malformed fixture starts from a valid snapshot");

    const term::Terminal_render_snapshot_row_content_view rows(valid);
    ok &= check_invalid_row_content(
        rows.row_at(-1),
        -1,
        "row-content direct row_at rejects negative rows");
    ok &= check_invalid_row_content(
        rows.row_at(rows.row_count()),
        rows.row_count(),
        "row-content direct row_at rejects the past-end row");
    ok &= check_invalid_row_content(
        rows.row_at(std::numeric_limits<int>::max()),
        std::numeric_limits<int>::max(),
        "row-content direct row_at rejects extreme positive rows");
    ok &= check(rows.cell_at(-1, 0) == nullptr &&
        rows.cell_at(std::numeric_limits<int>::max(), 0) == nullptr &&
        rows.provenance_at(-1) == nullptr &&
        rows.provenance_at(std::numeric_limits<int>::max()) == nullptr,
        "row-content nullable lookups reject invalid rows");

    term::Terminal_render_snapshot invalid_grid_size = valid;
    invalid_grid_size.grid_size.rows = -1;
    const term::Terminal_render_snapshot_row_content_view invalid_grid_rows(
        invalid_grid_size);
    std::size_t invalid_grid_iterated_rows = 0U;
    for (const term::Terminal_render_snapshot_row_content row : invalid_grid_rows) {
        static_cast<void>(row);
        ++invalid_grid_iterated_rows;
    }
    ok &= check(invalid_grid_rows.row_count() == 0 &&
        invalid_grid_iterated_rows == 0U &&
        invalid_grid_rows.cell_at(0, 0) == nullptr &&
        invalid_grid_rows.provenance_at(0) == nullptr,
        "row-content view iteration and nullable lookups reject negative row counts");
    ok &= check_invalid_row_content(
        invalid_grid_rows.row_at(0),
        0,
        "row-content direct row_at rejects rows when the snapshot row count is negative");

    term::Terminal_render_snapshot extra_out_of_range_cell = valid;
    extra_out_of_range_cell.cells.push_back(valid.cells.front());
    extra_out_of_range_cell.cells.back().position.row = valid.grid_size.rows;
    ok &= check(!term::Terminal_render_snapshot_row_content_view(extra_out_of_range_cell).
            materialized_flat_cells_match_snapshot(
                term::Terminal_render_snapshot_materialization_reason::ROW_VIEW_PARITY_TEST),
        "row-content materialization rejects out-of-range flat cells");

    return ok;
}

bool test_validate_render_snapshot_reports_wide_overlap_before_missing_continuation()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model({3, 12});
    model.ingest(QByteArrayLiteral("abcd"));
    const term::Terminal_render_snapshot valid =
        model.render_snapshot(request_for_model(model, 231U));
    ok &= check(term::validate_render_snapshot(valid).status ==
            term::Terminal_render_snapshot_status::OK,
        "wide-overlap status-order fixture starts from a valid snapshot");
    ok &= check(valid.cells.size() >= 3U,
        "wide-overlap status-order fixture has enough cells");

    if (valid.cells.size() >= 3U) {
        term::Terminal_render_snapshot wide_overlap = valid;
        term::Terminal_render_cell base             = valid.cells[0];
        term::Terminal_render_cell overlapping      = valid.cells[2];
        base.position                               = {0, 0};
        base.display_width                          = 3;
        base.wide_continuation                      = false;
        overlapping.position                        = {0, 2};
        overlapping.display_width                   = 1;
        overlapping.wide_continuation               = false;
        wide_overlap.cells                          = {base, overlapping};

        ok &= check(term::validate_render_snapshot(wide_overlap).status ==
                term::Terminal_render_snapshot_status::INVALID_CELL_OVERLAP,
            "validator reports a non-continuation inside a width-3 base as overlap");
    }

    return ok;
}

bool test_validate_render_snapshot_reports_wide_structure_before_style_mismatch()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model({3, 12});
    model.ingest(QByteArrayLiteral("abcd"));
    const term::Terminal_render_snapshot valid =
        model.render_snapshot(request_for_model(model, 233U));
    ok &= check(term::validate_render_snapshot(valid).status ==
            term::Terminal_render_snapshot_status::OK,
        "wide-structure status-order fixture starts from a valid snapshot");
    ok &= check(valid.cells.size() >= 3U,
        "wide-structure status-order fixture has enough cells");

    if (valid.cells.size() >= 3U) {
        term::Terminal_render_snapshot style_before_missing = valid;
        style_before_missing.styles.push_back(term::make_default_terminal_text_style());

        term::Terminal_render_cell base                     = valid.cells[0];
        term::Terminal_render_cell wrong_style_continuation = valid.cells[1];
        base.position                                       = {0, 0};
        base.display_width                                  = 3;
        base.wide_continuation                              = false;
        base.style_id                                       = term::k_default_terminal_style_id;
        wrong_style_continuation.position                   = {0, 1};
        wrong_style_continuation.text                       =
            term::Terminal_render_cell_text::empty();
        wrong_style_continuation.display_width              = 0;
        wrong_style_continuation.wide_continuation          = true;
        wrong_style_continuation.style_id                   =
            static_cast<term::Terminal_style_id>(
                style_before_missing.styles.size() - 1U);
        wrong_style_continuation.hyperlink_id               = base.hyperlink_id;
        wrong_style_continuation.text_category              =
            term::Terminal_render_cell_text_category::EMPTY;
        style_before_missing.cells                          = {base, wrong_style_continuation};

        ok &= check(term::validate_render_snapshot(style_before_missing).status ==
                term::Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION,
            "validator reports a missing width-3 continuation before wrong continuation style");

        term::Terminal_render_snapshot style_before_overlap = style_before_missing;
        term::Terminal_render_cell overlapping              = valid.cells[2];
        overlapping.position                                = {0, 2};
        overlapping.display_width                           = 1;
        overlapping.wide_continuation                       = false;
        style_before_overlap.cells                          =
            {base, wrong_style_continuation, overlapping};

        ok &= check(term::validate_render_snapshot(style_before_overlap).status ==
                term::Terminal_render_snapshot_status::INVALID_CELL_OVERLAP,
            "validator reports a non-continuation overlap before wrong continuation style");
    }

    return ok;
}

bool test_validate_render_snapshot_accepts_huge_sparse_empty_snapshot()
{
    bool ok = true;

    constexpr int max_grid_extent = std::numeric_limits<int>::max();
    term::Terminal_viewport_state viewport;
    viewport.visible_rows = max_grid_extent;

    const term::Terminal_render_snapshot snapshot =
        term::make_empty_render_snapshot({max_grid_extent, max_grid_extent}, viewport, 232U);
    const term::Terminal_render_snapshot_validation validation =
        term::validate_render_snapshot(snapshot);

    ok &= check(snapshot.cells.empty() &&
            validation.status == term::Terminal_render_snapshot_status::OK,
        "validator accepts an empty INT_MAX x INT_MAX sparse snapshot");

    return ok;
}

// Step 1 (reproduce-before-fix): drive the real live-content model producer over
// representative content and assert that snapshot.cells genuinely satisfy the
// row-major / column-ascending contract that row-content views rely on, before
// that contract is enforced by the validator. Each case both validates OK and
// passes the standalone ordering predicate.
bool test_real_model_snapshot_cells_are_row_major_column_ascending()
{
    bool ok = true;

    // Multi-row plain ASCII with wide (double-width) characters and sparse rows
    // that leave column gaps. The cursor address sequences create occupied cells
    // at non-contiguous columns so the snapshot exercises mid-row gaps.
    term::Terminal_screen_model ascii_model = make_model({4, 16});
    ascii_model.ingest(
        QByteArrayLiteral("alpha\r\n")
        + QByteArrayLiteral("\x1b[2;4Hbeta\r\n")
        + QStringLiteral("界世gamma").toUtf8()
        + QByteArrayLiteral("\r\n")
        + QByteArrayLiteral("\x1b[4;10Hx"));
    const term::Terminal_render_snapshot ascii_snapshot =
        ascii_model.render_snapshot(request_for_model(ascii_model, 200U));
    ok &= check(term::validate_render_snapshot(ascii_snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "real model ASCII+wide+gap snapshot validates");
    ok &= check(snapshot_cells_are_row_major_column_ascending(ascii_snapshot),
        "real model ASCII+wide+gap snapshot cells are row-major and column-ascending");
    ok &= check(ascii_snapshot.cells.size() > 1U,
        "real model ASCII+wide+gap snapshot emits multiple cells to order");

    // Scrollback offset: content tall enough to push rows into history, viewed at
    // a non-zero offset_from_tail so the snapshot mixes retained and resident rows.
    term::Terminal_screen_model scroll_model = make_model({3, 12});
    scroll_model.ingest(
        QByteArrayLiteral("one\r\ntwo\r\nthree\r\nfour\r\nfive"));
    ok &= check(scroll_model.scrollback_size() > 0,
        "scrollback ordering fixture has retained history");
    const term::Terminal_render_snapshot scroll_snapshot =
        scroll_model.render_snapshot(request_for_model(scroll_model, 201U, 1));
    ok &= check(term::validate_render_snapshot(scroll_snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "real model scrollback-offset snapshot validates");
    ok &= check(snapshot_cells_are_row_major_column_ascending(scroll_snapshot),
        "real model scrollback-offset snapshot cells are row-major and column-ascending");

    // Alternate buffer: switching to the alternate screen produces its own
    // resident-row snapshot, which must satisfy the same ordering contract.
    term::Terminal_screen_model alternate_model = make_model({3, 12});
    alternate_model.ingest(QByteArrayLiteral("PRIMARY\r\nROW2"));
    alternate_model.ingest(QByteArrayLiteral("\x1b[?1049h\x1b[2;3HALT"));
    const term::Terminal_render_snapshot alternate_snapshot =
        alternate_model.render_snapshot(request_for_model(alternate_model, 202U));
    ok &= check(alternate_snapshot.viewport.active_buffer ==
        term::Terminal_buffer_id::ALTERNATE,
        "alternate ordering fixture reports the alternate buffer");
    ok &= check(term::validate_render_snapshot(alternate_snapshot).status ==
        term::Terminal_render_snapshot_status::OK,
        "real model alternate-buffer snapshot validates");
    ok &= check(snapshot_cells_are_row_major_column_ascending(alternate_snapshot),
        "real model alternate-buffer snapshot cells are row-major and column-ascending");

    return ok;
}

// Step 3: the validator rejects an out-of-order snapshot with the distinct
// INVALID_CELL_ORDER status, while a real producer snapshot and the existing
// valid fixtures are not falsely rejected.
bool test_validate_render_snapshot_rejects_out_of_order_cells()
{
    bool ok = true;

    term::Terminal_screen_model model = make_model({3, 12});
    model.ingest(QByteArrayLiteral("abcd\r\nefgh\r\nijkl"));
    const term::Terminal_render_snapshot valid =
        model.render_snapshot(request_for_model(model, 210U));
    ok &= check(term::validate_render_snapshot(valid).status ==
        term::Terminal_render_snapshot_status::OK,
        "real model snapshot validates before order mutation");
    ok &= check(valid.cells.size() >= 4U,
        "order-rejection fixture has enough cells to reorder");

    // Swap two adjacent cells within the same row so columns decrease across the
    // boundary: a strict-order violation that is not a same-position overlap.
    term::Terminal_render_snapshot swapped_columns = valid;
    bool swapped_in_row = false;
    for (std::size_t i = 1U; i < swapped_columns.cells.size(); ++i) {
        if (swapped_columns.cells[i].position.row ==
                swapped_columns.cells[i - 1U].position.row &&
            swapped_columns.cells[i].position.column >
                swapped_columns.cells[i - 1U].position.column)
        {
            std::swap(swapped_columns.cells[i], swapped_columns.cells[i - 1U]);
            swapped_in_row = true;
            break;
        }
    }
    ok &= check(swapped_in_row,
        "order-rejection fixture found an adjacent same-row column pair to swap");
    ok &= check(term::validate_render_snapshot(swapped_columns).status ==
        term::Terminal_render_snapshot_status::INVALID_CELL_ORDER,
        "validator rejects a column-decreasing cell pair as INVALID_CELL_ORDER");

    // Move a later row's first cell ahead of an earlier row's cell so the row
    // index decreases across the boundary.
    term::Terminal_render_snapshot row_decrease = valid;
    bool moved_row = false;
    for (std::size_t i = 1U; i < row_decrease.cells.size(); ++i) {
        if (row_decrease.cells[i].position.row >
            row_decrease.cells[0].position.row)
        {
            const term::Terminal_render_cell later_row_cell = row_decrease.cells[i];
            row_decrease.cells.insert(row_decrease.cells.begin(), later_row_cell);
            moved_row = true;
            break;
        }
    }
    ok &= check(moved_row,
        "order-rejection fixture found a later-row cell to hoist");
    ok &= check(term::validate_render_snapshot(row_decrease).status ==
        term::Terminal_render_snapshot_status::INVALID_CELL_ORDER,
        "validator rejects a row-decreasing cell sequence as INVALID_CELL_ORDER");

    // The new status must be distinct from existing ones, and the unmutated
    // real-model snapshot must remain OK (no false rejection).
    ok &= check(term::Terminal_render_snapshot_status::INVALID_CELL_ORDER !=
            term::Terminal_render_snapshot_status::OK &&
        term::Terminal_render_snapshot_status::INVALID_CELL_ORDER !=
            term::Terminal_render_snapshot_status::INVALID_CELL_OVERLAP &&
        term::Terminal_render_snapshot_status::INVALID_CELL_ORDER !=
            term::Terminal_render_snapshot_status::INVALID_CELL_POSITION,
        "INVALID_CELL_ORDER is distinct from OK, overlap, and position statuses");
    ok &= check(term::validate_render_snapshot(valid).status ==
        term::Terminal_render_snapshot_status::OK,
        "validator does not falsely reject the in-order real-model snapshot");

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_owned_styled_wide_hyperlink_scrollback_snapshot();
    ok &= test_scrollback_wide_rows_are_repaired_on_resize();
    ok &= test_snapshot_rows_cover_primary_retained_and_alternate_sources();
    ok &= test_snapshot_cells_cache_text_category();
    ok &= test_snapshot_profile_counts_inline_single_bmp_cells();
    ok &= test_alternate_screen_hides_primary_scrollback();
    ok &= test_request_metadata_damage_selection_and_ime();
    ok &= test_selection_request_rejects_retained_line_row_reorder();
    ok &= test_dirty_rows_are_viewport_relative();
    ok &= test_model_snapshots_publish_visible_line_provenance();
    ok &= test_row_content_stamps_track_writes_and_survive_scrollback();
    ok &= test_row_content_stamps_survive_grid_resize();
    ok &= test_visible_line_provenance_validation();
    ok &= test_public_projection_scroll_snapshot_structural_validation();
    ok &= test_session_snapshot_handles_and_synchronized_release();
    ok &= test_snapshot_publication_generation_metadata();
    ok &= test_session_install_does_not_advance_rendered_publication();
    ok &= test_session_ime_overlay_does_not_clone_render_snapshot();
    ok &= test_backend_sync_metadata_publishes_after_same_grid_retry();
    ok &= test_resize_metadata_publication_respects_synchronized_output();
    ok &= test_synthetic_snapshots_preserve_or_suppress_line_provenance();
    ok &= test_session_profile_snapshot_publication_counters();
    ok &= test_dirty_row_coalescing_unifies_on_stricter_row_identity();
    ok &= test_row_content_view_matches_flat_snapshot_representation();
    ok &= test_row_content_view_malformed_lookup_behavior();
    ok &= test_validate_render_snapshot_reports_wide_overlap_before_missing_continuation();
    ok &= test_validate_render_snapshot_reports_wide_structure_before_style_mismatch();
    ok &= test_validate_render_snapshot_accepts_huge_sparse_empty_snapshot();
    ok &= test_real_model_snapshot_cells_are_row_major_column_ascending();
    ok &= test_validate_render_snapshot_rejects_out_of_order_cells();
    return ok ? 0 : 1;
}
