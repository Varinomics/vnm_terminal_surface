#include "helpers/test_check.h"
#include "helpers/primary_backing_test_config.h"
#include "vnm_terminal/internal/terminal_session.h"

#include <QByteArray>
#include <QSizeF>
#include <QString>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;
using vnm_terminal::test_helpers::recovery_disabled_primary_backing_session_config;

class Replay_backend final : public term::Terminal_backend
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

        m_callbacks = std::move(callbacks);
        m_running = true;
        return term::backend_accept();
    }

    term::Terminal_backend_result write(QByteArray bytes) override
    {
        write_requests.push_back(std::move(bytes));
        return term::backend_accept();
    }

    term::Terminal_backend_result resize(term::Terminal_backend_resize_request request) override
    {
        resize_requests.push_back(request);
        return term::backend_accept();
    }

    term::Terminal_backend_result set_output_paused(bool paused) override
    {
        pause_requests.push_back(paused);
        return term::backend_accept();
    }

    term::Terminal_backend_result interrupt() override
    {
        m_running = false;
        return term::backend_accept();
    }

    term::Terminal_backend_result terminate() override
    {
        m_running = false;
        return term::backend_accept();
    }

    bool emit_output(QByteArray bytes)
    {
        if (!m_running || !m_callbacks.output_received) {
            return false;
        }

        m_callbacks.output_received(std::move(bytes));
        return true;
    }

    std::vector<QByteArray> write_requests;
    std::vector<term::Terminal_backend_resize_request> resize_requests;
    std::vector<bool>                                  pause_requests;

private:
    bool                             m_running = false;
    term::Terminal_backend_callbacks m_callbacks;
};

term::Terminal_launch_config launch_config(term::terminal_grid_size_t grid_size)
{
    term::Terminal_launch_config config;
    config.argv              = {QStringLiteral("lazy-snapshot-stress-replay")};
    config.working_directory = QStringLiteral("C:/workspace");
    config.initial_grid_size = grid_size;
    return config;
}

std::unique_ptr<term::Terminal_session> make_session(
    Replay_backend*&           backend,
    term::terminal_grid_size_t grid_size)
{
    auto owned_backend = std::make_unique<Replay_backend>();
    backend = owned_backend.get();

    term::Terminal_session_config config =
        recovery_disabled_primary_backing_session_config();
    config.trace_notification_limit                 = 128U;
    config.trace_resize_limit                       = 32U;
    config.scrollback_limit                         = 24;

    auto session = std::make_unique<term::Terminal_session>(
        std::move(owned_backend),
        config);

    const term::Terminal_session_result start_result =
        session->start(launch_config(grid_size));
    if (start_result.code != term::Terminal_session_result_code::ACCEPTED) {
        backend = nullptr;
    }

    return session;
}

std::string message(std::string_view label, std::string_view suffix)
{
    std::string out(label);
    out += ": ";
    out += suffix;
    return out;
}

bool check_labeled(
    bool             condition,
    std::string_view label,
    std::string_view suffix)
{
    return check(condition, message(label, suffix));
}

std::string fallback_reason_key(term::Terminal_lazy_snapshot_fallback_reason reason)
{
    if (reason == term::Terminal_lazy_snapshot_fallback_reason::NONE) {
        return "none";
    }

    for (const term::terminal_lazy_snapshot_fallback_reason_descriptor_t& descriptor :
        term::terminal_lazy_snapshot_fallback_reason_descriptors())
    {
        if (descriptor.reason == reason) {
            return descriptor.key;
        }
    }
    return "unknown";
}

bool expect_lazy_fallback(
    const term::Terminal_session_lazy_snapshot_composer_result& result,
    term::Terminal_lazy_snapshot_fallback_reason                expected,
    std::string_view                                            label)
{
    std::string suffix("fallback expected=");
    suffix += fallback_reason_key(expected);
    suffix += " actual=";
    suffix += fallback_reason_key(result.fallback_reason);
    return check_labeled(
        !result.eligible && result.fallback_reason == expected,
        label,
        suffix);
}

QByteArray cursor_to(int row, int column)
{
    return QStringLiteral("\x1b[%1;%2H").arg(row).arg(column).toLatin1();
}

QByteArray clear_row_at(int row)
{
    return cursor_to(row, 1) + QByteArrayLiteral("\x1b[2K");
}

QByteArray row_write(int row, QByteArray text)
{
    return clear_row_at(row) + std::move(text);
}

