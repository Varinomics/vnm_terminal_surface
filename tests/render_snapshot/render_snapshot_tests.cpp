#include "helpers/test_check.h"
#include "vnm_terminal/internal/terminal_screen_model.h"
#include "vnm_terminal/internal/terminal_session.h"

#include <QByteArray>
#include <QSizeF>
#include <QString>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

term::Terminal_screen_model make_model(term::terminal_grid_size_t grid_size)
{
    return term::Terminal_screen_model({grid_size, 16, 8});
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
            text += cell.text;
        }
    }
    return text;
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
    request.dirty_rows                  = {5, 1, 2, 2, 3};
    request.cursor_shape                = term::Terminal_cursor_shape::BAR;
    request.cursor_blink_enabled        = false;
    request.backend_geometry_in_sync    = false;
    request.visual_bell_active          = true;
    request.ime_preedit.text            = QStringLiteral("ime");
    request.ime_preedit.cursor_position = 2;
    request.ime_preedit.active          = true;
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

}

int main()
{
    bool ok = true;
    ok &= test_owned_styled_wide_hyperlink_scrollback_snapshot();
    ok &= test_scrollback_wide_rows_are_repaired_on_resize();
    ok &= test_snapshot_rows_cover_primary_retained_and_alternate_sources();
    ok &= test_alternate_screen_hides_primary_scrollback();
    ok &= test_request_metadata_damage_selection_and_ime();
    ok &= test_selection_request_rejects_retained_line_row_reorder();
    ok &= test_dirty_rows_are_viewport_relative();
    ok &= test_model_snapshots_publish_visible_line_provenance();
    ok &= test_visible_line_provenance_validation();
    ok &= test_public_projection_scroll_snapshot_structural_validation();
    ok &= test_session_snapshot_handles_and_synchronized_release();
    ok &= test_session_ime_overlay_does_not_clone_render_snapshot();
    ok &= test_backend_sync_metadata_publishes_after_same_grid_retry();
    ok &= test_resize_metadata_publication_respects_synchronized_output();
    ok &= test_synthetic_snapshots_preserve_or_suppress_line_provenance();
    return ok ? 0 : 1;
}
