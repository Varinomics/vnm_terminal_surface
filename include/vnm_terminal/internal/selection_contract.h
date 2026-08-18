#pragma once

#include "vnm_terminal/internal/metrics_contract.h"
#include "vnm_terminal/internal/viewport_contract.h"
#include <QString>
#include <QByteArray>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <utility>
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

enum class Terminal_selection_internal_state
{
    NONE,
    DRAG_ARMED,
    DRAG_PREVIEW,
    ATTACHED,
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

enum class Terminal_history_resolution_status
{
    OK,
    INVALID_HANDLE,
    STALE_EPOCH,
    STALE_BYTE_SEQUENCE,
    STALE_ROW_SEQUENCE,
    RECORD_SIZE_MISMATCH,
    CONTENT_GENERATION_MISMATCH,
};

constexpr std::uint64_t k_terminal_history_retained_identity_epoch = 1U;
constexpr std::uint32_t k_terminal_history_retained_identity_record_bytes = 0U;

struct terminal_history_handle_t
{
    std::uint64_t              epoch              = 0U;
    std::uint64_t              byte_sequence      = 0U;
    std::uint64_t              row_sequence       = 0U;
    std::uint32_t              record_bytes       = 0U;
    std::uint64_t              content_generation = 0U;

    friend bool operator==(
        const terminal_history_handle_t&,
        const terminal_history_handle_t&) = default;
};

inline terminal_history_handle_t terminal_history_handle_from_retained_identity(
    std::uint64_t retained_line_id,
    std::uint64_t content_generation)
{
    if (retained_line_id == 0U) {
        return {};
    }

    return {
        k_terminal_history_retained_identity_epoch,
        retained_line_id,
        retained_line_id,
        k_terminal_history_retained_identity_record_bytes,
        content_generation,
    };
}

inline bool terminal_history_handle_has_identity(terminal_history_handle_t handle)
{
    return handle.epoch != 0U && handle.row_sequence != 0U;
}

struct terminal_grid_position_t
{
    int                        row    = 0;
    int                        column = 0;

    friend bool operator==(
        const terminal_grid_position_t&,
        const terminal_grid_position_t&) = default;
};

struct Terminal_selection_range
{
    terminal_grid_position_t   start;
    terminal_grid_position_t   end;
    Terminal_selection_mode    mode   = Terminal_selection_mode::NORMAL;

    friend bool operator==(
        const Terminal_selection_range&,
        const Terminal_selection_range&) = default;
};

struct terminal_selection_content_basis_t
{
    std::uint64_t              content_generation     = 0U;
    std::uint64_t              grid_reflow_generation = 0U;

    friend bool operator==(
        const terminal_selection_content_basis_t&,
        const terminal_selection_content_basis_t&) = default;
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
    terminal_history_handle_t          history_handle;

    friend bool operator==(
        const terminal_selection_line_lease_t&,
        const terminal_selection_line_lease_t&) = default;
};

// The rows a selection holds, shared rather than copied. A lease is copied on
// every content-changed publication, on every synchronized hold and rebase, and
// on every drag step, and it carries one record per selected row - so at a
// large selection those copies moved megabytes to change a handful of scalar
// fields. The rows themselves never change once a lease exists: translation
// builds a new list. Making that explicit lets every copy share one immutable
// block and reduces to a reference-count increment.
//
// Deliberately read-only. A caller that needs different rows builds a vector
// and assigns it, which is what keeps sharing safe without copy-on-write.
class Terminal_selection_line_lease_list
{
public:
    using Storage = std::vector<terminal_selection_line_lease_t>;

    Terminal_selection_line_lease_list()
    :
        m_rows(shared_empty_rows())
    {
    }

    Terminal_selection_line_lease_list(Storage rows)
    :
        m_rows(rows.empty()
            ? shared_empty_rows()
            : std::make_shared<const Storage>(std::move(rows)))
    {
    }

    Terminal_selection_line_lease_list(
        std::initializer_list<terminal_selection_line_lease_t> rows)
    :
        Terminal_selection_line_lease_list(Storage(rows))
    {
    }

    Terminal_selection_line_lease_list(
        const Terminal_selection_line_lease_list&) = default;
    Terminal_selection_line_lease_list& operator=(
        const Terminal_selection_line_lease_list&) = default;

    // A moved-from list reads as an empty one, which is what a moved-from
    // std::vector did here before. Leaving the block pointer null instead would
    // turn every accessor into a null dereference exactly where the member used
    // to be safe to query - and this is the change that introduces whole-lease
    // moves in the first place.
    Terminal_selection_line_lease_list(
        Terminal_selection_line_lease_list&& other) noexcept
    :
        m_rows(std::exchange(other.m_rows, shared_empty_rows()))
    {
    }

    Terminal_selection_line_lease_list& operator=(
        Terminal_selection_line_lease_list&& other) noexcept
    {
        m_rows = std::exchange(other.m_rows, shared_empty_rows());
        return *this;
    }

    bool        empty() const noexcept { return m_rows->empty(); }
    std::size_t size()  const noexcept { return m_rows->size();  }

    const terminal_selection_line_lease_t& operator[](std::size_t index) const
    {
        return (*m_rows)[index];
    }
    const terminal_selection_line_lease_t& front() const { return m_rows->front(); }
    const terminal_selection_line_lease_t* data()  const noexcept { return m_rows->data();  }
    const terminal_selection_line_lease_t* begin() const noexcept { return m_rows->data(); }
    const terminal_selection_line_lease_t* end()   const noexcept
    {
        return m_rows->data() + m_rows->size();
    }

    std::span<const terminal_selection_line_lease_t> rows() const noexcept
    {
        return *m_rows;
    }

    // Compares rows, not blocks. Two leases built separately from the same rows
    // are the same selection, and callers - tests especially - depend on that.
    // Sharing only makes the common case cheap enough to skip the walk.
    friend bool operator==(
        const Terminal_selection_line_lease_list& left,
        const Terminal_selection_line_lease_list& right)
    {
        return left.m_rows == right.m_rows || *left.m_rows == *right.m_rows;
    }
    friend bool operator==(
        const Terminal_selection_line_lease_list& left,
        const Storage&                            right)
    {
        return *left.m_rows == right;
    }

private:
    static const std::shared_ptr<const Storage>& shared_empty_rows()
    {
        static const std::shared_ptr<const Storage> empty =
            std::make_shared<const Storage>();
        return empty;
    }

    std::shared_ptr<const Storage> m_rows;
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
    Terminal_selection_line_lease_list selected_lines;
};

inline terminal_selection_line_lease_t terminal_selection_line_lease_from_retained_identity(
    int           row_offset,
    std::uint64_t retained_line_id,
    std::uint64_t content_generation)
{
    return {
        row_offset,
        terminal_history_handle_from_retained_identity(retained_line_id, content_generation),
    };
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
    void detach_visual_attachment();
    void install_translated_attachment(
        Terminal_selection_range          range,
        terminal_selection_visual_lease_t visual_lease);
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