QByteArray numbered_scrollback_prefix()
{
    QByteArray output;
    for (int index = 0; index < 10; ++index) {
        output += QStringLiteral("scroll%1\r\n")
            .arg(index, 2, 10, QLatin1Char('0'))
            .toLatin1();
    }
    return output;
}

QByteArray rich_visible_baseline()
{
    QByteArray output = numbered_scrollback_prefix();
    output += row_write(1, QByteArrayLiteral("\x1b[31;1mSTYLE\x1b[0m"));
    output += row_write(2, QByteArrayLiteral("wide \xE7\x95\x8C row"));
    output += row_write(3, QByteArrayLiteral("comb cafe\xCC\x81 row"));
    output += row_write(
        4,
        QByteArrayLiteral(
            "\x1b]8;id=alpha;https://alpha.example\x1b\\LINK\x1b]8;;\x1b\\"));
    output += clear_row_at(5);
    output += row_write(6, QByteArrayLiteral("SPARSE"));
    output += row_write(7, QByteArrayLiteral("TAILA"));
    output += row_write(8, QByteArrayLiteral("TAILB"));
    return output;
}

bool color_states_match(
    const term::Terminal_color_state& left,
    const term::Terminal_color_state& right)
{
    return
        left.default_foreground_rgba == right.default_foreground_rgba &&
        left.default_background_rgba == right.default_background_rgba &&
        left.cursor_rgba             == right.cursor_rgba             &&
        left.palette_rgba            == right.palette_rgba;
}

bool viewport_states_match(
    const term::Terminal_viewport_state& left,
    const term::Terminal_viewport_state& right)
{
    return
        left.active_buffer                   == right.active_buffer                   &&
        left.scrollback_rows                 == right.scrollback_rows                 &&
        left.visible_rows                    == right.visible_rows                    &&
        left.offset_from_tail                == right.offset_from_tail                &&
        left.follow_tail                     == right.follow_tail                     &&
        left.alternate_screen_scroll_policy == right.alternate_screen_scroll_policy;
}

bool mode_states_match(
    const term::Terminal_mode_state& left,
    const term::Terminal_mode_state& right)
{
    return
        left.application_cursor_keys == right.application_cursor_keys &&
        left.reverse_video           == right.reverse_video           &&
        left.origin_mode             == right.origin_mode             &&
        left.autowrap                == right.autowrap                &&
        left.cursor_visible          == right.cursor_visible          &&
        left.mouse_tracking          == right.mouse_tracking          &&
        left.focus_reporting         == right.focus_reporting         &&
        left.sgr_mouse_encoding      == right.sgr_mouse_encoding      &&
        left.alternate_scroll        == right.alternate_scroll        &&
        left.bracketed_paste         == right.bracketed_paste         &&
        left.synchronized_output     == right.synchronized_output;
}

bool cursors_match(
    const term::Terminal_render_cursor& left,
    const term::Terminal_render_cursor& right)
{
    return
        left.position      == right.position      &&
        left.shape         == right.shape         &&
        left.visible       == right.visible       &&
        left.blink_enabled == right.blink_enabled;
}

bool metadata_match(
    const term::Terminal_render_metadata& left,
    const term::Terminal_render_metadata& right)
{
    return
        left.sequence                     == right.sequence                     &&
        left.row_origin_generation        == right.row_origin_generation        &&
        left.backend_geometry_in_sync     == right.backend_geometry_in_sync     &&
        left.visual_bell_active           == right.visual_bell_active           &&
        left.mouse_reporting_mode_changed == right.mouse_reporting_mode_changed;
}

bool provenance_matches_exactly(
    const term::Terminal_render_line_provenance& left,
    const term::Terminal_render_line_provenance& right)
{
    return
        left.logical_row        == right.logical_row        &&
        left.retained_line_id   == right.retained_line_id   &&
        left.content_generation == right.content_generation &&
        left.content_stamp_ms   == right.content_stamp_ms;
}

bool dirty_ranges_match(
    const std::vector<term::Terminal_render_dirty_row_range>& left,
    const std::vector<term::Terminal_render_dirty_row_range>& right)
{
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (left[index].first_row != right[index].first_row ||
            left[index].row_count != right[index].row_count)
        {
            return false;
        }
    }
    return true;
}

