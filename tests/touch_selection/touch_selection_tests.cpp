#include "vnm_terminal/terminal_touch_selection.h"
#include "vnm_terminal/terminal_touch_selection_codec.h"
#include "vnm_terminal/terminal_canvas_export.h"
#include "vnm_terminal/vnm_terminal_canvas.h"
#include "vnm_terminal/vnm_terminal_surface.h"

#include "helpers/primary_backing_test_config.h"
#include "helpers/test_check.h"
#include "vnm_terminal/internal/qsg_terminal_render_frame.h"
#include "vnm_terminal/internal/qsg_atlas_renderer.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/internal/terminal_session.h"
#include "vnm_terminal/internal/terminal_touch_controller.h"
#include <QGuiApplication>
#include <QColor>
#include <QCoreApplication>
#include <QPointingDevice>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStyleHints>
#include <QTest>
#include <QTouchEvent>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;
namespace touch = vnm_terminal;

namespace {

using vnm_terminal::test_helpers::check;

class Touch_backend final : public term::Terminal_backend
{
public:
    term::Terminal_backend_result start(
        const term::Terminal_launch_config&, term::Terminal_backend_callbacks callbacks) override
    {
        m_callbacks = std::move(callbacks);
        return term::backend_accept();
    }
    term::Terminal_backend_result write(QByteArray) override { return term::backend_accept(); }
    term::Terminal_backend_result resize(term::Terminal_backend_resize_request) override { return term::backend_accept(); }
    term::Terminal_backend_result set_output_paused(bool) override { return term::backend_accept(); }
    term::Terminal_backend_result interrupt() override { return term::backend_accept(); }
    term::Terminal_backend_result terminate() override { return term::backend_accept(); }
    void output(QByteArray bytes) { m_callbacks.output_received(std::move(bytes)); }

private:
    term::Terminal_backend_callbacks m_callbacks;
};

touch::Terminal_touch_selection_request request_at(
    const term::Terminal_render_snapshot& snapshot,
    touch::Terminal_touch_selection_action action,
    int row,
    int column,
    std::uint64_t request_id)
{
    touch::Terminal_touch_selection_request request;
    request.request_id = request_id;
    request.gesture_id = QStringLiteral("authenticated-touch-test");
    request.action     = action;
    request.source     = snapshot.touch_selection->source;
    request.target     = {snapshot.touch_selection->visible_rows[(std::size_t)row], column};
    request.expected_selection_generation = snapshot.touch_selection->selection_generation;
    return request;
}

bool owner_selection_contract()
{
    auto backend = std::make_unique<Touch_backend>();
    auto* output = backend.get();
    auto config = touch::test_helpers::recovery_disabled_primary_backing_session_config();
    config.selection_viewport_projection_enabled = true;
    term::Terminal_session session(std::move(backend), config);
    term::Terminal_launch_config launch;
    launch.argv = {QStringLiteral("touch-contract-fixture")};
    launch.initial_grid_size = term::terminal_grid_size_t{4, 20};
    bool ok = check(session.start(launch).code == term::Terminal_session_result_code::ACCEPTED,
        "owner fixture starts");
    output->output(QByteArrayLiteral("alpha beta\r\nsecond\r\nthird\r\nfourth\r\nfifth"));
    session.process_backend_callback_events();
    auto snapshot = session.latest_render_snapshot_handle();
    ok &= check(snapshot && snapshot->touch_selection &&
        touch::terminal_canvas_selection_is_valid(*snapshot->touch_selection, 4, 20),
        "published selection source contains exact retained rows");
    if (!ok) {
        return false;
    }
    const auto before_inspection = *snapshot->touch_selection;
    auto inspect = request_at(*snapshot, touch::Terminal_touch_selection_action::INSPECT_WORD, 0, 1, 90U);
    const auto inspected = session.apply_touch_selection(inspect);
    ok &= check(inspected.status == touch::Terminal_touch_selection_status::OK &&
        inspected.word_available == true &&
        *session.latest_render_snapshot_handle()->touch_selection == before_inspection,
        "owner word inspection preserves the exact shared selection and handle state");
    inspect.target.column = 15;
    const auto inspected_blank = session.apply_touch_selection(inspect);
    ok &= check(inspected_blank.status == touch::Terminal_touch_selection_status::OK &&
        inspected_blank.word_available == false &&
        *session.latest_render_snapshot_handle()->touch_selection == before_inspection,
        "owner confirms blank cells without creating or clearing selection");
    ok &= check(touch::touch_selection_codec::decode_request(touch::touch_selection_codec::encode_request(inspect)) == inspect &&
        touch::touch_selection_codec::decode_result(touch::touch_selection_codec::encode_result(inspected)) == inspected,
        "the optional word query contract round-trips its explicit eligibility result");
    auto unsupported_query = touch::touch_selection_codec::encode_request(inspect);
    unsupported_query.insert(QStringLiteral("word_query"), touch::touch_selection_codec::word_query_marker(99));
    ok &= check(!touch::touch_selection_codec::decode_request(unsupported_query),
        "an unknown word query version cannot execute through the v1 selection route");
    auto word = request_at(*snapshot, touch::Terminal_touch_selection_action::SELECT_WORD, 0, 1, 1U);
    const auto selected = session.apply_touch_selection(word);
    ok &= check(selected.status == touch::Terminal_touch_selection_status::OK &&
        session.selected_text().text == QStringLiteral("second"),
        "word selection expands source text without sending mouse input");
    snapshot = session.latest_render_snapshot_handle();
    ok &= check(snapshot->touch_selection->touch_handles_visible &&
        snapshot->touch_selection->start == touch::terminal_canvas_selection_endpoint_t{0, 0, true} &&
        snapshot->touch_selection->end == touch::terminal_canvas_selection_endpoint_t{0, 6, true} &&
        snapshot->touch_selection->spans == std::vector<touch::terminal_canvas_selection_span_t>{{0, 0, 6}},
        "touch selection publishes shared handles and highlight");
    auto begin = request_at(*snapshot, touch::Terminal_touch_selection_action::BEGIN_HANDLE, 0, 6, 2U);
    begin.handle = touch::Terminal_selection_handle::END;
    ok &= check(session.apply_touch_selection(begin).status == touch::Terminal_touch_selection_status::OK,
        "existing owner selection accepts handle gesture");
    auto scroll = begin;
    scroll.request_id = 3U;
    scroll.action = touch::Terminal_touch_selection_action::SCROLL_EXTEND;
    scroll.scroll_lines = 1;
    scroll.target.column = 5;
    const auto extended = session.apply_touch_selection(scroll);
    ok &= check(extended.status == touch::Terminal_touch_selection_status::OK &&
        session.viewport_state().offset_from_tail == 1,
        "edge extension moves the original shared viewport");
    snapshot = session.latest_render_snapshot_handle();
    auto copy = request_at(*snapshot, touch::Terminal_touch_selection_action::COPY, 0, 0, 4U);
    const auto copied = session.apply_touch_selection(copy);
    ok &= check(copied.status == touch::Terminal_touch_selection_status::OK &&
        copied.text == QStringLiteral(" beta\n"),
        "copy retains owner row newline semantics across history");
    auto stale = copy;
    stale.request_id = 5U;
    session.clear_selection();
    ok &= check(session.apply_touch_selection(stale).status == touch::Terminal_touch_selection_status::STALE_SELECTION,
        "desktop replacement invalidates an old copy generation");

    (void)session.scroll_published_viewport_lines(-64);
    ok &= check(session.viewport_state().offset_from_tail == 0,
        "changed-row fixture presents the active output row");
    snapshot = session.latest_render_snapshot_handle();
    auto changed = request_at(*snapshot, touch::Terminal_touch_selection_action::SELECT_WORD, 3, 1, 6U);
    output->output(QByteArrayLiteral("\rupdated"));
    session.process_backend_callback_events();
    ok &= check(session.apply_touch_selection(changed).status == touch::Terminal_touch_selection_status::STALE_SOURCE,
        "a changed touched row is rejected rather than rebound to neighboring text");

    (void)session.scroll_published_viewport_lines(-64);
    output->output(QByteArray("\r\n") + QString::fromUtf8("cafe\xcc\x81 \xe7\x95\x8c").toUtf8());
    session.process_backend_callback_events();
    snapshot = session.latest_render_snapshot_handle();
    auto unicode = request_at(*snapshot, touch::Terminal_touch_selection_action::SELECT_WORD, 3, 3, 7U);
    ok &= check(session.apply_touch_selection(unicode).status == touch::Terminal_touch_selection_status::OK &&
        session.selected_text().text == QString::fromUtf8("cafe\xcc\x81"),
        "word expansion preserves a combining cluster in original text");
    auto lagged = request_at(*snapshot, touch::Terminal_touch_selection_action::SELECT_WORD, 3, 3, 8U);
    output->output(QByteArrayLiteral("\r\nother output"));
    session.process_backend_callback_events();
    ok &= check(session.apply_touch_selection(lagged).status == touch::Terminal_touch_selection_status::OK,
        "unchanged retained row proof tolerates unrelated publication lag");
    auto mismatched_basis = lagged;
    ++mismatched_basis.source.grid_reflow_generation;
    ok &= check(session.apply_touch_selection(mismatched_basis).status == touch::Terminal_touch_selection_status::STALE_SOURCE,
        "reflow provenance rejects an earlier basis even at the same grid size");
    auto old_geometry = lagged;
    old_geometry.request_id = 9U;
    session.resize(QSizeF(210.0, 80.0), {4, 21});
    ok &= check(session.apply_touch_selection(old_geometry).status == touch::Terminal_touch_selection_status::STALE_SOURCE,
        "reflow rejects targets in an earlier cell geometry");
    snapshot = session.latest_render_snapshot_handle();
    const auto offscreen = request_at(*snapshot, touch::Terminal_touch_selection_action::SELECT_WORD, 3, 2, 10U);
    output->output(QByteArrayLiteral("\r\n1\r\n2\r\n3\r\n4\r\n5"));
    session.process_backend_callback_events();
    ok &= check(session.apply_touch_selection(offscreen).status == touch::Terminal_touch_selection_status::STALE_SOURCE,
        "a word that left the shared viewport is rejected without a deferred offscreen menu");
    return ok;
}

class Touch_item final : public QQuickItem
{
public:
    explicit Touch_item(QQuickItem* parent)
    :
        QQuickItem(parent)
    {
        setAcceptTouchEvents(true);
        setWidth(200.0);
        setHeight(100.0);
    }
    std::unique_ptr<term::Terminal_touch_controller> controller;
    QPointF last_touch_scene_position;

protected:
    void touchEvent(QTouchEvent* event) override
    {
        if (!event->points().empty()) {
            last_touch_scene_position = event->points().front().scenePosition();
        }
        controller->touch_event(*event);
    }
    void touchUngrabEvent() override { controller->touch_ungrabbed(); }
};

bool wait_for_touch_hold(QQuickItem& item)
{
    // Qualification keeps the grab before selecting text or awaiting release.
    // A fixed sleep can expire before the event loop delivers the hold timer.
    return check(QTest::qWaitFor([&] { return item.keepTouchGrab(); }, 1000),
        "the hold qualifies before release or presentation changes");
}

bool touch_intent_contract()
{
    QGuiApplication::styleHints()->setMousePressAndHoldInterval(1000);
    QQuickWindow window;
    window.resize(200, 100);
    Touch_item item(window.contentItem());
    int taps = 0;
    int paste_menus = 0;
    int copy_menus = 0;
    std::vector<touch::Terminal_touch_selection_request> requests;
    term::Terminal_touch_presentation presentation;
    presentation.cell_metrics = {10.0, 20.0, 14.0, 6.0};
    presentation.selection.source = {1U, touch::Terminal_selection_buffer::PRIMARY_SCREEN, 1U, 5, 20};
    presentation.selection.selection_generation = 1U;
    for (int row = 0; row < 5; ++row) {
        presentation.selection.visible_rows.push_back({(std::uint64_t)row + 1U, 1U, 0});
    }
    item.controller = std::make_unique<term::Terminal_touch_controller>(item,
        term::Terminal_touch_controller::Callbacks{
            [&presentation] { return std::optional(presentation); },
            [&requests](const auto& request) { requests.push_back(request); },
            [&taps] { ++taps; },
            [&paste_menus, &copy_menus](touch::Terminal_touch_menu menu, QRectF, std::uint64_t) {
                menu == touch::Terminal_touch_menu::PASTE ? ++paste_menus : ++copy_menus;
            },
            [] {},
        }, true);
    window.show();
    QTest::qWait(30);
    auto* device = QTest::createTouchDevice();
    QTest::touchEvent(&window, device).press(0, QPoint(55, 30), &window);
    QTest::touchEvent(&window, device).release(0, QPoint(55, 30), &window);
    bool ok = check(taps == 1 && requests.empty(), "short touch release activates typing once");
    QGuiApplication::styleHints()->setMousePressAndHoldInterval(1000);
    QTest::touchEvent(&window, device).press(0, QPoint(55, 30), &window);
    QTest::touchEvent(&window, device).move(0, QPoint(100, 70), &window);
    QTest::touchEvent(&window, device).release(0, QPoint(100, 70), &window);
    ok &= check(taps == 1 && requests.empty(), "scroll movement cancels pending hold and typing activation");
    QGuiApplication::styleHints()->setMousePressAndHoldInterval(20);
    QTest::touchEvent(&window, device).press(0, QPoint(55, 30), &window);
    if (!wait_for_touch_hold(item)) {
        return false;
    }
    QTest::touchEvent(&window, device).release(0, QPoint(55, 30), &window);
    ok &= check(requests.size() == 1U && requests.front().action == touch::Terminal_touch_selection_action::SELECT_WORD &&
        requests.front().target.column == 5 && taps == 1 && copy_menus == 0,
        "a hold selects the displayed word and waits for its owner before the menu");
    if (!requests.empty()) {
        presentation.selection.has_selection = true;
        presentation.selection.touch_handles_visible = true;
        presentation.selection.selection_generation = 2U;
        presentation.selection.start = {1, 4, true};
        presentation.selection.end = {1, 8, true};
        presentation.selection.spans = {{1, 4, 4}};
        item.controller->complete({requests.front().request_id, requests.front().action,
            touch::Terminal_touch_selection_status::OK, 2U, {}, {}});
        ok &= check(requests.size() == 2U && requests.back().action == touch::Terminal_touch_selection_action::END_GESTURE,
            "release completes one owner gesture without a short tap");
        item.controller->complete({requests.back().request_id, requests.back().action,
            touch::Terminal_touch_selection_status::OK, 2U, {}, {}});
        ok &= check(copy_menus == 1 && taps == 1, "Copy menu follows matching presented owner selection");
        QTest::touchEvent(&window, device).press(0, QPoint(170, 10), &window);
        QTest::touchEvent(&window, device).release(0, QPoint(170, 10), &window);
        ok &= check(taps == 1 && requests.back().action == touch::Terminal_touch_selection_action::CLEAR &&
            requests.back().expected_selection_generation == 2U,
            "a short tap clears the selected text without activating the keyboard");
        const auto clearing = requests.back();
        QTest::touchEvent(&window, device).press(0, QPoint(170, 10), &window);
        QTest::touchEvent(&window, device).release(0, QPoint(170, 10), &window);
        ok &= check(taps == 1 && requests.back() == clearing,
            "a second tap while dismissal is in flight cannot activate typing");
        item.controller->complete({clearing.request_id, clearing.action,
            touch::Terminal_touch_selection_status::OK, 3U, {}, {}});
        QTest::touchEvent(&window, device).press(0, QPoint(170, 10), &window);
        QTest::touchEvent(&window, device).release(0, QPoint(170, 10), &window);
        ok &= check(taps == 1, "dismissal waits for the cleared presentation before admitting a typing tap");
        presentation.selection.selection_generation = 3U;
        presentation.selection.has_selection = false;
        presentation.selection.touch_handles_visible = false;
        presentation.selection.spans.clear();
        item.controller->presentation_changed();
        QTest::touchEvent(&window, device).press(0, QPoint(170, 10), &window);
        QTest::touchEvent(&window, device).release(0, QPoint(170, 10), &window);
        ok &= check(taps == 2 && copy_menus == 1,
            "the following tap activates typing after authoritative selection dismissal");

        QTest::touchEvent(&window, device).press(0, QPoint(55, 30), &window);
        if (!wait_for_touch_hold(item)) {
            return false;
        }
        QTest::touchEvent(&window, device).release(0, QPoint(55, 30), &window);
        const auto pending_selection = requests.back();
        QTest::touchEvent(&window, device).press(0, QPoint(170, 10), &window);
        QTest::touchEvent(&window, device).release(0, QPoint(170, 10), &window);
        item.controller->complete({pending_selection.request_id, pending_selection.action,
            touch::Terminal_touch_selection_status::OK, 4U, {}, {}});
        ok &= check(requests.back().action == touch::Terminal_touch_selection_action::CLEAR &&
            requests.back().expected_selection_generation == 4U && taps == 2 && copy_menus == 1,
            "dismissing an unacknowledged hold clears its accepted generation without opening a menu");
        const auto pending_clear = requests.back();
        item.controller->complete({pending_selection.request_id, pending_selection.action,
            touch::Terminal_touch_selection_status::OK, 4U, {}, {}});
        ok &= check(requests.back() == pending_clear && copy_menus == 1,
            "a duplicate selection reply cannot reopen a dismissed gesture");
    }
    return ok;
}

bool direct_word_hold_contract()
{
    QGuiApplication::styleHints()->setMousePressAndHoldInterval(20);
    QQuickWindow window;
    window.resize(200, 100);
    Touch_item item(window.contentItem());
    term::Terminal_touch_presentation shown;
    shown.cell_metrics = {10.0, 20.0, 14.0, 6.0};
    shown.selection.source = {1U, touch::Terminal_selection_buffer::PRIMARY_SCREEN, 1U, 5, 20};
    shown.selection.selection_generation = 1U;
    shown.selection.word_query_version = touch::k_terminal_word_query_version;
    for (int row = 0; row < 5; ++row) {
        shown.selection.visible_rows.push_back({(std::uint64_t)row + 1U, 1U, 0});
    }
    std::vector<touch::Terminal_touch_selection_request> requests;
    std::vector<touch::Terminal_touch_menu> menus;
    QStringList notices;
    int taps = 0;
    int dismissals = 0;
    item.controller = std::make_unique<term::Terminal_touch_controller>(item,
        term::Terminal_touch_controller::Callbacks{
            [&] { return std::optional(shown); },
            [&](const auto& request) { requests.push_back(request); },
            [&] { ++taps; },
            [&](auto menu, QRectF, std::uint64_t) { menus.push_back(menu); },
            [&] { ++dismissals; },
            [&](QString notice) { notices.append(std::move(notice)); },
        }, true);
    window.show();
    QTest::qWait(30);
    auto* device = QTest::createTouchDevice();
    const auto hold = [&](bool release = true) {
        QTest::touchEvent(&window, device).press(0, QPoint(55, 30), &window);
        if (!wait_for_touch_hold(item)) {
            return false;
        }
        if (release) {
            QTest::touchEvent(&window, device).release(0, QPoint(55, 30), &window);
        }
        return true;
    };
    if (!hold(false)) {
        return false;
    }
    bool ok = check(requests.size() == 1 &&
        requests.back().action == touch::Terminal_touch_selection_action::INSPECT_WORD && menus.empty() && taps == 0,
        "a capable-owner hold inspects the original word without issuing a selection mutation");
    if (!ok) {
        return false;
    }
    const auto inspected_target = requests.back();
    ++shown.selection.visible_rows[4].content_generation;
    item.controller->presentation_changed();
    std::swap(shown.selection.visible_rows[0], shown.selection.visible_rows[1]);
    item.controller->presentation_changed();
    item.controller->complete({inspected_target.request_id, inspected_target.action,
        touch::Terminal_touch_selection_status::OK, 1U, {}, {}, true});
    ok &= check(requests.size() == 2 && menus.empty() &&
        requests.back().action == touch::Terminal_touch_selection_action::SELECT_WORD &&
        requests.back().target == inspected_target.target && requests.back().source == inspected_target.source,
        "an owner-confirmed word is selected immediately during the hold at its original row and column");
    if (!ok) {
        QTest::touchEvent(&window, device).release(0, QPoint(55, 30), &window);
        return false;
    }
    const auto selected = requests.back();
    shown.selection.has_selection = true;
    shown.selection.touch_handles_visible = true;
    shown.selection.selection_generation = 2U;
    shown.selection.start = {0, 4, true};
    shown.selection.end = {0, 8, true};
    shown.selection.spans = {{0, 4, 4}};
    item.controller->presentation_changed();
    item.controller->complete({selected.request_id, selected.action,
        touch::Terminal_touch_selection_status::OK, 2U});
    ok &= check(requests.size() == 2 && menus.empty(),
        "the selected frame may precede its reply without cancelling the held gesture or opening Copy");
    QTest::touchEvent(&window, device).release(0, QPoint(55, 30), &window);
    const auto ended = requests.back();
    ok &= check(ended.action == touch::Terminal_touch_selection_action::END_GESTURE,
        "releasing the selected word ends the original gesture");
    item.controller->complete({ended.request_id, ended.action,
        touch::Terminal_touch_selection_status::OK, 2U});
    ok &= check(menus.back() == touch::Terminal_touch_menu::COPY && taps == 0,
        "the selected generation presents Copy only and does not activate typing");
    item.controller->cancel();
    shown.selection.has_selection = false;
    shown.selection.touch_handles_visible = false;
    shown.selection.spans.clear();
    if (!hold()) {
        return false;
    }
    auto query = requests.back();
    item.controller->complete({query.request_id, query.action,
        touch::Terminal_touch_selection_status::OK, 2U, {}, {}, false});
    ok &= check(menus.back() == touch::Terminal_touch_menu::PASTE,
        "owner-confirmed blank cells offer only Paste");
    const auto dismissals_before_output = dismissals;
    const auto notices_before_output = notices.size();
    ++shown.selection.visible_rows[1].content_generation;
    item.controller->presentation_changed();
    ok &= check(dismissals == dismissals_before_output && notices.size() == notices_before_output &&
        item.controller->selection_active(),
        "Paste on confirmed blank cells survives ordinary output at the pressed row");
    ++shown.selection.source.session_epoch;
    item.controller->presentation_changed();
    ok &= check(dismissals == dismissals_before_output + 1 && !item.controller->selection_active(),
        "a blank-cell Paste menu still closes when its terminal source changes");
    item.controller->cancel();
    if (!hold()) {
        return false;
    }
    query = requests.back();
    const auto requests_before_changed_reply = requests.size();
    ++shown.selection.visible_rows[1].content_generation;
    item.controller->presentation_changed();
    item.controller->complete({query.request_id, query.action,
        touch::Terminal_touch_selection_status::OK, 2U, {}, {}, true});
    ok &= check(requests.size() == requests_before_changed_reply && !notices.isEmpty(),
        "a changed original target fails explicitly instead of selecting replacement text");
    if (!hold()) {
        return false;
    }
    query = requests.back();
    const auto menus_before_dismissal = menus.size();
    item.controller->dismiss_selection();
    item.controller->complete({query.request_id, query.action,
        touch::Terminal_touch_selection_status::OK, 2U, {}, {}, true});
    ok &= check(menus.size() == menus_before_dismissal && taps == 0,
        "a late inspection result cannot reopen a dismissed menu or activate typing");
    if (!hold()) {
        return false;
    }
    query = requests.back();
    item.controller->complete({query.request_id, query.action,
        touch::Terminal_touch_selection_status::OK, 2U, {}, {}, true});
    const auto released_selection = requests.back();
    ok &= check(released_selection.action == touch::Terminal_touch_selection_action::SELECT_WORD &&
        released_selection.target == query.target,
        "a qualified hold released before its query reply still selects the original word");
    shown.selection.has_selection = true;
    shown.selection.touch_handles_visible = true;
    shown.selection.selection_generation = 3U;
    shown.selection.start = {0, 4, true};
    shown.selection.end = {0, 8, true};
    shown.selection.spans = {{0, 4, 4}};
    item.controller->complete({released_selection.request_id, released_selection.action,
        touch::Terminal_touch_selection_status::OK, 3U});
    const auto released_end = requests.back();
    ok &= check(released_end.action == touch::Terminal_touch_selection_action::END_GESTURE,
        "a late successful selection completes the already-released hold");
    item.controller->complete({released_end.request_id, released_end.action,
        touch::Terminal_touch_selection_status::OK, 3U});
    ok &= check(menus.back() == touch::Terminal_touch_menu::COPY && taps == 0,
        "a late successful selection opens Copy without opening the keyboard");
    item.controller->cancel();
    shown.selection.has_selection = false;
    shown.selection.touch_handles_visible = false;
    shown.selection.spans.clear();
    item.controller->request_padding_menu(QPointF(220, 30));
    ok &= check(menus.back() == touch::Terminal_touch_menu::PASTE,
        "viewport padding offers Paste without a fabricated word target");
    item.controller->cancel();
    QTest::touchEvent(&window, device).press(0, QPoint(55, 30), &window);
    if (!wait_for_touch_hold(item)) {
        return false;
    }
    query = requests.back();
    item.controller->complete({query.request_id, query.action,
        touch::Terminal_touch_selection_status::OK, 3U, {}, {}, false});
    const auto menus_before_shared_selection = menus.size();
    const auto requests_before_shared_selection = requests.size();
    shown.selection.has_selection = true;
    shown.selection.selection_generation = 4U;
    shown.selection.start = {0, 4, true};
    shown.selection.end = {0, 8, true};
    shown.selection.spans = {{0, 4, 4}};
    item.controller->presentation_changed();
    QTest::touchEvent(&window, device).release(0, QPoint(55, 30), &window);
    ok &= check(menus.size() == menus_before_shared_selection &&
        requests.size() == requests_before_shared_selection && shown.selection.has_selection && taps == 0,
        "a shared selection arriving before blank-hold release suppresses Paste without clearing that selection");
    const auto requests_before_selected_hold = requests.size();
    shown.block_cursor = QRectF(150, 60, 10, 20);
    QTest::touchEvent(&window, device).press(0, QPoint(155, 70), &window);
    shown.selection.selection_generation = 5U;
    item.controller->presentation_changed();
    if (!wait_for_touch_hold(item)) {
        return false;
    }
    shown.selection.selection_generation = 6U;
    item.controller->presentation_changed();
    QTest::touchEvent(&window, device).release(0, QPoint(155, 70), &window);
    ok &= check(menus.size() == menus_before_shared_selection + 1 &&
        menus.back() == touch::Terminal_touch_menu::COPY && requests.size() == requests_before_selected_hold,
        "holding an existing shared selection presents its current generation after changes during the hold");
    return ok;
}

bool cursor_hold_contract()
{
    QGuiApplication::styleHints()->setMousePressAndHoldInterval(20);
    QQuickWindow window;
    window.resize(400, 240);
    Touch_item item(window.contentItem());
    term::Terminal_touch_presentation shown;
    shown.cell_metrics = {10.0, 20.0, 14.0, 6.0};
    shown.selection.source = {1U, touch::Terminal_selection_buffer::PRIMARY_SCREEN, 1U, 5, 20};
    shown.selection.selection_generation = 1U;
    shown.selection.word_query_version = touch::k_terminal_word_query_version;
    for (int row = 0; row < 5; ++row) {
        shown.selection.visible_rows.push_back({(std::uint64_t)row + 1U, 1U, 0});
    }
    std::vector<touch::Terminal_touch_selection_request> requests;
    std::vector<touch::Terminal_touch_menu> menus;
    bool paste_available = true;
    int taps = 0;
    item.controller = std::make_unique<term::Terminal_touch_controller>(item,
        term::Terminal_touch_controller::Callbacks{
            [&] { return std::optional(shown); },
            [&](const auto& request) { requests.push_back(request); },
            [&] { ++taps; },
            [&](auto menu, QRectF, std::uint64_t) { menus.push_back(menu); },
            [] {},
            {},
            [&] { return paste_available; },
        }, true);
    window.show();
    QTest::qWait(30);
    auto* device = QTest::createTouchDevice();
    bool ok = true;
    struct Cursor_case { QPointF point; qreal scale; bool paste; };
    for (const auto& test : std::vector<Cursor_case>{
            {{55, 30}, 1.0, true}, {{61, 30}, 1.0, true}, {{65, 30}, 1.0, false},
            {{55, 44}, 1.0, true}, {{55, 50}, 1.0, false},
            {{55, 42}, 2.0, true}, {{55, 44}, 2.0, false}})
    {
        item.controller->cancel();
        item.setScale(test.scale);
        shown.block_cursor = QRectF(50, 20, 10, 20);
        requests.clear();
        menus.clear();
        const QPoint point = item.mapToScene(test.point).toPoint();
        QTest::touchEvent(&window, device).press(0, point, &window);
        if (!wait_for_touch_hold(item)) {
            return false;
        }
        ok &= check(menus.empty(), "cursor holds do not open Paste before release");
        // Cursor movement after qualification does not reinterpret this hold.
        shown.block_cursor.translate(40, 0);
        item.controller->presentation_changed();
        QTest::touchEvent(&window, device).release(0, point, &window);
        ok &= check(test.paste
                ? requests.empty() && menus == std::vector{touch::Terminal_touch_menu::PASTE}
                : requests.size() == 1 && requests.front().action == touch::Terminal_touch_selection_action::INSPECT_WORD &&
                    menus.empty(),
            "cursor-cell and bounded near-edge holds paste while neighboring centers and the screen-margin cap still select");
    }
    item.controller->cancel();
    item.setScale(1.0);
    shown.block_cursor = QRectF(50, 20, 10, 20);
    requests.clear();
    menus.clear();
    paste_available = false;
    QTest::touchEvent(&window, device).press(0, QPoint(55, 30), &window);
    if (!wait_for_touch_hold(item)) {
        return false;
    }
    QTest::touchEvent(&window, device).release(0, QPoint(55, 30), &window);
    ok &= check(requests.size() == 1 &&
        requests.front().action == touch::Terminal_touch_selection_action::INSPECT_WORD && menus.empty(),
        "a word under the cursor remains selectable when local Paste is unavailable");
    paste_available = true;
    item.controller->cancel();
    item.setScale(1.0);
    item.controller->set_viewport(QRectF(0, 40, 200, 60));
    shown.block_cursor = QRectF(50, 20, 10, 20);
    requests.clear();
    menus.clear();
    QTest::touchEvent(&window, device).press(0, QPoint(55, 42), &window);
    if (!wait_for_touch_hold(item)) {
        return false;
    }
    QTest::touchEvent(&window, device).release(0, QPoint(55, 42), &window);
    ok &= check(requests.size() == 1 && menus.empty(),
        "the proximity margin cannot revive a cursor outside the visible viewport");
    item.controller->cancel();
    item.controller->set_viewport({});
    requests.clear();
    QTest::touchEvent(&window, device).press(0, QPoint(55, 30), &window);
    if (!wait_for_touch_hold(item)) {
        return false;
    }
    ++shown.selection.source.session_epoch;
    item.controller->presentation_changed();
    QTest::touchEvent(&window, device).release(0, QPoint(55, 30), &window);
    ok &= check(requests.empty() && menus.empty() && taps == 0,
        "a cursor Paste hold is cancelled by a terminal source change without activating typing");
    return ok;
}

bool touch_lifecycle_contract()
{
    QGuiApplication::styleHints()->setMousePressAndHoldInterval(1000);
    QQuickWindow window;
    window.resize(200, 160);
    Touch_item item(window.contentItem());
    item.setHeight(160);
    term::Terminal_touch_presentation shown;
    shown.cell_metrics = {10.0, 20.0, 14.0, 6.0};
    shown.selection.source = {1U, touch::Terminal_selection_buffer::PRIMARY_SCREEN, 1U, 5, 20};
    shown.selection.selection_generation = 3U;
    shown.selection.has_selection = true;
    shown.selection.touch_handles_visible = true;
    shown.selection.start = {2, 3, true};
    shown.selection.end = {2, 10, true};
    shown.selection.spans = {{2, 3, 7}};
    for (int row = 0; row < 5; ++row) {
        shown.selection.visible_rows.push_back({(std::uint64_t)row + 1U, 1U, 0});
    }
    bool available = true;
    int taps = 0;
    int menus = 0;
    int dismissals = 0;
    std::vector<touch::Terminal_touch_selection_request> requests;
    item.controller = std::make_unique<term::Terminal_touch_controller>(item,
        term::Terminal_touch_controller::Callbacks{
            [&] { return available ? std::optional(shown) : std::nullopt; },
            [&](const auto& request) { requests.push_back(request); },
            [&] { ++taps; },
            [&](touch::Terminal_touch_menu, QRectF, std::uint64_t) { ++menus; },
            [&] { ++dismissals; },
        }, true);
    const auto complete = [&](std::uint64_t generation) {
        const auto request = requests.back();
        item.controller->complete({request.request_id, request.action,
            touch::Terminal_touch_selection_status::OK, generation, {}, {}});
    };
    window.show();
    QTest::qWait(30);
    auto* device = QTest::createTouchDevice();
    QTest::touchEvent(&window, device).press(0, QPoint(100, 67), &window);
    if (!check(requests.size() == 1U && requests.back().action == touch::Terminal_touch_selection_action::BEGIN_HANDLE,
            "the visible end handle starts a shared owner gesture")) {
        return false;
    }
    complete(3U);
    QTest::touchEvent(&window, device).move(0, QPoint(110, 67), &window);
    if (!check(QTest::qWaitFor([&] { return requests.size() == 2U; }, 1000) &&
            requests.back().action == touch::Terminal_touch_selection_action::MOVE_HANDLE,
            "handle movement extends the original selection")) {
        return false;
    }
    available = false;
    complete(4U);
    QTest::touchEvent(&window, device).move(0, QPoint(130, 87), &window);
    // Qt Quick compresses moves until its next frame; restore the presentation
    // only after the controller has actually received the move during the gap.
    if (!check(QTest::qWaitFor([&] { return item.last_touch_scene_position == QPointF(130, 87); }, 1000),
            "the render-gap move reaches the touch controller")) {
        return false;
    }
    bool ok = check(requests.size() == 2U, "a render gap defers the next move without cancelling the drag");
    available = true;
    shown.selection.selection_generation = 4U;
    item.controller->presentation_changed();
    if (!check(requests.size() == 3U && requests.back().action == touch::Terminal_touch_selection_action::MOVE_HANDLE &&
            requests.back().target.column == 13,
            "the latest finger position resumes once a presentation is available")) {
        return false;
    }
    shown.selection.selection_generation = 5U;
    shown.selection.end = {3, 13, true};
    shown.selection.spans = {{2, 3, 17}, {3, 0, 13}};
    QTest::touchEvent(&window, device).move(0, QPoint(140, 87), &window);
    QTest::touchEvent(&window, device).release(0, QPoint(140, 87), &window);
    ok &= check(requests.size() == 3U && menus == 0,
        "release waits for an in-flight move and retains the latest finger position");
    complete(5U);
    ok &= check(requests.size() == 4U &&
        requests.back().action == touch::Terminal_touch_selection_action::MOVE_HANDLE &&
        requests.back().target.column == 14,
        "the delayed move reply applies the final released endpoint once");
    complete(6U);
    ok &= check(requests.back().action == touch::Terminal_touch_selection_action::END_GESTURE && menus == 0,
        "handle release ends the owner gesture before showing its menu");
    complete(6U);
    ok &= check(menus == 0, "a successful end waits for its selected generation to be presented");
    shown.selection.selection_generation = 6U;
    shown.selection.end = {3, 14, true};
    shown.selection.spans = {{2, 3, 17}, {3, 0, 14}};
    item.controller->presentation_changed();
    item.controller->presentation_changed();
    ok &= check(requests.size() == 5U &&
        std::count_if(requests.begin(), requests.end(), [](const auto& request) {
            return request.action == touch::Terminal_touch_selection_action::END_GESTURE;
        }) == 1 && dismissals == 0,
        "successor presentations never end the released owner gesture again or dismiss Copy");
    ok &= check(menus == 1 && taps == 0, "handle release reopens Copy without activating typing");

    QTest::touchEvent(&window, device).press(0, QPoint(130, 87), &window);
    const auto abandoned = requests.back();
    ok &= check(dismissals == 1, "handle-down dismisses the existing Copy menu");
    item.controller->set_enabled(false);
    ok &= check(requests.back().action == touch::Terminal_touch_selection_action::CANCEL_GESTURE,
        "disable orders cancellation even while the begin acknowledgement is missing");
    item.controller->set_enabled(true);
    // These cancellation cases start with an unselected terminal. Selection
    // dismissal has its own owner-acknowledgement contract above.
    shown.selection.has_selection = false;
    shown.selection.touch_handles_visible = false;
    QTest::touchEvent(&window, device).release(0, QPoint(130, 87), &window);
    QTest::touchEvent(&window, device).press(0, QPoint(60, 30), &window);
    QTest::touchEvent(&window, device).release(0, QPoint(60, 30), &window);
    item.controller->complete({abandoned.request_id, abandoned.action,
        touch::Terminal_touch_selection_status::OK, 6U, {}, {}});
    ok &= check(taps == 1 && menus == 1, "a lost or late reply cannot swallow the next tap or reopen a menu");

    QTest::touchEvent(&window, device).press(0, QPoint(60, 30), &window);
    item.ungrabTouchPoints();
    QTest::touchEvent(&window, device).release(0, QPoint(60, 30), &window);
    QTest::touchEvent(&window, device).press(0, QPoint(60, 30), &window);
    QTest::touchEvent(&window, device).release(0, QPoint(60, 30), &window);
    ok &= check(taps == 2, "grab loss cancels the old point and the next tap still works");

    QTest::touchEvent(&window, device).press(0, QPoint(60, 30), &window);
    QTest::touchEvent(&window, device).stationary(0).press(1, QPoint(150, 30), &window);
    QTest::touchEvent(&window, device).release(0, QPoint(60, 30), &window).stationary(1);
    QTest::touchEvent(&window, device).release(1, QPoint(150, 30), &window);
    QTest::touchEvent(&window, device).press(0, QPoint(60, 30), &window);
    QTest::touchEvent(&window, device).release(0, QPoint(60, 30), &window);
    ok &= check(taps == 3 && menus == 1, "pinch with the original finger lifting first never becomes a tap or wedges input");

    QTest::touchEvent(&window, device).press(0, QPoint(60, 30), &window);
    QTouchEvent cancelled(QEvent::TouchCancel, device);
    QCoreApplication::sendEvent(&window, &cancelled);
    QTest::touchEvent(&window, device).release(0, QPoint(60, 30), &window);
    QTest::touchEvent(&window, device).press(0, QPoint(60, 30), &window);
    QTest::touchEvent(&window, device).release(0, QPoint(60, 30), &window);
    ok &= check(taps == 4 && menus == 1, "TouchCancel never activates typing and leaves the next point usable");

    shown.selection.has_selection = true;
    shown.selection.touch_handles_visible = true;
    QTest::touchEvent(&window, device).press(0, QPoint(130, 87), &window);
    const auto unacknowledged = requests.back();
    if (!check(unacknowledged.action == touch::Terminal_touch_selection_action::BEGIN_HANDLE,
            "the timeout fixture leaves a handle-begin request unacknowledged")) {
        return false;
    }
    // The 5000 ms coarse timer may round its deadline by 250 ms. Observe its
    // cancellation with the same 1000 ms delivery allowance as other events.
    if (!check(QTest::qWaitFor([&] {
            return requests.back().action == touch::Terminal_touch_selection_action::CANCEL_GESTURE &&
                requests.back().gesture_id == unacknowledged.gesture_id;
        }, 5000 + 1000), "the unacknowledged gesture is cancelled within a bounded wait")) {
        return false;
    }
    QTest::touchEvent(&window, device).release(0, QPoint(130, 87), &window);
    shown.selection.has_selection = false;
    shown.selection.touch_handles_visible = false;
    QTest::touchEvent(&window, device).press(0, QPoint(60, 30), &window);
    QTest::touchEvent(&window, device).release(0, QPoint(60, 30), &window);
    ok &= check(taps == 5 && requests.back().action == touch::Terminal_touch_selection_action::CANCEL_GESTURE,
        "an undelivered acknowledgement has a bounded wait and leaves new taps usable");

    shown.selection.has_selection = true;
    shown.selection.touch_handles_visible = true;
    QTest::touchEvent(&window, device).press(0, QPoint(130, 87), &window);
    complete(shown.selection.selection_generation);
    QTest::touchEvent(&window, device).move(0, QPoint(130, 3), &window);
    if (!check(QTest::qWaitFor([&] {
                return requests.back().action == touch::Terminal_touch_selection_action::SCROLL_EXTEND;
            }, 1000) &&
            requests.back().scroll_lines == 3,
            "dragging at the viewport edge requests shared scrolling")) {
        return false;
    }
    complete(shown.selection.selection_generation);
    const auto before_tick = requests.size();
    ok &= check(QTest::qWaitFor([&] { return requests.size() > before_tick; }, 1000) &&
        requests.back().action == touch::Terminal_touch_selection_action::SCROLL_EXTEND,
        "a held finger at the edge continues scrolling without another move event");
    item.controller->cancel();
    QTest::touchEvent(&window, device).release(0, QPoint(130, 3), &window);
    const auto before_copy = requests.size();
    const auto copy_id = item.controller->request_copy(shown.selection.selection_generation);
    ok &= check(copy_id != 0U && requests.size() == before_copy, "Copy returns its id before synchronous host dispatch");
    QCoreApplication::processEvents();
    ok &= check(requests.back().request_id == copy_id && requests.back().action == touch::Terminal_touch_selection_action::COPY,
        "queued Copy dispatch retains the returned correlation id");
    return ok;
}

bool moving_viewport_contract()
{
    QQuickWindow window;
    window.resize(200, 180);
    Touch_item item(window.contentItem());
    item.setY(40);
    term::Terminal_touch_presentation shown;
    shown.cell_metrics = {10, 20, 14, 6};
    shown.selection.source = {1U, touch::Terminal_selection_buffer::PRIMARY_SCREEN, 1U, 5, 20};
    shown.selection.selection_generation = 1U;
    for (int row = 0; row < 5; ++row) {
        shown.selection.visible_rows.push_back({(std::uint64_t)row + 1U, 1U, 0});
    }
    int taps = 0;
    int menus = 0;
    int dismissals = 0;
    std::vector<touch::Terminal_touch_selection_request> requests;
    item.controller = std::make_unique<term::Terminal_touch_controller>(item,
        term::Terminal_touch_controller::Callbacks{
            [&] { return std::optional(shown); },
            [&](const auto& request) { requests.push_back(request); },
            [&] { ++taps; },
            [&](touch::Terminal_touch_menu, QRectF, std::uint64_t) { ++menus; },
            [&] { ++dismissals; },
        }, true);
    const auto complete = [&](std::uint64_t generation) {
        const auto request = requests.back();
        item.controller->complete({request.request_id, request.action,
            touch::Terminal_touch_selection_status::OK, generation, {}, {}});
    };
    item.controller->set_viewport(QRectF(0, 0, 200, 100));
    window.show();
    QTest::qWait(30);
    auto* device = QTest::createTouchDevice();
    QGuiApplication::styleHints()->setMousePressAndHoldInterval(80);
    auto stationary_touch = QTest::touchEvent(&window, device, false);
    stationary_touch.press(0, QPoint(60, 70), &window).commit();
    item.setY(0);
    item.controller->set_viewport(QRectF(0, 10, 200, 90));
    stationary_touch.stationary(0).commit();
    if (!check(QTest::qWaitFor([&] { return !requests.empty(); }, 1000) &&
            requests.back().action == touch::Terminal_touch_selection_action::SELECT_WORD &&
            requests.back().target.row.retained_line_id == 4U && taps == 0,
            "incoming output moves content beneath a held screen point without cancelling or inventing finger travel")) {
        return false;
    }
    item.controller->cancel();
    stationary_touch.release(0, QPoint(60, 70), &window).commit();

    shown.selection.selection_generation = 3U;
    shown.selection.has_selection = true;
    shown.selection.touch_handles_visible = true;
    shown.selection.start = {2, 3, true};
    shown.selection.end = {2, 10, true};
    shown.selection.spans = {{2, 3, 7}};
    item.setY(20);
    item.controller->set_viewport(QRectF(0, 0, 200, 100));
    QTest::touchEvent(&window, device).press(0, QPoint(100, 87), &window);
    bool ok = check(requests.back().action == touch::Terminal_touch_selection_action::BEGIN_HANDLE,
        "translated content still hits the shared handle");
    complete(3U);
    QTest::touchEvent(&window, device).move(0, QPoint(130, 15), &window);
    if (!check(QTest::qWaitFor([&] {
                return requests.back().action == touch::Terminal_touch_selection_action::SCROLL_EXTEND;
            }, 1000), "the translated handle reaches the shared viewport edge")) {
        return false;
    }
    item.setY(-60);
    item.controller->set_viewport(QRectF(0, 70, 200, 30));
    shown.selection.selection_generation = 4U;
    for (auto& row : shown.selection.visible_rows) { row.retained_line_id += 10U; }
    item.controller->presentation_changed();
    complete(4U);
    const auto before_tick = requests.size();
    ok &= check(QTest::qWaitFor([&] { return requests.size() > before_tick; }, 1000) &&
        requests.back().action == touch::Terminal_touch_selection_action::SCROLL_EXTEND &&
        requests.back().scroll_lines == 3 && requests.back().target.row.retained_line_id == 13U &&
        requests.back().target.column == 13,
        "shared edge scroll continues at a stationary screen point after content origin and clipping change");
    shown.selection.selection_generation = 5U;
    shown.selection.end = {3, 13, true};
    shown.selection.spans = {{2, 3, 17}, {3, 0, 13}};
    complete(5U);
    QTest::touchEvent(&window, device).release(0, QPoint(130, 35), &window);
    complete(5U);
    complete(5U);
    ok &= check(menus == 1 && taps == 0 && dismissals == 0,
        "moving viewport preserves the released Copy menu without keyboard activation");
    item.controller->set_viewport(QRectF(0, 75, 200, 25));
    ok &= check(dismissals == 0, "clipping-only updates preserve an otherwise valid Copy menu");
    ++shown.selection.source.grid_reflow_generation;
    item.controller->presentation_changed();
    ok &= check(dismissals == 1, "actual source reflow still dismisses the old menu");
    return ok;
}

bool optional_projection_contract()
{
    auto frame = std::make_shared<touch::Terminal_canvas_frame>();
    frame->rows = 2;
    frame->columns = 12;
    frame->cell_width = 10;
    frame->cell_height = 20;
    frame->content_width = 120;
    frame->content_height = 40;
    frame->sequence = 1U;
    frame->default_background_rgba = 0xff091018U;
    frame->styles.push_back({0xffffffffU, 0xff091018U, 0U});
    frame->cells.push_back({0, 0, 1, 0U, QStringLiteral("A")});
    touch::Terminal_canvas_selection initial;
    initial.source = {1U, touch::Terminal_selection_buffer::PRIMARY_SCREEN, 1U, 2, 12};
    initial.selection_generation = 1U;
    initial.visible_rows = {{1U, 0U, 0}, {2U, 0U, 0}};
    bool ok = check(touch::touch_selection_codec::decode_frame(touch::touch_selection_codec::encode_frame(initial)) == initial,
        "an initial unselected source round-trips and supports first selection");
    VNM_TerminalCanvas canvas;
    frame->selection = initial;
    ok &= check(canvas.set_canvas_frame(frame) && canvas.canvas_frame()->selection.has_value(),
        "valid optional source installs before any selection exists");
    for (int defect = 0; defect < 4; ++defect) {
        auto malformed = initial;
        if (defect == 0) { malformed.visible_rows[0].retained_line_id = 0U; }
        if (defect == 1) { malformed.visible_rows[0].visual_fragment_index = 1; }
        if (defect == 2) { malformed.selection_generation = 0U; }
        if (defect == 3) { malformed.spans = {{0, 0, 1}}; }
        frame->selection = malformed;
        ++frame->sequence;
        ok &= check(!touch::touch_selection_codec::decode_frame(touch::touch_selection_codec::encode_frame(malformed)),
            "the wire decoder rejects the same malformed record as the common validator");
        ok &= check(canvas.set_canvas_frame(frame) && !canvas.canvas_frame()->selection &&
            canvas.frame_sequence() == frame->sequence,
            "malformed optional selection cannot reject or freeze base terminal text");
    }
    frame->selection = initial;
    frame->selection->record_version = 2U;
    ok &= check(canvas.set_canvas_frame(frame), "unknown selection versions leave base text available");
    frame->selection.reset();
    ok &= check(canvas.set_canvas_frame(frame), "absent optional metadata leaves base text available");
    touch::Terminal_touch_selection_result copy{9U, touch::Terminal_touch_selection_action::COPY,
        touch::Terminal_touch_selection_status::OK, 1U,
        QString(touch::touch_selection_codec::k_max_copy_text_bytes + 1, QLatin1Char('x')), {}};
    const auto limited = touch::touch_selection_codec::decode_result(touch::touch_selection_codec::encode_result(copy));
    ok &= check(limited && limited->status == touch::Terminal_touch_selection_status::OVER_LIMIT && limited->text.isEmpty(),
        "copy over the wire limit is rejected without truncation");

    term::Terminal_viewport_state viewport;
    viewport.visible_rows = 2;
    auto snapshot = std::make_shared<term::Terminal_render_snapshot>(term::make_empty_render_snapshot({2, 12}, viewport, 1U));
    initial.has_selection = true;
    initial.touch_handles_visible = true;
    initial.start = {0, 0, true};
    initial.end = {1, 1, true};
    initial.spans = {{0, 0, 12}, {1, 0, 1}};
    snapshot->touch_selection = initial;
    snapshot->cursor.position = {1, 4};
    snapshot->cursor.visible = true;
    snapshot->cursor.blink_enabled = true;
    const term::terminal_cell_metrics_t metrics{10, 20, 14, 6};
    term::Terminal_render_options options;
    options.selection_background = QColor(Qt::blue);
    options.selection_foreground = QColor(Qt::white);
    const auto rendered = term::build_terminal_render_frame(snapshot.get(), QSizeF(120, 40), metrics, options, false);
    ok &= check(rendered.overlay_rects.empty() && !rendered.touch_handle_rects.empty() &&
        rendered.touch_handle_rects.back().color == options.selection_foreground,
        "shared handles contrast with selection and remain separate from visual-bell overlays");
    term::Captured_atlas_frame capture;
    capture.snapshot = snapshot;
    capture.cell_metrics = metrics;
    capture.options = options;
    capture.cursor_blink_visible = false;
    term::Qsg_atlas_recorder recorder;
    recorder.record_render(capture, 1U, QRect(0, 0, 120, 40), true);
    ok &= check(recorder.snapshot().rendered_touch_block_cursor == QRectF(40, 20, 10, 20),
        "the drawn block cursor remains a touch target during its blink-off phase");
    auto next_snapshot = std::make_shared<term::Terminal_render_snapshot>(*snapshot);
    next_snapshot->touch_selection->selection_generation = 2U;
    next_snapshot->cursor.position.column = 5;
    capture.snapshot = next_snapshot;
    capture.cell_metrics.width = 12;
    recorder.record_capture(capture);
    const auto drawn_metrics = recorder.snapshot().rendered_touch_cell_metrics;
    ok &= check(recorder.snapshot().rendered_touch_selection->selection_generation == 1U &&
        drawn_metrics.width == metrics.width && drawn_metrics.height == metrics.height &&
        drawn_metrics.ascent == metrics.ascent && drawn_metrics.descent == metrics.descent,
        "new capture preparation retains the actual drawn touch source and geometry");
    ok &= check(recorder.snapshot().rendered_touch_block_cursor == QRectF(40, 20, 10, 20),
        "a newer cursor capture cannot get ahead of the drawn touch source");
    recorder.record_render(capture, 2U, QRect(0, 0, 120, 40), true);
    ok &= check(recorder.snapshot().rendered_touch_block_cursor == QRectF(60, 20, 12, 20),
        "a drawn successor updates cursor position and cell geometry together");
    capture.options.cursor_shape_override = term::Terminal_cursor_shape::BAR;
    recorder.record_render(capture, 3U, QRect(0, 0, 120, 40), true);
    ok &= check(recorder.snapshot().rendered_touch_block_cursor.isEmpty(),
        "a bar cursor does not get the block-cursor Paste exception");
    capture.options.cursor_shape_override = term::Terminal_cursor_shape::BLOCK;
    capture.options.cursor_presentation_suppressed = true;
    recorder.record_render(capture, 4U, QRect(0, 0, 120, 40), true);
    ok &= check(recorder.snapshot().rendered_touch_block_cursor.isEmpty(),
        "a presentation-suppressed cursor cannot become a touch target");
    capture.options.cursor_presentation_suppressed = false;
    next_snapshot->cursor.visible = false;
    recorder.record_render(capture, 5U, QRect(0, 0, 120, 40), true);
    ok &= check(recorder.snapshot().rendered_touch_block_cursor.isEmpty(),
        "a logically hidden cursor cannot become a touch target");
    VNM_TerminalSurface surface;
    const auto surface_metrics = term::VNM_TerminalSurface_render_bridge::cell_metrics(surface);
    surface.setWidth(12 * surface_metrics.width);
    surface.setHeight(2 * surface_metrics.height);
    term::VNM_TerminalSurface_render_bridge::set_render_snapshot(surface, snapshot);
    const auto exported = touch::export_terminal_canvas_frame(surface);
    ok &= check(exported.frame && exported.frame->selection &&
        exported.frame->selection->spans == initial.spans && exported.frame->selection->source == initial.source,
        "public export carries shared endpoint, highlight and row provenance metadata");
    return ok;
}

bool owner_hold_contract()
{
    auto backend = std::make_unique<Touch_backend>();
    auto* output = backend.get();
    term::Terminal_session_config config;
    config.selection_viewport_projection_enabled = true;
    config.synchronized_output_scroll_policy = term::Terminal_synchronized_output_scroll_policy::IMMEDIATE_PUBLIC_PROJECTION;
    term::Terminal_session session(std::move(backend), config);
    term::Terminal_launch_config launch;
    launch.argv = {QStringLiteral("touch-hold-fixture")};
    launch.initial_grid_size = term::terminal_grid_size_t{2, 20};
    bool ok = check(session.start(launch).code == term::Terminal_session_result_code::ACCEPTED, "held owner fixture starts");
    output->output(QByteArrayLiteral("alpha beta"));
    session.process_backend_callback_events();
    auto snapshot = session.latest_render_snapshot_handle();
    const auto first = request_at(*snapshot, touch::Terminal_touch_selection_action::SELECT_WORD, 0, 1, 1U);
    ok &= check(session.apply_touch_selection(first).status == touch::Terminal_touch_selection_status::OK, "prior owner word selected");
    auto cancellation = first;
    cancellation.action = touch::Terminal_touch_selection_action::CANCEL_GESTURE;
    cancellation.request_id = 2U;
    ok &= check(session.apply_touch_selection(cancellation).status == touch::Terminal_touch_selection_status::OK &&
        !session.latest_render_snapshot_handle()->touch_selection->touch_handles_visible,
        "ordered cancellation succeeds when the selection acknowledgement and its new generation were lost");
    output->output(QByteArrayLiteral("\x1b[?2026h"));
    session.process_backend_callback_events();
    snapshot = session.latest_render_snapshot_handle();
    const auto held = request_at(*snapshot, touch::Terminal_touch_selection_action::SELECT_WORD, 0, 7, 3U);
    ok &= check(session.apply_touch_selection(held).status == touch::Terminal_touch_selection_status::UNAVAILABLE &&
        session.selected_text().text == QStringLiteral("alpha"),
        "public projection hold cannot report a requested word installed over an earlier selection");
    output->output(QByteArrayLiteral("\x1b[?2026l"));
    session.process_backend_callback_events();
    snapshot = session.latest_render_snapshot_handle();
    const auto blank = request_at(*snapshot, touch::Terminal_touch_selection_action::SELECT_WORD, 0, 15, 4U);
    ok &= check(session.apply_touch_selection(blank).status == touch::Terminal_touch_selection_status::UNAVAILABLE,
        "blank terminal padding is not presented as a word to copy");
    return ok;
}

bool item_touch_routing_contract()
{
    const auto exercise = [](auto& item, QQuickWindow& window) {
        using Item = std::decay_t<decltype(item)>;
        int taps = 0;
        int paste_menus = 0;
        QPointF paste_scene_position;
        QObject::connect(&item, &Item::terminal_tapped, &item, [&] { ++taps; });
        QObject::connect(&item, &Item::touch_context_menu_requested, &item,
            [&](touch::Terminal_touch_menu menu, QRectF rect, qulonglong) {
                if (menu == touch::Terminal_touch_menu::PASTE) {
                    ++paste_menus;
                    paste_scene_position = item.mapToScene(rect.center());
                }
            });
        item.setWidth(200);
        item.setHeight(100);
        item.set_touch_selection_enabled(true);
        window.resize(200, 100);
        window.show();
        QTest::qWait(30);
        auto* device = QTest::createTouchDevice();
        QGuiApplication::styleHints()->setMousePressAndHoldInterval(1000);
        auto stationary_touch = QTest::touchEvent(&window, device, false);
        stationary_touch.press(0, QPoint(60, 30), &window).commit();
        item.setY(-20);
        item.set_touch_viewport(QRectF(0, 20, 200, 80));
        stationary_touch.stationary(0).commit();
        stationary_touch.release(0, QPoint(60, 30), &window).commit();
        bool ok = check(taps == 1, "actual moving Surface or Canvas preserves short-touch typing activation");
        QGuiApplication::styleHints()->setMousePressAndHoldInterval(20);
        QTest::touchEvent(&window, device).press(0, QPoint(60, 30), &window);
        item.setY(0);
        item.set_touch_viewport(QRectF(0, 0, 200, 100));
        if (!wait_for_touch_hold(item)) {
            return false;
        }
        QTest::touchEvent(&window, device).release(0, QPoint(60, 30), &window);
        ok &= check(taps == 1 && paste_menus == 1 && paste_scene_position == QPointF(60, 30),
            "actual moving item routes Paste at the held screen point without a short tap");
        item.set_touch_selection_enabled(false);
        QTest::touchEvent(&window, device).press(0, QPoint(60, 30), &window);
        QTest::touchEvent(&window, device).release(0, QPoint(60, 30), &window);
        item.set_touch_selection_enabled(true);
        QGuiApplication::styleHints()->setMousePressAndHoldInterval(1000);
        QTest::touchEvent(&window, device).press(0, QPoint(60, 30), &window);
        QTest::touchEvent(&window, device).release(0, QPoint(60, 30), &window);
        ok &= check(taps == 2, "actual item enable lifecycle discards disabled gestures and restores taps");
        QObject::disconnect(&item, nullptr, &item, nullptr);
        return ok;
    };
    QQuickWindow canvas_window;
    VNM_TerminalCanvas canvas(canvas_window.contentItem());
    bool ok = exercise(canvas, canvas_window);
    canvas_window.hide();
    QQuickWindow surface_window;
    VNM_TerminalSurface surface(surface_window.contentItem());
    ok &= exercise(surface, surface_window);
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    bool ok = owner_selection_contract();
    ok &= touch_intent_contract();
    ok &= direct_word_hold_contract();
    ok &= cursor_hold_contract();
    ok &= touch_lifecycle_contract();
    ok &= moving_viewport_contract();
    ok &= optional_projection_contract();
    ok &= owner_hold_contract();
    ok &= item_touch_routing_contract();
    return ok ? 0 : 1;
}
