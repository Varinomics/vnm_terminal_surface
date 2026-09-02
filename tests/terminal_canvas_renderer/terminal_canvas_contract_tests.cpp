#include "helpers/test_check.h"
#include "vnm_terminal/terminal_canvas_frame.h"
#include "vnm_terminal/vnm_terminal_canvas.h"

#include <QGuiApplication>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace {

using vnm_terminal::test_helpers::check;

std::shared_ptr<vnm_terminal::Terminal_canvas_frame> make_frame(
    std::uint64_t sequence)
{
    auto frame = std::make_shared<vnm_terminal::Terminal_canvas_frame>();
    frame->rows                    = 2;
    frame->columns                 = 12;
    frame->cell_width              = 10.0;
    frame->cell_height             = 20.0;
    frame->content_width           = 120.0;
    frame->content_height          = 40.0;
    frame->sequence                = sequence;
    frame->default_foreground_rgba = 0xffffffffU;
    frame->default_background_rgba = 0xff091018U;
    frame->styles.push_back({
        frame->default_foreground_rgba,
        frame->default_background_rgba,
        0U,
    });
    frame->cells.push_back({0, 0, 1, 0U, QStringLiteral("A")});
    frame->content_extent.emplace();
    return frame;
}

std::shared_ptr<vnm_terminal::Terminal_canvas_frame> make_text_frame(
    std::uint64_t        sequence,
    std::vector<QString> cell_text)
{
    auto frame = make_frame(sequence);
    frame->rows    = 1;
    frame->columns = static_cast<int>(cell_text.size());
    frame->content_width  = frame->cell_width * frame->columns;
    frame->content_height = frame->cell_height;
    frame->cells.clear();
    for (std::size_t index = 0U; index < cell_text.size(); ++index) {
        frame->cells.push_back({
            0,
            static_cast<int>(index),
            1,
            0U,
            std::move(cell_text[index]),
        });
    }
    return frame;
}

bool test_frame_contract_and_text_bounds()
{
    bool               ok = true;
    VNM_TerminalCanvas canvas;
    ok &= check(canvas.flags().testFlag(QQuickItem::ItemHasContents),
        "canvas declares scene-graph content");
    ok &= check(canvas.clip(), "canvas clips rendering to its bounded item");
    ok &= check(canvas.childItems().empty(),
        "canvas does not materialize per-cell child items");

    const std::shared_ptr<vnm_terminal::Terminal_canvas_frame> source =
        make_frame(11U);
    ok &= check(canvas.set_canvas_frame(source), "valid public frame is accepted");
    ok &= check(
        canvas.rows() == 2 && canvas.columns() == 12 &&
        canvas.frame_sequence() == 11U,
        "accepted frame publishes read-only item state");
    ok &= check(canvas.implicitWidth() > 0.0 && canvas.implicitHeight() > 0.0,
        "accepted frame publishes natural grid dimensions");

    source->sequence = 99U;
    ok &= check(canvas.frame_sequence() == 11U,
        "item owns an immutable copy of an accepted frame");

    auto invalid = make_frame(12U);
    invalid->api_version = vnm_terminal::k_terminal_canvas_frame_api_version + 1U;
    ok &= check(!canvas.set_canvas_frame(invalid),
        "unsupported frame version is rejected");
    ok &= check(canvas.frame_sequence() == 11U,
        "rejected frame preserves the prior accepted canvas");

    auto invalid_cursor = make_frame(12U);
    invalid_cursor->cursor.shape =
        static_cast<vnm_terminal::Terminal_canvas_cursor_shape>(0xffU);
    ok &= check(!canvas.set_canvas_frame(invalid_cursor),
        "unknown cursor shape is rejected");
    ok &= check(canvas.frame_sequence() == 11U,
        "cursor-shape rejection preserves the prior accepted canvas");

    const QString exact_cell_text(
        vnm_terminal::k_terminal_canvas_max_cell_text_utf16_code_units,
        QLatin1Char('x'));
    ok &= check(
        canvas.set_canvas_frame(make_text_frame(12U, {exact_cell_text})),
        "exact per-cell UTF-16 code-unit limit is accepted");

    QString excess_cell_text = exact_cell_text;
    excess_cell_text += QLatin1Char('x');
    ok &= check(
        !canvas.set_canvas_frame(make_text_frame(13U, {excess_cell_text})),
        "per-cell UTF-16 code-unit limit plus one is rejected");
    ok &= check(canvas.frame_sequence() == 12U,
        "per-cell text rejection preserves the prior accepted frame");

    constexpr qsizetype k_multibyte_utf8_bytes_per_code_unit = 2;
    static_assert(
        vnm_terminal::k_terminal_canvas_max_frame_text_utf8_bytes %
            (vnm_terminal::k_terminal_canvas_max_cell_text_utf16_code_units *
                k_multibyte_utf8_bytes_per_code_unit) == 0);
    const qsizetype aggregate_cell_count =
        vnm_terminal::k_terminal_canvas_max_frame_text_utf8_bytes /
        (vnm_terminal::k_terminal_canvas_max_cell_text_utf16_code_units *
            k_multibyte_utf8_bytes_per_code_unit);
    const QString multibyte_cell_text(
        vnm_terminal::k_terminal_canvas_max_cell_text_utf16_code_units,
        QChar(0x00e9U));
    std::vector<QString> exact_frame_text(
        static_cast<std::size_t>(aggregate_cell_count),
        multibyte_cell_text);
    ok &= check(
        canvas.set_canvas_frame(make_text_frame(13U, exact_frame_text)),
        "exact aggregate UTF-8 byte limit accepts multibyte text");

    exact_frame_text.push_back(QStringLiteral("x"));
    ok &= check(
        !canvas.set_canvas_frame(make_text_frame(14U, std::move(exact_frame_text))),
        "aggregate UTF-8 byte limit plus one is rejected");
    ok &= check(canvas.frame_sequence() == 13U,
        "aggregate text rejection preserves the prior accepted frame");

    ok &= check(canvas.set_canvas_frame({}), "null frame clears the canvas");
    ok &= check(canvas.rows() == 0 && canvas.columns() == 0,
        "clear releases published canvas geometry");
    return ok;
}