bool selection_spans_match(
    const std::vector<term::Terminal_render_selection_span>& left,
    const std::vector<term::Terminal_render_selection_span>& right)
{
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t index = 0U; index < left.size(); ++index) {
        const term::Terminal_render_selection_span& left_span  = left[index];
        const term::Terminal_render_selection_span& right_span = right[index];
        if (left_span.source_range != right_span.source_range ||
            left_span.row          != right_span.row          ||
            left_span.first_column != right_span.first_column ||
            left_span.column_count != right_span.column_count)
        {
            return false;
        }
    }
    return true;
}

bool hyperlinks_match(
    const std::vector<term::Terminal_render_hyperlink_metadata>& left,
    const std::vector<term::Terminal_render_hyperlink_metadata>& right)
{
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (left[index].hyperlink_id != right[index].hyperlink_id ||
            left[index].identity_key != right[index].identity_key ||
            left[index].uri          != right[index].uri)
        {
            return false;
        }
    }
    return true;
}

bool cells_match_at(
    const term::Terminal_render_cell* left,
    const term::Terminal_render_cell* right)
{
    if (left == nullptr || right == nullptr) {
        return left == right;
    }

    return term::render_snapshot_cells_equal(*left, *right);
}

bool row_observations_match(
    const term::Terminal_render_snapshot_row_content& full_row,
    const term::Terminal_render_snapshot_row_content& lazy_row,
    std::string_view                                  label)
{
    bool ok = true;
    ok &= check_labeled(full_row.valid() == lazy_row.valid(), label, "row validity");
    ok &= check_labeled(full_row.row() == lazy_row.row(), label, "row index");
    ok &= check_labeled(
        full_row.column_count() == lazy_row.column_count(),
        label,
        "row column count");
    ok &= check_labeled(
        full_row.cell_count() == lazy_row.cell_count(),
        label,
        "row cell count");
    ok &= check_labeled(full_row.dirty() == lazy_row.dirty(), label, "row dirty flag");

    const term::Terminal_render_line_provenance* full_provenance =
        full_row.provenance_or_null();
    const term::Terminal_render_line_provenance* lazy_provenance =
        lazy_row.provenance_or_null();
    ok &= check_labeled(
        (full_provenance == nullptr) == (lazy_provenance == nullptr),
        label,
        "row provenance presence");
    if (full_provenance != nullptr && lazy_provenance != nullptr) {
        ok &= check_labeled(
            provenance_matches_exactly(*full_provenance, *lazy_provenance),
            label,
            "row provenance fields");
    }

    auto full_cell = full_row.begin();
    auto lazy_cell = lazy_row.begin();
    for (; full_cell != full_row.end() && lazy_cell != lazy_row.end();
        ++full_cell, ++lazy_cell)
    {
        ok &= check_labeled(
            term::render_snapshot_cells_equal(*full_cell, *lazy_cell),
            label,
            "row cell stream");
    }

    for (int column = 0; column < full_row.column_count(); ++column) {
        ok &= check_labeled(
            cells_match_at(full_row.cell_at(column), lazy_row.cell_at(column)),
            label,
            "row cell lookup");
    }
    return ok;
}

