#include "vnm_terminal/internal/selection_contract.h"

#include <QStringList>
#include <algorithm>
#include <utility>

namespace vnm_terminal::internal {

namespace {

terminal_grid_position_t normalized_start(const Terminal_selection_range& range)
{
    if (range.start.row < range.end.row) {
        return range.start;
    }

    if (range.start.row > range.end.row) {
        return range.end;
    }

    return range.start.column <= range.end.column ? range.start : range.end;
}

terminal_grid_position_t normalized_end(const Terminal_selection_range& range)
{
    if (range.start.row < range.end.row) {
        return range.end;
    }

    if (range.start.row > range.end.row) {
        return range.start;
    }

    return range.start.column <= range.end.column ? range.end : range.start;
}

int first_selected_row(const Terminal_selection_range& range)
{
    return std::min(range.start.row, range.end.row);
}

int last_selected_row(const Terminal_selection_range& range)
{
    return std::max(range.start.row, range.end.row);
}

}

void Selection_contract_controller::begin(terminal_grid_position_t anchor)
{
    m_range          = {anchor, anchor, Terminal_selection_mode::NORMAL};
    m_selected_text.reset();
    clear_visual_lease();
    m_durable_payload_identity     = 0U;
    m_provisional_payload_identity = next_payload_identity();
    m_internal_state               = Terminal_selection_internal_state::DRAG_ARMED;
    m_has_selection                = true;
}

void Selection_contract_controller::extend(terminal_grid_position_t extent)
{
    m_range.end = extent;
    m_selected_text.reset();
    clear_visual_lease();
    if (m_internal_state == Terminal_selection_internal_state::DRAG_ARMED ||
        m_internal_state == Terminal_selection_internal_state::DRAG_PREVIEW)
    {
        m_internal_state = Terminal_selection_internal_state::DRAG_PREVIEW;
    }
}

void Selection_contract_controller::clear()
{
    m_has_selection  = false;
    m_internal_state = Terminal_selection_internal_state::NONE;
    m_range          = {};
    m_selected_text.reset();
    clear_payload_identity();
    clear_visual_lease();
}

void Selection_contract_controller::set_range(Terminal_selection_range range)
{
    if (range.mode == Terminal_selection_mode::NONE) {
        clear();
        return;
    }

    m_range          = range;
    m_selected_text.reset();
    clear_payload_identity();
    clear_visual_lease();
    m_internal_state = Terminal_selection_internal_state::ATTACHED_VISIBLE;
    m_has_selection  = true;
}

void Selection_contract_controller::set_range(
    Terminal_selection_range range,
    QString                  selected_text)
{
    set_range(range);
    if (m_has_selection) {
        m_selected_text                = std::move(selected_text);
        m_durable_payload_identity     = next_payload_identity();
        m_provisional_payload_identity = 0U;
    }
}

void Selection_contract_controller::set_range(
    Terminal_selection_range             range,
    QString                              selected_text,
    terminal_selection_visual_lease_t    visual_lease)
{
    set_range(range, std::move(selected_text));
    if (m_has_selection) {
        record_visual_lease(std::move(visual_lease));
    }
}

void Selection_contract_controller::hide_visual_attachment()
{
    if (!m_has_selection || !m_selected_text.has_value()) {
        return;
    }

    m_internal_state =
        m_visual_lease.has_value()
            ? Terminal_selection_internal_state::ATTACHED_HIDDEN
            : Terminal_selection_internal_state::PAYLOAD_ONLY;
}

void Selection_contract_controller::detach_visual_attachment()
{
    clear_visual_lease();
    if (!m_has_selection) {
        m_internal_state = Terminal_selection_internal_state::NONE;
        return;
    }

    m_internal_state =
        m_selected_text.has_value()
            ? Terminal_selection_internal_state::PAYLOAD_ONLY
            : Terminal_selection_internal_state::NONE;
    if (m_internal_state == Terminal_selection_internal_state::NONE) {
        m_has_selection = false;
    }
}

void Selection_contract_controller::update_visual_lease_source(
    terminal_selection_content_basis_t source_content_basis,
    std::uint64_t                      grid_reflow_basis,
    std::uint64_t                      row_origin_generation,
    terminal_grid_size_t               grid_size,
    const Terminal_viewport_state&     viewport_mapping)
{
    if (!m_visual_lease.has_value()) {
        return;
    }

    m_visual_lease->source_content_basis  = source_content_basis;
    m_visual_lease->grid_reflow_basis     = grid_reflow_basis;
    m_visual_lease->row_origin_generation = row_origin_generation;
    m_visual_lease->grid_size             = grid_size;
    m_visual_lease->viewport_mapping      = viewport_mapping;
}

bool Selection_contract_controller::apply_scrollback_eviction(int evicted_rows)
{
    if (!m_has_selection || evicted_rows <= 0) {
        return false;
    }

    if (first_selected_row(m_range) < evicted_rows) {
        m_range.mode = Terminal_selection_mode::NONE;
        clear_visual_lease();
        m_internal_state =
            m_selected_text.has_value()
                ? Terminal_selection_internal_state::PAYLOAD_ONLY
                : Terminal_selection_internal_state::NONE;
        if (m_internal_state == Terminal_selection_internal_state::NONE) {
            m_has_selection = false;
        }
        return true;
    }

    m_range.start.row -= evicted_rows;
    m_range.end.row   -= evicted_rows;
    if (m_visual_lease.has_value()) {
        m_visual_lease->selected_range = m_range;
        m_visual_lease->anchor         = m_range.start;
        m_visual_lease->extent         = m_range.end;
    }
    return true;
}

Terminal_selection_result Selection_contract_controller::selected_text() const
{
    if (!m_has_selection) {
        return {Terminal_selection_result_code::NO_SELECTION, {}};
    }

    if (!m_selected_text.has_value()) {
        return {Terminal_selection_result_code::INVALID_RANGE, {}};
    }

    return {Terminal_selection_result_code::OK, *m_selected_text};
}

Terminal_selection_result Selection_contract_controller::selected_text(
    std::span<const QString> logical_rows) const
{
    if (!m_has_selection) {
        return {Terminal_selection_result_code::NO_SELECTION, {}};
    }

    if (m_selected_text.has_value()) {
        return {Terminal_selection_result_code::OK, *m_selected_text};
    }

    const int first_row = first_selected_row(m_range);
    const int last_row  = last_selected_row(m_range);

    if (first_row < 0 || last_row >= static_cast<int>(logical_rows.size())) {
        return {Terminal_selection_result_code::INVALID_RANGE, {}};
    }

    if (first_row == last_row) {
        const int      left  = std::min(m_range.start.column, m_range.end.column);
        const int      right = std::max(m_range.start.column, m_range.end.column);
        const QString& text  = logical_rows[static_cast<std::size_t>(first_row)];

        if (left < 0 || right > text.size()) {
            return {Terminal_selection_result_code::INVALID_RANGE, {}};
        }

        return {Terminal_selection_result_code::OK, text.mid(left, right - left)};
    }

    const terminal_grid_position_t start = normalized_start(m_range);
    const terminal_grid_position_t end   = normalized_end(m_range);
    QStringList selected_rows;

    for (int row = first_row; row <= last_row; ++row) {
        const QString& text = logical_rows[static_cast<std::size_t>(row)];

        if (row == start.row) {
            if (start.column < 0 || start.column > text.size()) {
                return {Terminal_selection_result_code::INVALID_RANGE, {}};
            }

            selected_rows.push_back(text.mid(start.column));
        }
        else
        if (row == end.row) {
            if (end.column < 0 || end.column > text.size()) {
                return {Terminal_selection_result_code::INVALID_RANGE, {}};
            }

            selected_rows.push_back(text.left(end.column));
        }
        else {
            selected_rows.push_back(text);
        }
    }

    return {Terminal_selection_result_code::OK, selected_rows.join(QLatin1Char('\n'))};
}

std::uint64_t Selection_contract_controller::next_payload_identity()
{
    const std::uint64_t identity = m_next_payload_identity++;
    if (m_next_payload_identity == 0U) {
        m_next_payload_identity = 1U;
    }
    return identity;
}

void Selection_contract_controller::clear_payload_identity()
{
    m_durable_payload_identity     = 0U;
    m_provisional_payload_identity = 0U;
}

void Selection_contract_controller::clear_visual_lease()
{
    m_visual_lease.reset();
}

void Selection_contract_controller::record_visual_lease(
    terminal_selection_visual_lease_t visual_lease)
{
    visual_lease.selected_range                = m_range;
    visual_lease.anchor                        = m_range.start;
    visual_lease.extent                        = m_range.end;
    visual_lease.durable_payload_identity      = m_durable_payload_identity;
    visual_lease.provisional_payload_identity  = m_provisional_payload_identity;
    m_visual_lease                             = visual_lease;
}

}