bool test_content_extent_properties_and_atomic_lifecycle()
{
    bool               ok = true;
    VNM_TerminalCanvas canvas;
    int                frame_change_count = 0;
    QObject::connect(
        &canvas,
        &VNM_TerminalCanvas::frame_changed,
        [&frame_change_count]() { ++frame_change_count; });

    ok &= check(
        canvas.cell_height() == 0.0 &&
        !canvas.content_extent_available() &&
        canvas.content_bottom_row_exclusive() == 0 &&
        canvas.scrollback_rows() == 0 &&
        canvas.viewport_offset_from_tail() == 0 &&
        !canvas.alternate_screen(),
        "canvas exposes neutral semantic properties without an accepted frame");

    auto accepted = make_frame(100U);
    accepted->content_extent->scrollback_rows  = 7;
    accepted->content_extent->offset_from_tail = 3;
    ok &= check(canvas.set_canvas_frame(accepted),
        "known coherent semantic extent is accepted");
    ok &= check(
        frame_change_count == 1 &&
        canvas.cell_height() == 20.0 &&
        canvas.content_extent_available() &&
        canvas.content_bottom_row_exclusive() == 1 &&
        canvas.scrollback_rows() == 7 &&
        canvas.viewport_offset_from_tail() == 3 &&
        !canvas.alternate_screen(),
        "accepted frame publishes all semantic properties in one transaction");

    accepted->cell_height = 99.0;
    accepted->content_extent->content_bottom_row_exclusive = 2;
    accepted->content_extent->scrollback_rows               = 99;
    accepted->content_extent->offset_from_tail               = 98;
    accepted->content_extent->active_buffer =
        vnm_terminal::Terminal_canvas_buffer::ALTERNATE_BUFFER;
    ok &= check(
        canvas.cell_height() == 20.0 &&
        canvas.content_bottom_row_exclusive() == 1 &&
        canvas.scrollback_rows() == 7 &&
        canvas.viewport_offset_from_tail() == 3 &&
        !canvas.alternate_screen(),
        "semantic getters read the installed immutable frame copy");

    const auto expect_rejected =
        [&canvas, &frame_change_count](
            std::shared_ptr<vnm_terminal::Terminal_canvas_frame> candidate,
            const char*                                         message)
        {
            const bool rejected = !canvas.set_canvas_frame(std::move(candidate));
            return check(
                rejected &&
                frame_change_count == 1 &&
                canvas.frame_sequence() == 100U &&
                canvas.content_extent_available() &&
                canvas.content_bottom_row_exclusive() == 1 &&
                canvas.scrollback_rows() == 7 &&
                canvas.viewport_offset_from_tail() == 3 &&
                !canvas.alternate_screen(),
                message);
        };

    auto invalid_bottom_low = make_frame(101U);
    invalid_bottom_low->content_extent->content_bottom_row_exclusive = 0;
    ok &= expect_rejected(
        std::move(invalid_bottom_low),
        "bottom below one rejects without changing accepted properties");

    auto invalid_bottom_high = make_frame(102U);
    invalid_bottom_high->content_extent->content_bottom_row_exclusive = 3;
    ok &= expect_rejected(
        std::move(invalid_bottom_high),
        "bottom beyond frame rows rejects without changing accepted properties");

    auto incoherent_bottom = make_frame(103U);
    incoherent_bottom->content_extent->content_bottom_row_exclusive = 2;
    ok &= expect_rejected(
        std::move(incoherent_bottom),
        "plausible but snapshot-incoherent bottom is rejected exactly");

    auto invalid_scrollback = make_frame(104U);
    invalid_scrollback->content_extent->scrollback_rows = -1;
    ok &= expect_rejected(
        std::move(invalid_scrollback),
        "negative scrollback rejects without changing accepted properties");

    auto excessive_scrollback = make_frame(105U);
    const int maximum_safe_scrollback =
        std::numeric_limits<int>::max() - (excessive_scrollback->rows - 1);
    excessive_scrollback->content_extent->scrollback_rows =
        maximum_safe_scrollback + 1;
    ok &= expect_rejected(
        std::move(excessive_scrollback),
        "scrollback that can overflow a viewport logical row is rejected");

    auto invalid_offset_low = make_frame(106U);
    invalid_offset_low->content_extent->offset_from_tail = -1;
    ok &= expect_rejected(
        std::move(invalid_offset_low),
        "negative offset rejects without changing accepted properties");

    auto invalid_offset_high = make_frame(107U);
    invalid_offset_high->content_extent->scrollback_rows  = 2;
    invalid_offset_high->content_extent->offset_from_tail = 3;
    ok &= expect_rejected(
        std::move(invalid_offset_high),
        "offset beyond scrollback rejects without changing accepted properties");

    auto invalid_alternate_scrollback = make_frame(108U);
    invalid_alternate_scrollback->content_extent->active_buffer =
        vnm_terminal::Terminal_canvas_buffer::ALTERNATE_BUFFER;
    invalid_alternate_scrollback->content_extent->scrollback_rows = 1;
    ok &= expect_rejected(
        std::move(invalid_alternate_scrollback),
        "alternate scrollback rejects without changing accepted properties");

    auto invalid_alternate_offset = make_frame(109U);
    invalid_alternate_offset->content_extent->active_buffer =
        vnm_terminal::Terminal_canvas_buffer::ALTERNATE_BUFFER;
    invalid_alternate_offset->content_extent->scrollback_rows  = 1;
    invalid_alternate_offset->content_extent->offset_from_tail = 1;
    ok &= expect_rejected(
        std::move(invalid_alternate_offset),
        "alternate offset rejects without changing accepted properties");

    auto invalid_buffer = make_frame(110U);
    invalid_buffer->content_extent->active_buffer =
        static_cast<vnm_terminal::Terminal_canvas_buffer>(0xffU);
    ok &= expect_rejected(
        std::move(invalid_buffer),
        "unknown known-record buffer rejects without changing accepted properties");

    auto maximum_scrollback = make_frame(111U);
    maximum_scrollback->content_extent->scrollback_rows = maximum_safe_scrollback;
    ok &= check(
        canvas.set_canvas_frame(maximum_scrollback) &&
            canvas.scrollback_rows() == maximum_safe_scrollback,
        "largest scrollback with safe viewport logical rows is accepted");

    auto hidden_cursor = make_frame(112U);
    hidden_cursor->cursor.row     = 1;
    hidden_cursor->cursor.column  = 11;
    hidden_cursor->cursor.visible = false;
    hidden_cursor->content_extent->content_bottom_row_exclusive = 2;
    ok &= check(canvas.set_canvas_frame(hidden_cursor) &&
            canvas.content_bottom_row_exclusive() == 2,
        "hidden in-range semantic cursor participates in exact coherence");

    auto unknown_record = make_frame(113U);
    unknown_record->content_extent->record_version =
        vnm_terminal::k_terminal_canvas_content_extent_version + 1U;
    unknown_record->content_extent->content_bottom_row_exclusive = -4;
    unknown_record->content_extent->scrollback_rows               = -5;
    unknown_record->content_extent->offset_from_tail               = 99;
    unknown_record->content_extent->active_buffer =
        static_cast<vnm_terminal::Terminal_canvas_buffer>(0xffU);
    ok &= check(canvas.set_canvas_frame(unknown_record),
        "unknown semantic record version leaves the base canvas renderable");
    const std::shared_ptr<const vnm_terminal::Terminal_canvas_frame>
        stored_unknown_record = canvas.canvas_frame();
    ok &= check(
        stored_unknown_record != nullptr &&
            stored_unknown_record->content_extent.has_value() &&
            stored_unknown_record->content_extent->record_version ==
                vnm_terminal::k_terminal_canvas_content_extent_version + 1U &&
            stored_unknown_record->content_extent->content_bottom_row_exclusive == -4 &&
            stored_unknown_record->content_extent->scrollback_rows == -5 &&
            stored_unknown_record->content_extent->offset_from_tail == 99 &&
            stored_unknown_record->content_extent->active_buffer ==
                static_cast<vnm_terminal::Terminal_canvas_buffer>(0xffU),
        "unknown semantic record is preserved in the installed immutable frame");
    ok &= check(
        canvas.cell_height() == 20.0 &&
        !canvas.content_extent_available() &&
        canvas.content_bottom_row_exclusive() == 0 &&
        canvas.scrollback_rows() == 0 &&
        canvas.viewport_offset_from_tail() == 0 &&
        !canvas.alternate_screen(),
        "unknown semantic record exposes only neutral capability values");

    auto absent_record = make_frame(114U);
    absent_record->content_extent.reset();
    ok &= check(canvas.set_canvas_frame(absent_record),
        "absent semantic record leaves the base canvas renderable");
    ok &= check(
        canvas.cell_height() == 20.0 &&
        !canvas.content_extent_available() &&
        canvas.content_bottom_row_exclusive() == 0 &&
        canvas.scrollback_rows() == 0 &&
        canvas.viewport_offset_from_tail() == 0 &&
        !canvas.alternate_screen(),
        "absent semantic record exposes only neutral capability values");

    auto alternate = make_frame(115U);
    alternate->content_extent->active_buffer =
        vnm_terminal::Terminal_canvas_buffer::ALTERNATE_BUFFER;
    ok &= check(canvas.set_canvas_frame(alternate) && canvas.alternate_screen(),
        "coherent alternate extent materializes and exposes its buffer");

    const int changes_before_clear = frame_change_count;
    ok &= check(canvas.set_canvas_frame({}), "explicit null clears accepted extent");
    ok &= check(
        frame_change_count == changes_before_clear + 1 &&
        canvas.rows() == 0 &&
        canvas.columns() == 0 &&
        canvas.cell_height() == 0.0 &&
        !canvas.content_extent_available() &&
        canvas.content_bottom_row_exclusive() == 0 &&
        canvas.scrollback_rows() == 0 &&
        canvas.viewport_offset_from_tail() == 0 &&
        !canvas.alternate_screen(),
        "one clear transaction resets geometry and all semantic properties");
    return ok;
}