bool snapshot_observations_match(
    const term::Terminal_render_snapshot& full,
    const term::Terminal_render_snapshot& lazy,
    std::string_view                      label)
{
    bool ok = true;
    ok &= check_labeled(lazy.lazy_row_payloads != nullptr, label, "lazy payload presence");
    ok &= check_labeled(lazy.cells.empty(), label, "lazy snapshot has no flat cells");
    ok &= check_labeled(
        lazy.cells.capacity() == 0U,
        label,
        "lazy snapshot retains no flat cell capacity");
    ok &= check_labeled(
        term::validate_render_snapshot(full).status ==
            term::Terminal_render_snapshot_status::OK,
        label,
        "full snapshot validates");
    ok &= check_labeled(
        term::validate_render_snapshot(lazy).status ==
            term::Terminal_render_snapshot_status::OK,
        label,
        "lazy snapshot validates");
    ok &= check_labeled(full.basis == lazy.basis, label, "snapshot basis");
    ok &= check_labeled(full.purpose == lazy.purpose, label, "snapshot purpose");
    ok &= check_labeled(
        term::grid_sizes_match(full.grid_size, lazy.grid_size),
        label,
        "grid size");
    ok &= check_labeled(
        viewport_states_match(full.viewport, lazy.viewport),
        label,
        "viewport state");
    ok &= check_labeled(
        color_states_match(full.color_state, lazy.color_state),
        label,
        "color state");
    ok &= check_labeled(full.styles == lazy.styles, label, "style table");
    ok &= check_labeled(
        dirty_ranges_match(full.dirty_row_ranges, lazy.dirty_row_ranges),
        label,
        "dirty row ranges");
    ok &= check_labeled(
        hyperlinks_match(full.hyperlinks, lazy.hyperlinks),
        label,
        "hyperlink metadata");
    ok &= check_labeled(cursors_match(full.cursor, lazy.cursor), label, "cursor");
    ok &= check_labeled(
        term::same_ime_preedit_state(full.ime_preedit, lazy.ime_preedit),
        label,
        "IME preedit");
    ok &= check_labeled(
        selection_spans_match(full.selection_spans, lazy.selection_spans),
        label,
        "selection spans");
    ok &= check_labeled(metadata_match(full.metadata, lazy.metadata), label, "metadata");
    ok &= check_labeled(mode_states_match(full.modes, lazy.modes), label, "mode state");

    const term::Terminal_render_snapshot_row_content_view full_rows(full);
    const term::Terminal_render_snapshot_row_content_view lazy_rows(lazy);
    ok &= check_labeled(
        full_rows.row_count() == lazy_rows.row_count(),
        label,
        "row count");
    ok &= check_labeled(
        full_rows.column_count() == lazy_rows.column_count(),
        label,
        "column count");
    ok &= check_labeled(
        full_rows.cell_count() == lazy_rows.cell_count(),
        label,
        "view cell count");

    for (int row = 0; row < full_rows.row_count(); ++row) {
        ok &= check_labeled(
            full_rows.row_text(row, 0, full.grid_size.columns, false) ==
                lazy_rows.row_text(row, 0, lazy.grid_size.columns, false),
            label,
            "untrimmed row text");
        ok &= check_labeled(
            full_rows.row_text(row, 0, full.grid_size.columns, true) ==
                lazy_rows.row_text(row, 0, lazy.grid_size.columns, true),
            label,
            "trimmed row text");
        ok &= row_observations_match(full_rows.row_at(row), lazy_rows.row_at(row), label);
    }

    return ok;
}

bool expect_rich_snapshot_features(
    const term::Terminal_render_snapshot& snapshot,
    std::string_view                      label)
{
    bool ok = true;
    const term::Terminal_render_snapshot_row_content_view rows(snapshot);
    ok &= check_labeled(snapshot.viewport.scrollback_rows > 0, label, "scrollback exists");
    ok &= check_labeled(
        rows.row_text(0, 0, snapshot.grid_size.columns, true) ==
            QStringLiteral("STYLE"),
        label,
        "styled row text");
    ok &= check_labeled(snapshot.styles.size() > 1U, label, "style metadata exists");

    const term::Terminal_render_cell* styled_cell = rows.row_at(0).cell_at(0);
    ok &= check_labeled(
        styled_cell != nullptr &&
            styled_cell->style_id != term::k_default_terminal_style_id,
        label,
        "styled cell keeps non-default style id");

    const QString wide_glyph = QString::fromUtf8(QByteArrayLiteral("\xE7\x95\x8C"));
    ok &= check_labeled(
        rows.row_text(1, 0, snapshot.grid_size.columns, true).contains(wide_glyph),
        label,
        "wide glyph row text");
    const term::Terminal_render_cell* wide_cell = rows.row_at(1).cell_at(5);
    const term::Terminal_render_cell* wide_continuation = rows.row_at(1).cell_at(6);
    ok &= check_labeled(
        wide_cell != nullptr &&
            wide_cell->display_width == 2 &&
            wide_continuation != nullptr &&
            wide_continuation->wide_continuation,
        label,
        "wide glyph cell structure");

    const QString combining_text =
        QString::fromUtf8(QByteArrayLiteral("comb cafe\xCC\x81 row"));
    ok &= check_labeled(
        rows.row_text(2, 0, snapshot.grid_size.columns, true) == combining_text,
        label,
        "combining-mark row text");

    const term::Terminal_render_cell* link_cell = rows.row_at(3).cell_at(0);
    ok &= check_labeled(
        link_cell != nullptr && link_cell->hyperlink_id != 0U,
        label,
        "hyperlink cell id");
    ok &= check_labeled(!snapshot.hyperlinks.empty(), label, "hyperlink metadata exists");

    bool found_hyperlink = false;
    for (const term::Terminal_render_hyperlink_metadata& hyperlink : snapshot.hyperlinks) {
        found_hyperlink =
            found_hyperlink ||
            (link_cell != nullptr &&
                hyperlink.hyperlink_id == link_cell->hyperlink_id &&
                hyperlink.uri == QByteArrayLiteral("https://alpha.example"));
    }
    ok &= check_labeled(found_hyperlink, label, "hyperlink metadata resolves");

    ok &= check_labeled(
        rows.row_text(4, 0, snapshot.grid_size.columns, true).isEmpty(),
        label,
        "EL blank row trims to empty");
    const term::Terminal_render_line_provenance* blank_provenance =
        rows.row_at(4).provenance_or_null();
    ok &= check_labeled(
        blank_provenance != nullptr && blank_provenance->retained_line_id != 0U,
        label,
        "EL blank row keeps provenance");
    return ok;
}

