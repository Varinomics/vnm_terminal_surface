#pragma once

#include "vnm_terminal/internal/metrics_contract.h"
#include "vnm_terminal/internal/viewport_contract.h"
#include <QString>
#include <QByteArray>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace vnm_terminal::internal {

enum class Terminal_selection_mode
{
    NONE,
    NORMAL,
    WORD,
    LINE,
};

enum class Terminal_selection_result_code
{
    OK,
    NO_SELECTION,
    INVALID_RANGE,
};

enum class Terminal_osc52_policy
{
    DENY,
    REQUEST,
    ALLOW,
};

enum class Terminal_clipboard_response_decision
{
    DENY,
    ALLOW,
};

enum class Terminal_selection_internal_state
{
    NONE,
    DRAG_ARMED,
    DRAG_PREVIEW,
    ATTACHED_VISIBLE,
    ATTACHED_HIDDEN,
    PAYLOAD_ONLY,
};

enum class Terminal_selection_anchor_domain
{
    NONE,
    UNRESOLVED_ACTIVE_GRID,
    PRIMARY_BACKING,
    ALTERNATE_ACTIVE_GRID,
    PAYLOAD_ONLY,
};

enum class Terminal_selection_backing_event_kind
{
    NONE,
    PRIMARY_SCROLLBACK_EVICTION,
};

struct terminal_grid_position_t
{
    int                        row    = 0;
    int                        column = 0;
};

struct Terminal_selection_range
{
    terminal_grid_position_t   start;
    terminal_grid_position_t   end;
    Terminal_selection_mode    mode   = Terminal_selection_mode::NORMAL;
};

struct terminal_selection_content_basis_t
{
    std::uint64_t              content_generation     = 0U;
    std::uint64_t              grid_reflow_generation = 0U;
};

struct terminal_selection_source_identity_t
{
    terminal_selection_content_basis_t source_content_basis;
    Terminal_selection_anchor_domain   anchor_domain = Terminal_selection_anchor_domain::NONE;
    std::uint64_t                      session_epoch     = 0U;
    Terminal_buffer_id                 buffer_id         = Terminal_buffer_id::PRIMARY;
    std::uint64_t                      grid_reflow_basis = 0U;
    std::uint64_t                      row_origin_generation = 0U;
    terminal_grid_size_t               grid_size;
    Terminal_viewport_state            viewport_mapping;
};

struct terminal_selection_line_lease_t
{
    int                                row_offset         = 0;
    std::uint64_t                      retained_line_id   = 0U;
    std::uint64_t                      content_generation = 0U;
};

struct terminal_selection_backing_event_t
{
    Terminal_selection_backing_event_kind kind =
        Terminal_selection_backing_event_kind::NONE;
    int                                    evicted_rows = 0;
};

struct terminal_selection_visual_lease_t
{
    terminal_selection_content_basis_t source_content_basis;
    Terminal_selection_anchor_domain   anchor_domain = Terminal_selection_anchor_domain::NONE;
    std::uint64_t                      session_epoch = 0U;
    Terminal_buffer_id                 buffer_id     = Terminal_buffer_id::PRIMARY;
    std::uint64_t                      grid_reflow_basis = 0U;
    std::uint64_t                      row_origin_generation = 0U;
    terminal_grid_size_t               grid_size;
    Terminal_viewport_state            viewport_mapping;
    Terminal_selection_range           selected_range;
    terminal_grid_position_t           anchor;
    terminal_grid_position_t           extent;
    std::uint64_t                      durable_payload_identity     = 0U;
    std::uint64_t                      provisional_payload_identity = 0U;
    std::vector<terminal_selection_line_lease_t>
                                       selected_lines;
};

inline bool operator==(
    terminal_grid_position_t lhs,
    terminal_grid_position_t rhs)
{
    return lhs.row == rhs.row && lhs.column == rhs.column;
}

inline bool operator!=(
    terminal_grid_position_t lhs,
    terminal_grid_position_t rhs)
{
    return !(lhs == rhs);
}

inline bool operator==(
    const Terminal_selection_range& lhs,
    const Terminal_selection_range& rhs)
{
    return lhs.start == rhs.start && lhs.end == rhs.end && lhs.mode == rhs.mode;
}