bool test_cursor_blink_phase_and_lifecycle()
{
    bool             ok = true;
    QPointer<QTimer> timer_lifetime;
    std::vector<bool> phases;
    {
        VNM_TerminalCanvas canvas;
        QTimer* const      timer = canvas.findChild<QTimer*>();
        timer_lifetime = timer;
        ok &= check(timer != nullptr, "canvas owns a cursor blink timer");
        if (timer == nullptr) {
            return false;
        }

        QObject::connect(
            &canvas,
            &VNM_TerminalCanvas::cursor_blink_phase_changed,
            [&phases](bool visible) { phases.push_back(visible); });

        auto blinking = make_frame(20U);
        blinking->cursor = {
            0,
            0,
            vnm_terminal::Terminal_canvas_cursor_shape::BLOCK,
            true,
            true,
        };
        ok &= check(canvas.set_canvas_frame(blinking),
            "blinking cursor frame is accepted");
        ok &= check(timer->isActive(),
            "accepted blinking cursor starts the canvas-owned timer");
        ok &= check(timer->interval() == 500,
            "canvas-owned blink cadence is 500 ms");
        const int active_timer_id = timer->timerId();
        bool      replacements_accepted = true;
        for (std::uint64_t sequence = 21U; sequence <= 23U; ++sequence) {
            auto replacement = make_frame(sequence);
            replacement->cursor = blinking->cursor;
            replacements_accepted &= canvas.set_canvas_frame(replacement);
        }
        ok &= check(
            replacements_accepted && canvas.frame_sequence() == 23U,
            "frequent blink-enabled replacements publish the latest frame");
        ok &= check(timer->timerId() == active_timer_id && phases.empty(),
            "blink-enabled replacements preserve cadence and visible phase");

        ok &= check(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection),
            "blink timer timeout can be delivered deterministically");
        ok &= check(phases.size() == 1U && !phases[0],
            "first blink phase hides the cursor");

        replacements_accepted = true;
        for (std::uint64_t sequence = 24U; sequence <= 26U; ++sequence) {
            auto replacement = make_frame(sequence);
            replacement->cursor = blinking->cursor;
            replacements_accepted &= canvas.set_canvas_frame(replacement);
        }
        ok &= check(
            replacements_accepted && canvas.frame_sequence() == 26U,
            "frequent hidden-phase replacements publish the latest frame");
        ok &= check(
            timer->timerId() == active_timer_id &&
            phases.size() == 1U && !phases[0],
            "hidden phase and original timer cadence survive frame replacement");

        ok &= check(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection),
            "second blink timer timeout can be delivered deterministically");
        ok &= check(phases.size() == 2U && phases[1],
            "second blink phase restores the cursor");

        auto steady = make_frame(27U);
        steady->cursor.visible       = true;
        steady->cursor.blink_enabled = false;
        ok &= check(canvas.set_canvas_frame(steady),
            "steady cursor frame is accepted");
        ok &= check(!timer->isActive(),
            "blink-disabled cursor stops the canvas-owned timer");
        const std::size_t phase_count = phases.size();
        ok &= check(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection),
            "stopped timer signal remains safely deliverable during teardown checks");
        ok &= check(phases.size() == phase_count,
            "stopped timer cannot advance a non-blinking cursor phase");

        auto restarted = make_frame(28U);
        restarted->cursor = blinking->cursor;
        ok &= check(canvas.set_canvas_frame(restarted),
            "blinking cursor can restart after a steady frame");
        ok &= check(timer->isActive(), "restarted blink timer is active");
        ok &= check(canvas.set_canvas_frame({}), "clearing a blinking frame succeeds");
        ok &= check(!timer->isActive(), "clearing the frame stops the blink timer");
    }

    ok &= check(timer_lifetime.isNull(),
        "destroying the canvas releases its blink timer");
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    bool            ok = true;
    ok &= test_frame_contract_and_text_bounds();
    ok &= test_content_extent_properties_and_atomic_lifecycle();
    ok &= test_cursor_blink_phase_and_lifecycle();
    return ok ? 0 : 1;
}