bool compose_and_compare_lazy_snapshot(
    term::Terminal_session&                               session,
    std::shared_ptr<const term::Terminal_render_snapshot> previous,
    const term::Terminal_render_snapshot&                 current,
    std::string_view                                      label)
{
    bool ok = true;
    const term::Terminal_session_lazy_snapshot_composer_result parity =
        session.compose_lazy_render_snapshot_for_testing(previous, current);
    ok &= check_labeled(parity.eligible, label, "parity composer is eligible");
    ok &= check_labeled(
        parity.lazy_snapshot.has_value(),
        label,
        "parity composer returned a lazy snapshot");
    ok &= check_labeled(
        parity.materialization_matches_full_snapshot,
        label,
        "parity composer reports full materialization match");
    ok &= check_labeled(
        parity.consumer_materialization_calls == 1U,
        label,
        "parity composer records one comparison materialization");
    ok &= check_labeled(
        parity.dirty_rows_visible > 0U &&
            parity.previous_snapshot_borrowed_rows > 0U,
        label,
        "parity composer owns dirty rows and borrows clean rows");
    ok &= check_labeled(
        parity.previous_snapshot_borrow_candidate_rows ==
                static_cast<std::uint64_t>(current.grid_size.rows) &&
            parity.previous_snapshot_borrowed_rows + parity.dirty_rows_visible ==
                parity.previous_snapshot_borrow_candidate_rows,
        label,
        "parity composer reports complete previous-row borrow denominator");
    ok &= check_labeled(
        parity.producer_owned_rows <= parity.dirty_rows_visible &&
            parity.producer_materialized_rows <= parity.dirty_rows_visible &&
            parity.producer_cells_scanned <=
                parity.dirty_rows_visible *
                    static_cast<std::uint64_t>(current.grid_size.columns),
        label,
        "parity composer producer work is bounded by dirty visible rows");
    if (parity.lazy_snapshot.has_value()) {
        ok &= snapshot_observations_match(current, *parity.lazy_snapshot, label);
    }

    const term::Terminal_session_lazy_snapshot_composer_result candidate =
        session.compose_lazy_render_snapshot_for_benchmark_evidence(
            previous,
            current,
            term::Terminal_lazy_snapshot_evidence_mode::
                PUBLICATION_CANDIDATE_NO_MATERIALIZATION);
    ok &= check_labeled(candidate.eligible, label, "candidate composer is eligible");
    ok &= check_labeled(
        candidate.lazy_snapshot.has_value(),
        label,
        "candidate composer returned a lazy snapshot");
    ok &= check_labeled(
        !candidate.materialization_matches_full_snapshot &&
            candidate.consumer_materialization_calls == 0U &&
            candidate.consumer_materialization_rows  == 0U &&
            candidate.consumer_materialization_cells == 0U,
        label,
        "candidate composer avoids counted consumer materialization");
    if (candidate.lazy_snapshot.has_value()) {
        ok &= snapshot_observations_match(current, *candidate.lazy_snapshot, label);
    }
    return ok;
}

bool emit_output_and_compare(
    Replay_backend&                                      backend,
    term::Terminal_session&                              session,
    std::shared_ptr<const term::Terminal_render_snapshot>& previous,
    QByteArray                                           output,
    std::string_view                                     label)
{
    bool ok = true;
    ok &= check_labeled(backend.emit_output(std::move(output)), label, "backend emits output");
    const std::shared_ptr<const term::Terminal_render_snapshot> current =
        session.latest_render_snapshot_handle();
    ok &= check_labeled(current != nullptr, label, "current full snapshot exists");
    if (current == nullptr) {
        return false;
    }

    ok &= check_labeled(
        current->lazy_row_payloads == nullptr && !current->cells.empty(),
        label,
        "production snapshot remains fully materialized");
    ok &= compose_and_compare_lazy_snapshot(session, previous, *current, label);

    previous = current;
    session.mark_render_snapshot_synced(session.render_snapshot_generation());
    return ok;
}