inline bool operator!=(
    const Terminal_selection_range& lhs,
    const Terminal_selection_range& rhs)
{
    return !(lhs == rhs);
}

inline bool operator==(
    terminal_selection_content_basis_t lhs,
    terminal_selection_content_basis_t rhs)
{
    return
        lhs.content_generation     == rhs.content_generation &&
        lhs.grid_reflow_generation == rhs.grid_reflow_generation;
}

inline bool operator!=(
    terminal_selection_content_basis_t lhs,
    terminal_selection_content_basis_t rhs)
{
    return !(lhs == rhs);
}

inline bool operator==(
    terminal_selection_line_lease_t lhs,
    terminal_selection_line_lease_t rhs)
{
    return
        lhs.row_offset         == rhs.row_offset         &&
        lhs.retained_line_id   == rhs.retained_line_id   &&
        lhs.content_generation == rhs.content_generation;
}

inline bool operator!=(
    terminal_selection_line_lease_t lhs,
    terminal_selection_line_lease_t rhs)
{
    return !(lhs == rhs);
}

struct Terminal_selection_result
{
    Terminal_selection_result_code code = Terminal_selection_result_code::OK;
    QString                        text;
};

struct Terminal_osc52_write_request
{
    std::uint64_t  request_id       = 0U;
    QString        target_selection = QStringLiteral("clipboard");
    QByteArray     decoded_payload;
    std::size_t    raw_payload_size = 0U;
    QString        source_sequence;
};

struct Terminal_osc52_write_response
{
    std::uint64_t  request_id       = 0U;
    Terminal_clipboard_response_decision decision =
        Terminal_clipboard_response_decision::DENY;
};

class Selection_contract_controller
{
public:
    bool                            has_selection()            const { return has_copyable_payload();     }
    bool                            has_copyable_payload()     const
    {
        return m_has_selection && m_selected_text.has_value();
    }
    bool                            has_internal_selection()   const { return m_has_selection;            }
    bool                            has_cached_selected_text() const { return m_selected_text.has_value(); }
    const Terminal_selection_range& range()                    const { return m_range;                    }
    Terminal_selection_internal_state internal_state()          const { return m_internal_state;           }
    Terminal_selection_anchor_domain anchor_domain()            const { return m_anchor_domain;            }
    std::uint64_t durable_payload_identity()                    const { return m_durable_payload_identity; }
    std::uint64_t provisional_payload_identity()                const { return m_provisional_payload_identity; }
    const std::optional<terminal_selection_visual_lease_t>& visual_lease() const
    {
        return m_visual_lease;
    }

    void begin(terminal_grid_position_t anchor);
    void extend(terminal_grid_position_t extent);
    void clear();
    void set_range(Terminal_selection_range range);
    void set_range(Terminal_selection_range range, QString selected_text);
    void set_range(
        Terminal_selection_range             range,
        QString                              selected_text,
        terminal_selection_visual_lease_t    visual_lease);
    void hide_visual_attachment();
    void detach_visual_attachment();
    void update_visual_lease_source(
        terminal_selection_content_basis_t source_content_basis,
        std::uint64_t                      grid_reflow_basis,
        std::uint64_t                      row_origin_generation,
        terminal_grid_size_t               grid_size,
        const Terminal_viewport_state&     viewport_mapping);
    bool apply_backing_event(terminal_selection_backing_event_t event);

    Terminal_selection_result selected_text() const;
    Terminal_selection_result selected_text(std::span<const QString> logical_rows) const;

private:
    std::uint64_t next_payload_identity();
    void clear_payload_identity();
    void clear_visual_lease();
    void record_visual_lease(terminal_selection_visual_lease_t visual_lease);

    Terminal_selection_internal_state m_internal_state = Terminal_selection_internal_state::NONE;
    Terminal_selection_anchor_domain
                               m_anchor_domain = Terminal_selection_anchor_domain::NONE;
    bool                       m_has_selection = false;
    Terminal_selection_range   m_range;
    std::optional<QString>     m_selected_text;
    std::optional<terminal_selection_visual_lease_t>
                               m_visual_lease;
    std::uint64_t              m_next_payload_identity        = 1U;
    std::uint64_t              m_durable_payload_identity     = 0U;
    std::uint64_t              m_provisional_payload_identity = 0U;
};

}