std::optional<std::uint64_t> first_hyperlink_id(
    const term::Terminal_render_snapshot& snapshot)
{
    for (const term::Terminal_render_hyperlink_metadata& hyperlink : snapshot.hyperlinks) {
        if (hyperlink.hyperlink_id != 0U) {
            return hyperlink.hyperlink_id;
        }
    }
    return std::nullopt;
}

bool expect_hyperlink_namespace_mismatch_fallback(
    term::Terminal_session&                               session,
    const std::shared_ptr<const term::Terminal_render_snapshot>& previous,
    const term::Terminal_render_snapshot&                 current)
{
    bool ok = true;
    std::optional<std::uint64_t> source_hyperlink_id = first_hyperlink_id(current);
    ok &= check(source_hyperlink_id.has_value(),
        "hyperlink namespace mismatch fixture has a hyperlink id");
    if (!source_hyperlink_id.has_value()) {
        return false;
    }

    term::Terminal_render_snapshot remapped = current;
    const std::uint64_t target_hyperlink_id = *source_hyperlink_id + 100U;
    for (term::Terminal_render_hyperlink_metadata& hyperlink : remapped.hyperlinks) {
        if (hyperlink.hyperlink_id == *source_hyperlink_id) {
            hyperlink.hyperlink_id = target_hyperlink_id;
        }
    }
    for (term::Terminal_render_cell& cell : remapped.cells) {
        if (cell.hyperlink_id == *source_hyperlink_id) {
            cell.hyperlink_id = target_hyperlink_id;
        }
    }

    ok &= check(
        term::validate_render_snapshot(remapped).status ==
            term::Terminal_render_snapshot_status::OK,
        "hyperlink namespace mismatch fixture remains a valid full snapshot");
    const term::Terminal_session_lazy_snapshot_composer_result result =
        session.compose_lazy_render_snapshot_for_testing(previous, remapped);
    ok &= check(!result.lazy_snapshot.has_value(),
        "hyperlink namespace mismatch does not produce a lazy snapshot");
    ok &= expect_lazy_fallback(
        result,
        term::Terminal_lazy_snapshot_fallback_reason::
            HYPERLINK_NAMESPACE_INCOMPATIBILITY,
        "lazy composer rejects compatible-but-renumbered hyperlink namespace");
    return ok;
}

bool test_lazy_snapshot_correctness_stress_replay()
{
    bool ok = true;

    Replay_backend* backend = nullptr;
    std::unique_ptr<term::Terminal_session> session =
        make_session(backend, {8, 32});
    ok &= check(backend != nullptr, "stress replay session starts");
    if (backend == nullptr) {
        return false;
    }

    ok &= check(backend->emit_output(rich_visible_baseline()),
        "stress replay emits rich baseline");
    std::shared_ptr<const term::Terminal_render_snapshot> previous =
        session->latest_render_snapshot_handle();
    ok &= check(previous != nullptr, "stress replay baseline snapshot exists");
    if (previous == nullptr) {
        return false;
    }
    ok &= check(previous->lazy_row_payloads == nullptr,
        "stress replay baseline production snapshot is full");
    ok &= expect_rich_snapshot_features(*previous, "stress replay baseline");
    session->mark_render_snapshot_synced(session->render_snapshot_generation());

    ok &= emit_output_and_compare(
        *backend,
        *session,
        previous,
        row_write(6, QByteArrayLiteral("\x1b[32mGREEN\x1b[0m")),
        "stress replay styled dirty row");
    ok &= emit_output_and_compare(
        *backend,
        *session,
        previous,
        row_write(2, QByteArrayLiteral("wide \xE7\x95\x8C!\xE7\x95\x8C")),
        "stress replay wide dirty row");
    ok &= emit_output_and_compare(
        *backend,
        *session,
        previous,
        row_write(3, QByteArrayLiteral("comb re\xCC\x81sum\xCC\x81" "e")),
        "stress replay combining dirty row");
    ok &= emit_output_and_compare(
        *backend,
        *session,
        previous,
        clear_row_at(5),
        "stress replay borrowed blank row");

    const std::shared_ptr<const term::Terminal_render_snapshot> namespace_current =
        session->latest_render_snapshot_handle();
    if (namespace_current != nullptr) {
        ok &= expect_hyperlink_namespace_mismatch_fallback(
            *session,
            previous,
            *namespace_current);
    }

    return ok;
}

bool test_lazy_snapshot_resize_and_viewport_replay_boundaries()
{
    bool ok = true;

    Replay_backend* backend = nullptr;
    std::unique_ptr<term::Terminal_session> session =
        make_session(backend, {5, 24});
    ok &= check(backend != nullptr, "boundary replay session starts");
    if (backend == nullptr) {
        return false;
    }

    ok &= check(backend->emit_output(numbered_scrollback_prefix()),
        "boundary replay creates scrollback");
    std::shared_ptr<const term::Terminal_render_snapshot> previous =
        session->latest_render_snapshot_handle();
    ok &= check(previous != nullptr && previous->viewport.scrollback_rows > 0,
        "boundary replay baseline has scrollback");
    if (previous == nullptr) {
        return false;
    }
    session->mark_render_snapshot_synced(session->render_snapshot_generation());

    const term::Terminal_viewport_scroll_result scroll =
        session->scroll_viewport_lines(1);
    const std::shared_ptr<const term::Terminal_render_snapshot> scrolled =
        session->latest_render_snapshot_handle();
    ok &= check(
        scroll.action == term::Terminal_viewport_scroll_action::VIEWPORT_MOVED,
        "boundary replay moves the viewport");
    ok &= check(scrolled != nullptr, "boundary replay scrolled snapshot exists");
    if (scrolled != nullptr) {
        const term::Terminal_session_lazy_snapshot_composer_result result =
            session->compose_lazy_render_snapshot_for_testing(previous, *scrolled);
        ok &= expect_lazy_fallback(
            result,
            term::Terminal_lazy_snapshot_fallback_reason::VIEWPORT_MISMATCH,
            "lazy composer rejects real viewport scroll mapping changes");

        term::Terminal_render_snapshot content_viewport_mismatch = *previous;
        ++content_viewport_mismatch.viewport.offset_from_tail;
        const term::Terminal_session_lazy_snapshot_composer_result mismatch =
            session->compose_lazy_render_snapshot_for_testing(
                previous,
                content_viewport_mismatch);
        ok &= expect_lazy_fallback(
            mismatch,
            term::Terminal_lazy_snapshot_fallback_reason::VIEWPORT_MISMATCH,
            "lazy composer rejects content snapshots with different viewport mapping");

        term::Terminal_render_snapshot public_projection_snapshot = *previous;
        public_projection_snapshot.basis =
            term::Terminal_render_snapshot_basis::PUBLIC_PROJECTION;
        public_projection_snapshot.purpose =
            term::Terminal_render_snapshot_purpose::SCROLL;
        const term::Terminal_session_lazy_snapshot_composer_result public_projection =
            session->compose_lazy_render_snapshot_for_testing(
                previous,
                public_projection_snapshot);
        ok &= expect_lazy_fallback(
            public_projection,
            term::Terminal_lazy_snapshot_fallback_reason::PUBLIC_PROJECTION,
            "lazy composer keeps public viewport projection out of lazy content");
    }

    session->mark_render_snapshot_synced(session->render_snapshot_generation());
    const term::Terminal_session_result resize =
        session->resize(QSizeF(240.0, 80.0), {6, 24});
    const std::shared_ptr<const term::Terminal_render_snapshot> resized =
        session->latest_render_snapshot_handle();
    ok &= check(resize.code == term::Terminal_session_result_code::ACCEPTED,
        "boundary replay resize is accepted");
    ok &= check(resized != nullptr, "boundary replay resized snapshot exists");
    if (resized != nullptr) {
        const term::Terminal_session_lazy_snapshot_composer_result result =
            session->compose_lazy_render_snapshot_for_testing(previous, *resized);
        ok &= expect_lazy_fallback(
            result,
            term::Terminal_lazy_snapshot_fallback_reason::GRID_MISMATCH,
            "lazy composer rejects resized grids");
    }

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_lazy_snapshot_correctness_stress_replay();
    ok &= test_lazy_snapshot_resize_and_viewport_replay_boundaries();
    return ok ? 0 : 1;
}
