#pragma once

#include "vnm_terminal/internal/ime_contract.h"
#include "vnm_terminal/internal/metrics_contract.h"
#include "vnm_terminal/internal/selection_contract.h"
#include "vnm_terminal/internal/terminal_render_cell_text.h"
#include "vnm_terminal/internal/terminal_style.h"
#include "vnm_terminal/internal/viewport_contract.h"
#include <QByteArray>
#include <QChar>
#include <QString>
#include <QStringView>
#include <QtGlobal>
#include <algorithm>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>
#include <cstddef>
#include <cstdint>

namespace vnm_terminal::internal {

enum class Terminal_cursor_shape
{
    BLOCK,
    BAR,
    UNDERLINE,
};

enum class Terminal_mouse_tracking_mode
{
    NONE,
    BUTTON,
    DRAG,
    ANY,
};

struct Terminal_mode_state
{
    bool                         application_cursor_keys = false;
    bool                         reverse_video           = false;
    bool                         origin_mode             = false;
    bool                         autowrap                = true;
    bool                         cursor_visible          = true;
    Terminal_mouse_tracking_mode mouse_tracking          = Terminal_mouse_tracking_mode::NONE;
    bool                         focus_reporting         = false;
    bool                         sgr_mouse_encoding      = false;
    bool                         alternate_scroll        = false;
    bool                         bracketed_paste         = false;
    bool                         synchronized_output     = false;
};

enum class Terminal_render_snapshot_status
{
    OK,
    INVALID_GRID_SIZE,
    INVALID_CELL_POSITION,
    INVALID_CELL_WIDTH,
    INVALID_CELL_TEXT_CATEGORY,
    INVALID_CELL_OVERLAP,
    INVALID_WIDE_CELL_CONTINUATION,
    INVALID_WIDE_CELL_STYLE,
    INVALID_STYLE_ID,
    INVALID_CURSOR_POSITION,
    INVALID_VIEWPORT,
    INVALID_DIRTY_ROW_RANGE,
    INVALID_SELECTION_SPAN,
    INVALID_LINE_PROVENANCE,
    INVALID_HYPERLINK_METADATA,
    INVALID_SNAPSHOT_BASIS_PURPOSE,
    INVALID_CELL_ORDER,
};

enum class Terminal_render_snapshot_basis
{
    LIVE_CONTENT,
    PUBLIC_PROJECTION,
};

enum class Terminal_render_snapshot_purpose
{
    CONTENT,
    SELECTION_DERIVED,
    GEOMETRY_DERIVED,
    SCROLL,
};

enum class Terminal_render_snapshot_materialization_reason
{
    GEOMETRY_DERIVED_SNAPSHOT,
    ROW_VIEW_PARITY_TEST,
};

enum class Terminal_retained_line_provenance_source
{
    TERMINAL_STORAGE,
    RECOVERED_PRIMARY_REPAINT,
};

inline bool retained_line_provenance_source_is_valid(
    Terminal_retained_line_provenance_source source)
{
    switch (source) {
        case Terminal_retained_line_provenance_source::TERMINAL_STORAGE:
        case Terminal_retained_line_provenance_source::RECOVERED_PRIMARY_REPAINT:
            return true;
    }

    return false;
}

inline QString retained_line_provenance_source_name(
    Terminal_retained_line_provenance_source source)
{
    switch (source) {
        case Terminal_retained_line_provenance_source::TERMINAL_STORAGE:
            return QStringLiteral("terminal_storage");
        case Terminal_retained_line_provenance_source::RECOVERED_PRIMARY_REPAINT:
            return QStringLiteral("recovered_primary_repaint");
    }

    return QStringLiteral("unknown");
}

inline bool render_snapshot_materialization_reason_is_valid(
    Terminal_render_snapshot_materialization_reason reason)
{
    switch (reason) {
        case Terminal_render_snapshot_materialization_reason::GEOMETRY_DERIVED_SNAPSHOT:
        case Terminal_render_snapshot_materialization_reason::ROW_VIEW_PARITY_TEST:
            return true;
    }

    return false;
}

enum class Terminal_synchronized_output_scroll_policy
{
    DEFER_UNTIL_CONTENT_PUBLICATION,
    IMMEDIATE_PUBLIC_PROJECTION,
};

enum class Terminal_synchronized_output_policy_change_event
{
    NONE,
    CHANGED_MID_HOLD,
};

enum class Terminal_public_scroll_diagnostic_reason
{
    NONE,
    SYNCHRONIZED_OUTPUT_SCROLL_POLICY_CHANGED_MID_HOLD,
    PUBLIC_PROJECTION_GEOMETRY_INVALIDATED,
    PUBLIC_PROJECTION_MEMORY_PRESSURE_INVALIDATED,
    PUBLIC_PROJECTION_INVALIDATED_DEFERRED_INTENT,
    PUBLIC_PROJECTION_SCROLL_PUBLICATION_FAILED,
    DETACHED_ANCHOR_CONTENT_GENERATION_CHANGED,
    DETACHED_ANCHOR_GEOMETRY_CHANGED,
    DETACHED_ANCHOR_NOT_RETAINED,
    SELECTION_PUBLIC_PROJECTION_UNSUPPORTED,
    SCREEN_BUFFER_EPOCH_CHANGED,
    BUFFER_TRANSITION_RELEASED,
};

enum class Terminal_release_reconciliation_result
{
    NONE,
    STICKY_TAIL,
    EXACT_ANCHOR,
    RETAINED_ID_BEST_EFFORT,
    NEAREST_SUCCESSOR,
    NEAREST_PREDECESSOR,
    OLDEST_AVAILABLE_LIVE,
    DEFERRED_OFFSET,
    INCOMPATIBLE_BUFFER,
};

enum class Terminal_hidden_row_eligibility
{
    NOT_EVALUATED,
    ELIGIBLE,
    INELIGIBLE,
};

enum class Terminal_hidden_row_clamp_reason
{
    NONE,
    PUBLIC_VIEWPORT_BOUNDARY,
    LIVE_VIEWPORT_BOUNDARY,
};

enum class Terminal_public_projection_disable_reason
{
    NONE,
    GEOMETRY_INVALIDATED,
    MEMORY_PRESSURE,
    PROJECTION_INVALIDATED,
    UNSUPPORTED_BUFFER,
};

inline QString render_snapshot_basis_name(Terminal_render_snapshot_basis basis)
{
    switch (basis) {
        case Terminal_render_snapshot_basis::LIVE_CONTENT:
            return QStringLiteral("LIVE_CONTENT");
        case Terminal_render_snapshot_basis::PUBLIC_PROJECTION:
            return QStringLiteral("PUBLIC_PROJECTION");
    }

    return QStringLiteral("UNKNOWN");
}

inline QString render_snapshot_purpose_name(Terminal_render_snapshot_purpose purpose)
{
    switch (purpose) {
        case Terminal_render_snapshot_purpose::CONTENT:
            return QStringLiteral("CONTENT");
        case Terminal_render_snapshot_purpose::SELECTION_DERIVED:
            return QStringLiteral("SELECTION_DERIVED");
        case Terminal_render_snapshot_purpose::GEOMETRY_DERIVED:
            return QStringLiteral("GEOMETRY_DERIVED");
        case Terminal_render_snapshot_purpose::SCROLL:
            return QStringLiteral("SCROLL");
    }

    return QStringLiteral("UNKNOWN");
}

inline QString synchronized_output_scroll_policy_name(
    Terminal_synchronized_output_scroll_policy policy)
{
    switch (policy) {
        case Terminal_synchronized_output_scroll_policy::DEFER_UNTIL_CONTENT_PUBLICATION:
            return QStringLiteral("DEFER_UNTIL_CONTENT_PUBLICATION");
        case Terminal_synchronized_output_scroll_policy::IMMEDIATE_PUBLIC_PROJECTION:
            return QStringLiteral("IMMEDIATE_PUBLIC_PROJECTION");
    }

    return QStringLiteral("UNKNOWN");
}

inline QString synchronized_output_policy_change_event_name(
    Terminal_synchronized_output_policy_change_event event)
{
    switch (event) {
        case Terminal_synchronized_output_policy_change_event::NONE:
            return QStringLiteral("none");
        case Terminal_synchronized_output_policy_change_event::CHANGED_MID_HOLD:
            return QStringLiteral("changed_mid_hold");
    }

    return QStringLiteral("unknown");
}

inline QString public_scroll_diagnostic_reason_name(
    Terminal_public_scroll_diagnostic_reason reason)
{
    switch (reason) {
        case Terminal_public_scroll_diagnostic_reason::NONE:
            return QStringLiteral("none");
        case Terminal_public_scroll_diagnostic_reason::
            SYNCHRONIZED_OUTPUT_SCROLL_POLICY_CHANGED_MID_HOLD:
            return QStringLiteral("synchronized_output_scroll_policy_changed_mid_hold");
        case Terminal_public_scroll_diagnostic_reason::PUBLIC_PROJECTION_GEOMETRY_INVALIDATED:
            return QStringLiteral("public_projection_geometry_invalidated");
        case Terminal_public_scroll_diagnostic_reason::
            PUBLIC_PROJECTION_MEMORY_PRESSURE_INVALIDATED:
            return QStringLiteral("public_projection_memory_pressure_invalidated");
        case Terminal_public_scroll_diagnostic_reason::
            PUBLIC_PROJECTION_INVALIDATED_DEFERRED_INTENT:
            return QStringLiteral("public_projection_invalidated_deferred_intent");
        case Terminal_public_scroll_diagnostic_reason::
            PUBLIC_PROJECTION_SCROLL_PUBLICATION_FAILED:
            return QStringLiteral("public_projection_scroll_publication_failed");
        case Terminal_public_scroll_diagnostic_reason::
            DETACHED_ANCHOR_CONTENT_GENERATION_CHANGED:
            return QStringLiteral("detached_anchor_content_generation_changed");
        case Terminal_public_scroll_diagnostic_reason::DETACHED_ANCHOR_GEOMETRY_CHANGED:
            return QStringLiteral("detached_anchor_geometry_changed");
        case Terminal_public_scroll_diagnostic_reason::DETACHED_ANCHOR_NOT_RETAINED:
            return QStringLiteral("detached_anchor_not_retained");
        case Terminal_public_scroll_diagnostic_reason::SELECTION_PUBLIC_PROJECTION_UNSUPPORTED:
            return QStringLiteral("selection_public_projection_unsupported");
        case Terminal_public_scroll_diagnostic_reason::SCREEN_BUFFER_EPOCH_CHANGED:
            return QStringLiteral("screen_buffer_epoch_changed");
        case Terminal_public_scroll_diagnostic_reason::BUFFER_TRANSITION_RELEASED:
            return QStringLiteral("buffer_transition_released");
    }

    return QStringLiteral("unknown");
}

inline QString release_reconciliation_result_name(
    Terminal_release_reconciliation_result result)
{
    switch (result) {
        case Terminal_release_reconciliation_result::NONE:
            return QStringLiteral("none");
        case Terminal_release_reconciliation_result::STICKY_TAIL:
            return QStringLiteral("sticky_tail");
        case Terminal_release_reconciliation_result::EXACT_ANCHOR:
            return QStringLiteral("exact_anchor");
        case Terminal_release_reconciliation_result::RETAINED_ID_BEST_EFFORT:
            return QStringLiteral("retained_id_best_effort");
        case Terminal_release_reconciliation_result::NEAREST_SUCCESSOR:
            return QStringLiteral("nearest_successor");
        case Terminal_release_reconciliation_result::NEAREST_PREDECESSOR:
            return QStringLiteral("nearest_predecessor");
        case Terminal_release_reconciliation_result::OLDEST_AVAILABLE_LIVE:
            return QStringLiteral("oldest_available_live");
        case Terminal_release_reconciliation_result::DEFERRED_OFFSET:
            return QStringLiteral("deferred_offset");
        case Terminal_release_reconciliation_result::INCOMPATIBLE_BUFFER:
            return QStringLiteral("incompatible_buffer");
    }

    return QStringLiteral("unknown");
}

inline QString hidden_row_eligibility_name(Terminal_hidden_row_eligibility eligibility)
{
    switch (eligibility) {
        case Terminal_hidden_row_eligibility::NOT_EVALUATED:
            return QStringLiteral("not_evaluated");
        case Terminal_hidden_row_eligibility::ELIGIBLE:
            return QStringLiteral("eligible");
        case Terminal_hidden_row_eligibility::INELIGIBLE:
            return QStringLiteral("ineligible");
    }

    return QStringLiteral("unknown");
}

inline QString hidden_row_clamp_reason_name(Terminal_hidden_row_clamp_reason reason)
{
    switch (reason) {
        case Terminal_hidden_row_clamp_reason::NONE:
            return QStringLiteral("none");
        case Terminal_hidden_row_clamp_reason::PUBLIC_VIEWPORT_BOUNDARY:
            return QStringLiteral("public_viewport_boundary");
        case Terminal_hidden_row_clamp_reason::LIVE_VIEWPORT_BOUNDARY:
            return QStringLiteral("live_viewport_boundary");
    }

    return QStringLiteral("unknown");
}

inline QString public_projection_disable_reason_name(
    Terminal_public_projection_disable_reason reason)
{
    switch (reason) {
        case Terminal_public_projection_disable_reason::NONE:
            return QStringLiteral("none");
        case Terminal_public_projection_disable_reason::GEOMETRY_INVALIDATED:
            return QStringLiteral("geometry_invalidated");
        case Terminal_public_projection_disable_reason::MEMORY_PRESSURE:
            return QStringLiteral("memory_pressure");
        case Terminal_public_projection_disable_reason::PROJECTION_INVALIDATED:
            return QStringLiteral("projection_invalidated");
        case Terminal_public_projection_disable_reason::UNSUPPORTED_BUFFER:
            return QStringLiteral("unsupported_buffer");
    }

    return QStringLiteral("unknown");
}

struct Terminal_render_cell
{
    terminal_grid_position_t   position;
    Terminal_render_cell_text  text;
    std::uint64_t              hyperlink_id      = 0U;
    int                        display_width     = 1;
    bool                       wide_continuation = false;
    Terminal_style_id          style_id          = k_default_terminal_style_id;
    Terminal_render_cell_text_category
                               text_category     = Terminal_render_cell_text_category::UNKNOWN;
};

static_assert(sizeof(Terminal_render_cell) <= 48U);

struct Terminal_render_cursor
{
    terminal_grid_position_t   position;
    Terminal_cursor_shape      shape         = Terminal_cursor_shape::BLOCK;
    bool                       visible       = true;
    bool                       blink_enabled = true;
};

struct Terminal_render_dirty_row_range
{
    int                        first_row = 0;
    int                        row_count = 0;
};

struct Terminal_render_selection_span
{
    Terminal_selection_range   source_range;
    int                        row          = 0;
    int                        first_column = 0;
    int                        column_count = 0;
};

struct Terminal_render_line_provenance
{
    std::int64_t               logical_row        = 0;
    std::uint64_t              retained_line_id   = 0U;
    std::uint64_t              content_generation = 0U;
    // Wall-clock time of the last content change (ms since epoch); zero means
    // the line was never written. Not part of row identity: the stamp only
    // moves when content_generation advances, so equality stays decided by the
    // generation and the field rides along for GUI-thread timestamp lookups.
    qint64                     content_stamp_ms   = 0;
    Terminal_retained_line_provenance_source
                               source             =
                                   Terminal_retained_line_provenance_source::TERMINAL_STORAGE;
};

struct Terminal_render_selection_request
{
    Terminal_selection_range                         range;
    std::vector<terminal_selection_line_lease_t>     expected_lines;
};

inline bool operator==(
    const Terminal_render_line_provenance& left,
    const Terminal_render_line_provenance& right)
{
    return
        left.logical_row        == right.logical_row        &&
        left.retained_line_id   == right.retained_line_id   &&
        left.content_generation == right.content_generation &&
        left.source             == right.source;
}

struct Terminal_render_hyperlink_metadata
{
    std::uint64_t              hyperlink_id                 = 0U;
    QByteArray                 identity_key;
    QByteArray                 uri;
};

struct Terminal_render_metadata
{
    std::uint64_t              sequence                         = 0U;
    std::uint64_t              publication_generation           = 0U;
    std::uint64_t              processed_backend_callback_epoch = 0U;
    std::uint64_t              satisfied_input_freshness_token  = 0U;
    std::uint64_t              row_origin_generation            = 0U;
    bool                       backend_geometry_in_sync         = true;
    bool                       visual_bell_active               = false;
    bool                       mouse_reporting_mode_changed     = false;
};

struct Terminal_public_scroll_diagnostics
{
    Terminal_synchronized_output_scroll_policy effective_policy =
        Terminal_synchronized_output_scroll_policy::DEFER_UNTIL_CONTENT_PUBLICATION;
    Terminal_synchronized_output_policy_change_event policy_change_event =
        Terminal_synchronized_output_policy_change_event::NONE;
    Terminal_public_scroll_diagnostic_reason diagnostic_reason =
        Terminal_public_scroll_diagnostic_reason::NONE;
    std::uint64_t public_projection_generation = 0U;
    Terminal_viewport_state public_viewport_before;
    Terminal_viewport_state public_viewport_after;
    Terminal_viewport_state live_viewport_before_on_release;
    Terminal_viewport_state live_viewport_after_on_release;
    bool visible_scroll_applied = false;
    bool live_content_publication_blocked = false;
    Terminal_release_reconciliation_result release_reconciliation_result =
        Terminal_release_reconciliation_result::NONE;
    Terminal_hidden_row_eligibility hidden_row_eligibility =
        Terminal_hidden_row_eligibility::NOT_EVALUATED;
    Terminal_hidden_row_clamp_reason hidden_row_clamp_reason =
        Terminal_hidden_row_clamp_reason::NONE;
    Terminal_public_projection_disable_reason public_projection_disable_reason =
        Terminal_public_projection_disable_reason::NONE;
};

struct Terminal_render_snapshot_request
{
    std::uint64_t                         sequence                         = 0U;
    std::uint64_t                         publication_generation           = 0U;
    std::uint64_t                         processed_backend_callback_epoch = 0U;
    std::uint64_t                         satisfied_input_freshness_token  = 0U;
    std::uint64_t                         row_origin_generation            = 0U;
    Terminal_render_snapshot_basis        basis = Terminal_render_snapshot_basis::LIVE_CONTENT;
    Terminal_render_snapshot_purpose      purpose = Terminal_render_snapshot_purpose::CONTENT;
    Terminal_public_scroll_diagnostics    public_scroll_diagnostics;
    Terminal_viewport_state               viewport;
    std::vector<int>                      dirty_rows;
    bool                                  viewport_changed = false;
    Terminal_cursor_shape                 cursor_shape = Terminal_cursor_shape::BLOCK;
    bool                                  cursor_blink_enabled = true;
    Ime_preedit_state                     ime_preedit;
    std::vector<Terminal_render_selection_request> selections;
    bool                                  backend_geometry_in_sync = true;
    bool                                  visual_bell_active = false;
    bool                                  mouse_reporting_mode_changed = false;
};

struct Terminal_render_snapshot_row_payload_namespace
{
    std::uint64_t              style_table_generation         = 0U;
    std::uint64_t              hyperlink_namespace_generation = 0U;
};

inline bool operator==(
    const Terminal_render_snapshot_row_payload_namespace& left,
    const Terminal_render_snapshot_row_payload_namespace& right)
{
    return
        left.style_table_generation         == right.style_table_generation &&
        left.hyperlink_namespace_generation == right.hyperlink_namespace_generation;
}

struct Terminal_render_snapshot_row_payload_identity
{
    int                                           row = 0;
    Terminal_render_line_provenance               provenance;
    std::uint64_t                                 row_origin_generation = 0U;
    Terminal_render_snapshot_row_payload_namespace metadata_namespace;
};

struct Terminal_render_snapshot_immutable_row_payload
{
    Terminal_render_snapshot_row_payload_identity identity;
    std::vector<Terminal_render_cell>             cells;
};

inline std::size_t render_snapshot_row_payload_retained_bytes(
    const Terminal_render_snapshot_immutable_row_payload& payload)
{
    std::size_t bytes = payload.cells.capacity() * sizeof(Terminal_render_cell);
    for (const Terminal_render_cell& cell : payload.cells) {
        const QString* fallback = cell.text.fallback_qstring_or_null();
        if (fallback != nullptr) {
            bytes += sizeof(QString);
            bytes += static_cast<std::size_t>(fallback->capacity()) * sizeof(QChar);
        }
    }
    return bytes;
}

inline std::size_t render_snapshot_row_payload_owner_retained_bytes(
    const std::vector<Terminal_render_snapshot_immutable_row_payload>& payloads)
{
    std::size_t bytes =
        sizeof(Terminal_render_snapshot_immutable_row_payload) * payloads.capacity();
    for (const Terminal_render_snapshot_immutable_row_payload& payload : payloads) {
        bytes += render_snapshot_row_payload_retained_bytes(payload);
    }
    return bytes;
}

class Terminal_render_snapshot_row_payload_owner
{
public:
    Terminal_render_snapshot_row_payload_owner(
        std::uint64_t                                           retained_generation,
        std::vector<Terminal_render_snapshot_immutable_row_payload> payloads)
    :
        m_retained_generation(retained_generation),
        m_retained_bytes(render_snapshot_row_payload_owner_retained_bytes(payloads)),
        m_payloads(std::move(payloads))
    {}

    std::uint64_t retained_generation() const { return m_retained_generation; }
    std::size_t retained_bytes() const { return m_retained_bytes; }
    std::size_t payload_count() const { return m_payloads.size(); }

    const Terminal_render_snapshot_immutable_row_payload& payload_at(
        std::size_t index) const
    {
        Q_ASSERT(index < m_payloads.size());
        return m_payloads[index];
    }

private:
    std::uint64_t                                           m_retained_generation = 0U;
    std::size_t                                             m_retained_bytes      = 0U;
    std::vector<Terminal_render_snapshot_immutable_row_payload> m_payloads;
};

struct Terminal_render_snapshot_row_payload_ref
{
    std::shared_ptr<const Terminal_render_snapshot_row_payload_owner> owner;
    std::size_t                                                       payload_index = 0U;

    const Terminal_render_snapshot_immutable_row_payload* payload_or_null() const
    {
        if (owner == nullptr || payload_index >= owner->payload_count()) {
            return nullptr;
        }

        return &owner->payload_at(payload_index);
    }
};

struct Terminal_render_snapshot_style_id_remap
{
    Terminal_style_id source_id = k_default_terminal_style_id;
    Terminal_style_id target_id = k_default_terminal_style_id;
};

struct Terminal_render_snapshot_hyperlink_id_remap
{
    std::uint64_t source_id = 0U;
    std::uint64_t target_id = 0U;
};

struct Terminal_render_snapshot;

struct Terminal_render_snapshot_lazy_row_payload
{
    Terminal_render_snapshot_row_payload_ref        source;
    std::shared_ptr<const Terminal_render_snapshot> source_snapshot;
    int                                             source_snapshot_row = -1;
    Terminal_render_snapshot_row_payload_namespace  source_snapshot_metadata_namespace;
    Terminal_render_snapshot_row_payload_namespace  receiving_namespace;
    std::shared_ptr<const std::vector<Terminal_render_cell>>
                                                       cells_in_receiving_namespace;
};

struct Terminal_render_snapshot_lazy_payloads
{
    Terminal_render_snapshot_row_payload_namespace       receiving_namespace;
    std::vector<Terminal_render_snapshot_lazy_row_payload> rows;
};

struct Terminal_render_snapshot_row_payload_retention_policy
{
    std::size_t retained_generation_limit = 2U;
    std::size_t retained_byte_limit       = 16U * 1024U * 1024U;
};

class Terminal_render_snapshot_row_payload_retention
{
public:
    explicit Terminal_render_snapshot_row_payload_retention(
        Terminal_render_snapshot_row_payload_retention_policy policy = {})
    :
        m_policy(policy)
    {}

    void retain(std::shared_ptr<const Terminal_render_snapshot_row_payload_owner> owner)
    {
        Q_ASSERT(owner != nullptr);

        const std::uint64_t generation = owner->retained_generation();
        m_owners.erase(
            std::remove_if(
                m_owners.begin(),
                m_owners.end(),
                [generation](
                    const std::shared_ptr<const Terminal_render_snapshot_row_payload_owner>&
                        retained) {
                    return retained->retained_generation() == generation;
                }),
            m_owners.end());
        m_owners.push_back(std::move(owner));
        std::sort(
            m_owners.begin(),
            m_owners.end(),
            [](
                const std::shared_ptr<const Terminal_render_snapshot_row_payload_owner>& left,
                const std::shared_ptr<const Terminal_render_snapshot_row_payload_owner>& right) {
                return left->retained_generation() < right->retained_generation();
            });
        evict_to_policy();
    }

    std::size_t owner_count() const { return m_owners.size(); }

    std::size_t retained_bytes() const
    {
        std::size_t bytes = 0U;
        for (const std::shared_ptr<const Terminal_render_snapshot_row_payload_owner>& owner :
            m_owners)
        {
            bytes += owner->retained_bytes();
        }
        return bytes;
    }

    bool contains_generation(std::uint64_t generation) const
    {
        return std::any_of(
            m_owners.begin(),
            m_owners.end(),
            [generation](
                const std::shared_ptr<const Terminal_render_snapshot_row_payload_owner>& owner) {
                return owner->retained_generation() == generation;
            });
    }

private:
    void evict_to_policy()
    {
        while (m_owners.size() > m_policy.retained_generation_limit) {
            m_owners.erase(m_owners.begin());
        }

        while (!m_owners.empty() && retained_bytes() > m_policy.retained_byte_limit) {
            m_owners.erase(m_owners.begin());
        }
    }

    Terminal_render_snapshot_row_payload_retention_policy
        m_policy;
    std::vector<std::shared_ptr<const Terminal_render_snapshot_row_payload_owner>>
        m_owners;
};

struct Terminal_render_snapshot
{
    Terminal_render_snapshot_basis                     basis = Terminal_render_snapshot_basis::LIVE_CONTENT;
    Terminal_render_snapshot_purpose                   purpose = Terminal_render_snapshot_purpose::CONTENT;
    Terminal_public_scroll_diagnostics                 public_scroll_diagnostics;
    terminal_grid_size_t                               grid_size;
    Terminal_viewport_state                            viewport;
    Terminal_color_state                               color_state;
    std::vector<Terminal_text_style>                   styles;
    std::vector<Terminal_render_cell>                  cells;
    std::shared_ptr<const Terminal_render_snapshot_lazy_payloads>
                                                          lazy_row_payloads;
    std::vector<Terminal_render_line_provenance>       visible_line_provenance;
    std::vector<Terminal_render_dirty_row_range>       dirty_row_ranges;
    std::vector<Terminal_render_hyperlink_metadata>    hyperlinks;
    Terminal_render_cursor                             cursor;
    Ime_preedit_state                                  ime_preedit;
    std::vector<Terminal_render_selection_span>        selection_spans;
    Terminal_render_metadata                           metadata;
    Terminal_mode_state                                modes;
};

struct Terminal_render_snapshot_validation
{
    Terminal_render_snapshot_status status = Terminal_render_snapshot_status::OK;
};

inline Terminal_render_snapshot make_empty_render_snapshot(
    terminal_grid_size_t       grid_size,
    Terminal_viewport_state    viewport,
    std::uint64_t              sequence,
    std::uint64_t              publication_generation = 0U)
{
    Terminal_render_snapshot snapshot;
    snapshot.grid_size                       = grid_size;
    snapshot.viewport                        = viewport;
    snapshot.viewport.visible_rows           = grid_size.rows > 0 ? grid_size.rows : viewport.visible_rows;
    snapshot.metadata.sequence               = sequence;
    snapshot.metadata.publication_generation = publication_generation;
    snapshot.styles.push_back(make_default_terminal_text_style());
    return snapshot;
}

inline std::vector<Terminal_render_dirty_row_range> compact_dirty_row_ranges(
    std::vector<int>           dirty_rows,
    int                        visible_rows,
    bool                       full_repaint)
{
    if (visible_rows <= 0) {
        return {};
    }

    if (full_repaint) {
        return {{0, visible_rows}};
    }

    std::sort(dirty_rows.begin(), dirty_rows.end());
    dirty_rows.erase(std::unique(dirty_rows.begin(), dirty_rows.end()), dirty_rows.end());

    std::vector<Terminal_render_dirty_row_range> ranges;
    for (int row : dirty_rows) {
        if (row < 0 || row >= visible_rows) {
            continue;
        }

        if (!ranges.empty() &&
            ranges.back().first_row + ranges.back().row_count == row)
        {
            ++ranges.back().row_count;
            continue;
        }

        ranges.push_back({row, 1});
    }

    return ranges;
}

// Two snapshots coalesce their dirty rows by physical row index only when they
// describe the same physical row-identity space: same grid, same viewport
// mapping, and the same row-origin generation. A scrollback-eviction row-origin
// change shifts which logical line each physical index names, so coalescing
// across it would carry a stale or missing row; the row-origin guard forces a
// full repaint instead.
inline bool snapshots_share_row_identity_space(
    const Terminal_render_snapshot&    left,
    const Terminal_render_snapshot&    right)
{
    return
        grid_sizes_match(left.grid_size, right.grid_size)                           &&
        left.metadata.row_origin_generation == right.metadata.row_origin_generation &&
        viewport_mappings_match(left.viewport, right.viewport);
}

inline void append_dirty_range_rows(
    std::vector<int>&                                   rows,
    const std::vector<Terminal_render_dirty_row_range>& ranges)
{
    for (const Terminal_render_dirty_row_range& range : ranges) {
        for (int row = range.first_row; row < range.first_row + range.row_count; ++row) {
            rows.push_back(row);
        }
    }
}

// Merge the dirty rows of a previously published snapshot into a newer one when
// both share a physical row-identity space, returning the newer snapshot with a
// coalesced dirty-row range set. Diverging grid, viewport, or row-origin yields
// a full-viewport repaint range.
inline Terminal_render_snapshot snapshot_with_coalesced_dirty_rows(
    const Terminal_render_snapshot&        previous_snapshot,
    Terminal_render_snapshot               snapshot)
{
    if (!snapshots_share_row_identity_space(previous_snapshot, snapshot)) {
        snapshot.dirty_row_ranges =
            compact_dirty_row_ranges({}, snapshot.grid_size.rows, true);
        return snapshot;
    }

    std::vector<int> dirty_rows;
    append_dirty_range_rows(dirty_rows, previous_snapshot.dirty_row_ranges);
    append_dirty_range_rows(dirty_rows, snapshot.dirty_row_ranges);
    snapshot.dirty_row_ranges =
        compact_dirty_row_ranges(std::move(dirty_rows), snapshot.grid_size.rows, false);
    return snapshot;
}

// Shared-pointer wrapper over snapshot_with_coalesced_dirty_rows for callers
// that publish the coalesced snapshot through a shared_ptr<const> channel.
inline std::shared_ptr<const Terminal_render_snapshot> coalesced_dirty_row_snapshot_handle(
    const Terminal_render_snapshot&    previous,
    const Terminal_render_snapshot&    current)
{
    Terminal_render_snapshot coalesced = snapshot_with_coalesced_dirty_rows(previous, current);
    return std::make_shared<const Terminal_render_snapshot>(std::move(coalesced));
}

inline terminal_grid_position_t normalized_selection_start(
    const Terminal_selection_range& range)
{
    if (range.start.row < range.end.row) {
        return range.start;
    }

    if (range.start.row > range.end.row) {
        return range.end;
    }

    return range.start.column <= range.end.column ? range.start : range.end;
}

inline terminal_grid_position_t normalized_selection_end(
    const Terminal_selection_range& range)
{
    if (range.start.row < range.end.row) {
        return range.end;
    }

    if (range.start.row > range.end.row) {
        return range.start;
    }

    return range.start.column <= range.end.column ? range.end : range.start;
}

inline std::size_t normalized_selection_row_count(
    const Terminal_selection_range& range)
{
    const terminal_grid_position_t start = normalized_selection_start(range);
    const terminal_grid_position_t end   = normalized_selection_end(range);
    const std::int64_t row_count =
        static_cast<std::int64_t>(end.row) - static_cast<std::int64_t>(start.row) + 1;
    return row_count > 0 ? static_cast<std::size_t>(row_count) : 0U;
}

inline int render_snapshot_first_visible_logical_row(
    const Terminal_render_snapshot& snapshot)
{
    return snapshot.viewport.active_buffer == Terminal_buffer_id::ALTERNATE
        ? 0
        : snapshot.viewport.scrollback_rows - snapshot.viewport.offset_from_tail;
}

inline bool render_snapshot_visible_line_provenance_is_valid(
    const Terminal_render_snapshot& snapshot)
{
    if (snapshot.grid_size.rows <= 0) {
        return false;
    }

    if (snapshot.visible_line_provenance.size() !=
        static_cast<std::size_t>(snapshot.grid_size.rows))
    {
        return false;
    }

    const std::int64_t first_visible_logical_row =
        render_snapshot_first_visible_logical_row(snapshot);
    for (int row = 0; row < snapshot.grid_size.rows; ++row) {
        const Terminal_render_line_provenance& provenance =
            snapshot.visible_line_provenance[static_cast<std::size_t>(row)];
        const std::int64_t expected_logical_row =
            first_visible_logical_row + static_cast<std::int64_t>(row);
        if (provenance.logical_row != expected_logical_row ||
            provenance.retained_line_id == 0U              ||
            !retained_line_provenance_source_is_valid(provenance.source))
        {
            return false;
        }
    }

    return true;
}

inline void suppress_selection_spans_without_valid_line_provenance(
    Terminal_render_snapshot& snapshot)
{
    if (!snapshot.selection_spans.empty() &&
        !render_snapshot_visible_line_provenance_is_valid(snapshot))
    {
        snapshot.selection_spans.clear();
    }
}

inline bool render_snapshot_cells_equal(
    const Terminal_render_cell& left,
    const Terminal_render_cell& right)
{
    return
        left.position          == right.position          &&
        left.text              == right.text              &&
        left.hyperlink_id      == right.hyperlink_id      &&
        left.display_width     == right.display_width     &&
        left.wide_continuation == right.wide_continuation &&
        left.style_id          == right.style_id          &&
        left.text_category     == right.text_category;
}

inline bool render_snapshot_cells_equal_except_metadata_ids(
    const Terminal_render_cell& left,
    const Terminal_render_cell& right)
{
    return
        left.position          == right.position          &&
        left.text              == right.text              &&
        left.display_width     == right.display_width     &&
        left.wide_continuation == right.wide_continuation &&
        left.text_category     == right.text_category;
}

inline int render_snapshot_viewable_row_count(const Terminal_render_snapshot& snapshot)
{
    return std::max(snapshot.grid_size.rows, 0);
}

inline bool render_snapshot_row_is_viewable(
    const Terminal_render_snapshot& snapshot,
    int                             row)
{
    return row >= 0 && row < render_snapshot_viewable_row_count(snapshot);
}

inline bool render_cell_text_has_non_space(const Terminal_render_cell_text& text)
{
    const std::optional<ushort> single_code_unit = text.single_code_unit();
    if (single_code_unit.has_value()) {
        return *single_code_unit != 0x20U;
    }

    const QString* fallback = text.fallback_qstring_or_null();
    if (fallback == nullptr) {
        return false;
    }

    for (const QChar character : *fallback) {
        if (character != QLatin1Char(' ')) {
            return true;
        }
    }
    return false;
}

inline bool render_snapshot_row_is_dirty(
    const Terminal_render_snapshot& snapshot,
    int                             row)
{
    if (!render_snapshot_row_is_viewable(snapshot, row)) {
        return false;
    }

    const auto range = std::upper_bound(
        snapshot.dirty_row_ranges.begin(),
        snapshot.dirty_row_ranges.end(),
        row,
        [](int target_row, const Terminal_render_dirty_row_range& range) {
            return target_row < range.first_row;
        });
    if (range == snapshot.dirty_row_ranges.begin()) {
        return false;
    }

    const Terminal_render_dirty_row_range& candidate = *std::prev(range);
    const std::int64_t first_row = candidate.first_row;
    const std::int64_t end_row =
        first_row + static_cast<std::int64_t>(candidate.row_count);
    return
        static_cast<std::int64_t>(row) >= first_row &&
        static_cast<std::int64_t>(row) <  end_row;
}

inline bool collect_render_snapshot_hyperlink_ids(
    const Terminal_render_snapshot& snapshot,
    std::set<std::uint64_t>&        hyperlink_ids)
{
    hyperlink_ids.clear();
    for (const Terminal_render_hyperlink_metadata& hyperlink : snapshot.hyperlinks) {
        if (hyperlink.hyperlink_id                     == 0U ||
            hyperlink_ids.find(hyperlink.hyperlink_id) != hyperlink_ids.end())
        {
            return false;
        }
        hyperlink_ids.insert(hyperlink.hyperlink_id);
    }
    return true;
}

inline std::optional<Terminal_style_id> remapped_render_snapshot_style_id(
    const std::vector<Terminal_render_snapshot_style_id_remap>& remaps,
    Terminal_style_id                                           source_id)
{
    for (const Terminal_render_snapshot_style_id_remap& remap : remaps) {
        if (remap.source_id == source_id) {
            return remap.target_id;
        }
    }
    return std::nullopt;
}

inline std::optional<std::uint64_t> remapped_render_snapshot_hyperlink_id(
    const std::vector<Terminal_render_snapshot_hyperlink_id_remap>& remaps,
    std::uint64_t                                                   source_id)
{
    if (source_id == 0U) {
        return 0U;
    }

    for (const Terminal_render_snapshot_hyperlink_id_remap& remap : remaps) {
        if (remap.source_id == source_id) {
            if (remap.target_id == 0U) {
                return std::nullopt;
            }
            return remap.target_id;
        }
    }
    return std::nullopt;
}

inline std::shared_ptr<const std::vector<Terminal_render_cell>>
remapped_render_snapshot_row_cells(
    const Terminal_render_snapshot_immutable_row_payload&              payload,
    const std::vector<Terminal_render_snapshot_style_id_remap>&        style_id_remaps,
    const std::vector<Terminal_render_snapshot_hyperlink_id_remap>&    hyperlink_id_remaps)
{
    std::vector<Terminal_render_cell> cells = payload.cells;
    for (Terminal_render_cell& cell : cells) {
        const std::optional<Terminal_style_id> style_id =
            remapped_render_snapshot_style_id(style_id_remaps, cell.style_id);
        const std::optional<std::uint64_t> hyperlink_id =
            remapped_render_snapshot_hyperlink_id(hyperlink_id_remaps, cell.hyperlink_id);
        if (!style_id.has_value() || !hyperlink_id.has_value()) {
            return nullptr;
        }

        cell.style_id     = *style_id;
        cell.hyperlink_id = *hyperlink_id;
    }

    return std::make_shared<const std::vector<Terminal_render_cell>>(std::move(cells));
}

inline std::optional<Terminal_render_snapshot_lazy_row_payload>
borrowed_render_snapshot_lazy_row_payload(
    Terminal_render_snapshot_row_payload_ref                           source,
    Terminal_render_snapshot_row_payload_namespace                     receiving_namespace,
    const std::vector<Terminal_render_snapshot_style_id_remap>&        style_id_remaps,
    const std::vector<Terminal_render_snapshot_hyperlink_id_remap>&    hyperlink_id_remaps)
{
    const Terminal_render_snapshot_immutable_row_payload* payload =
        source.payload_or_null();
    if (payload == nullptr) {
        return std::nullopt;
    }

    Terminal_render_snapshot_lazy_row_payload row;
    row.source              = std::move(source);
    row.receiving_namespace = receiving_namespace;
    if (!(payload->identity.metadata_namespace == receiving_namespace)) {
        row.cells_in_receiving_namespace =
            remapped_render_snapshot_row_cells(
                *payload,
                style_id_remaps,
                hyperlink_id_remaps);
        if (row.cells_in_receiving_namespace == nullptr) {
            return std::nullopt;
        }
    }

    return row;
}

inline std::optional<Terminal_render_snapshot_lazy_row_payload>
borrowed_render_snapshot_lazy_row_payload_from_snapshot_row(
    std::shared_ptr<const Terminal_render_snapshot>             source_snapshot,
    int                                                         row_index,
    Terminal_render_snapshot_row_payload_namespace              source_namespace,
    Terminal_render_snapshot_row_payload_namespace              receiving_namespace)
{
    if (source_snapshot == nullptr || !(source_namespace == receiving_namespace)) {
        return std::nullopt;
    }

    Terminal_render_snapshot_lazy_row_payload row;
    row.source_snapshot                    = std::move(source_snapshot);
    row.source_snapshot_row                = row_index;
    row.source_snapshot_metadata_namespace = source_namespace;
    row.receiving_namespace                = receiving_namespace;
    return row;
}

class Terminal_render_snapshot_row_content_view;

class Terminal_render_snapshot_row_content
{
public:
    using const_iterator = std::vector<Terminal_render_cell>::const_iterator;

    int row() const { return m_row; }
    bool valid() const { return render_snapshot_row_is_viewable(*m_snapshot, m_row); }
    int column_count() const { return m_snapshot->grid_size.columns; }
    std::size_t cell_count() const
    {
        return static_cast<std::size_t>(std::distance(m_first, m_last));
    }

    const_iterator begin() const { return m_first; }
    const_iterator end() const { return m_last; }

    const Terminal_render_cell* cell_at(int column) const
    {
        if (column < 0 || column >= m_snapshot->grid_size.columns) {
            return nullptr;
        }

        const const_iterator found = std::lower_bound(
            m_first,
            m_last,
            column,
            [](const Terminal_render_cell& cell, int target_column) {
                return cell.position.column < target_column;
            });
        return found != m_last && found->position.column == column ? &*found : nullptr;
    }

    QString text(
        int     first_column,
        int     end_column,
        bool    trim_trailing_spaces) const
    {
        if (!valid()) {
            return {};
        }

        Q_ASSERT(first_column >= 0);
        Q_ASSERT(first_column <= end_column);
        Q_ASSERT(end_column <= m_snapshot->grid_size.columns);

        int effective_end_column = end_column;
        if (trim_trailing_spaces) {
            effective_end_column = first_column;
            const_iterator last_cell = m_last;
            while (last_cell != m_first) {
                --last_cell;
                if (last_cell->position.column < first_column) {
                    break;
                }

                if (last_cell->position.column >= end_column ||
                    last_cell->wide_continuation              ||
                    !render_cell_text_has_non_space(last_cell->text))
                {
                    continue;
                }

                effective_end_column = last_cell->position.column + 1;
                break;
            }

            if (effective_end_column == first_column) {
                return {};
            }
        }

        QString extracted;
        const_iterator cell = m_first;
        for (int column = first_column; column < effective_end_column; ++column) {
            while (cell != m_last && cell->position.column < column) {
                ++cell;
            }

            if (cell == m_last || cell->position.column > column) {
                extracted += QLatin1Char(' ');
                continue;
            }

            if (cell->wide_continuation) {
                ++cell;
                continue;
            }

            cell->text.append_to(extracted);
            ++cell;
        }

        if (trim_trailing_spaces) {
            while (extracted.endsWith(QLatin1Char(' '))) {
                extracted.chop(1);
            }
        }
        return extracted;
    }

    const Terminal_render_line_provenance* provenance_or_null() const
    {
        if (!valid()) {
            return nullptr;
        }

        const std::size_t row_index = static_cast<std::size_t>(m_row);
        return row_index < m_snapshot->visible_line_provenance.size()
            ? &m_snapshot->visible_line_provenance[row_index]
            : nullptr;
    }

    bool dirty() const
    {
        return render_snapshot_row_is_dirty(*m_snapshot, m_row);
    }

private:
    friend class Terminal_render_snapshot_row_content_view;

    Terminal_render_snapshot_row_content(
        const Terminal_render_snapshot& snapshot,
        int                             row,
        const_iterator                  first,
        const_iterator                  last)
    :
        m_snapshot(&snapshot),
        m_row(row),
        m_first(first),
        m_last(last)
    {}

    const Terminal_render_snapshot* m_snapshot = nullptr;
    int                             m_row      = 0;
    const_iterator                  m_first;
    const_iterator                  m_last;
};

class Terminal_render_snapshot_row_content_view
{
public:
    class const_iterator
    {
    public:
        Terminal_render_snapshot_row_content operator*() const
        {
            return m_view->row_at(m_row);
        }

        const_iterator& operator++()
        {
            ++m_row;
            return *this;
        }

        friend bool operator==(const const_iterator&, const const_iterator&) = default;

    private:
        friend class Terminal_render_snapshot_row_content_view;

        const_iterator(
            const Terminal_render_snapshot_row_content_view& view,
            int                                              row)
        :
            m_view(&view),
            m_row(row)
        {}

        const Terminal_render_snapshot_row_content_view* m_view = nullptr;
        int                                              m_row  = 0;
    };

    explicit Terminal_render_snapshot_row_content_view(
        const Terminal_render_snapshot& snapshot)
    :
        m_snapshot(snapshot)
    {}
    explicit Terminal_render_snapshot_row_content_view(
        const Terminal_render_snapshot&& snapshot) = delete;

    const Terminal_render_snapshot& snapshot() const { return m_snapshot; }
    terminal_grid_size_t grid_size() const { return m_snapshot.grid_size; }
    int row_count() const { return render_snapshot_viewable_row_count(m_snapshot); }
    int column_count() const { return m_snapshot.grid_size.columns; }

    const std::vector<Terminal_text_style>& styles() const { return m_snapshot.styles; }
    const std::vector<Terminal_render_dirty_row_range>& dirty_row_ranges() const
    {
        return m_snapshot.dirty_row_ranges;
    }
    const std::vector<Terminal_render_line_provenance>& visible_line_provenance() const
    {
        return m_snapshot.visible_line_provenance;
    }
    const std::vector<Terminal_render_selection_span>& selection_spans() const
    {
        return m_snapshot.selection_spans;
    }
    const std::vector<Terminal_render_hyperlink_metadata>& hyperlinks() const
    {
        return m_snapshot.hyperlinks;
    }
    const Terminal_render_cursor& cursor() const { return m_snapshot.cursor; }
    const Ime_preedit_state& ime_preedit() const { return m_snapshot.ime_preedit; }
    const Terminal_render_metadata& metadata() const { return m_snapshot.metadata; }
    const Terminal_mode_state& modes() const { return m_snapshot.modes; }
    std::size_t cell_count() const
    {
        if (m_snapshot.lazy_row_payloads == nullptr) {
            return m_snapshot.cells.size();
        }

        std::size_t count = 0U;
        for (int row = 0; row < row_count(); ++row) {
            count += row_at_unchecked(row).cell_count();
        }
        return count;
    }

    Terminal_render_snapshot_row_content row_at(int row) const
    {
        if (!render_snapshot_row_is_viewable(m_snapshot, row)) {
            return empty_row_at(row);
        }

        return row_at_unchecked(row);
    }

    const Terminal_render_cell* cell_at(int row, int column) const
    {
        if (!render_snapshot_row_is_viewable(m_snapshot, row)) {
            return nullptr;
        }

        return row_at_unchecked(row).cell_at(column);
    }

    QString row_text(
        int     row,
        int     first_column,
        int     end_column,
        bool    trim_trailing_spaces) const
    {
        if (!render_snapshot_row_is_viewable(m_snapshot, row)) {
            return {};
        }

        return row_at_unchecked(row).text(first_column, end_column, trim_trailing_spaces);
    }

    const Terminal_render_line_provenance* provenance_at(int row) const
    {
        if (!render_snapshot_row_is_viewable(m_snapshot, row)) {
            return nullptr;
        }

        return row_at_unchecked(row).provenance_or_null();
    }

    const_iterator begin() const { return const_iterator(*this, 0); }
    const_iterator end() const { return const_iterator(*this, row_count()); }

    std::vector<Terminal_render_cell> materialize_flat_cells(
        Terminal_render_snapshot_materialization_reason reason) const
    {
        Q_ASSERT(render_snapshot_materialization_reason_is_valid(reason));
        (void)reason;

        std::vector<Terminal_render_cell> cells;
        cells.reserve(cell_count());
        for (const Terminal_render_snapshot_row_content row : *this) {
            cells.insert(cells.end(), row.begin(), row.end());
        }
        return cells;
    }

    bool materialized_flat_cells_match_snapshot(
        Terminal_render_snapshot_materialization_reason reason) const
    {
        return materialized_flat_cells_match(m_snapshot.cells, reason);
    }

    bool materialized_flat_cells_match(
        const std::vector<Terminal_render_cell>&        expected,
        Terminal_render_snapshot_materialization_reason reason) const
    {
        const std::vector<Terminal_render_cell> materialized = materialize_flat_cells(reason);
        if (materialized.size() != expected.size()) {
            return false;
        }

        for (std::size_t i = 0; i < materialized.size(); ++i) {
            if (!render_snapshot_cells_equal(materialized[i], expected[i])) {
                return false;
            }
        }
        return true;
    }

private:
    Terminal_render_snapshot_row_content empty_row_at(int row) const
    {
        const auto end = m_snapshot.cells.end();
        return Terminal_render_snapshot_row_content(m_snapshot, row, end, end);
    }

    Terminal_render_snapshot_row_content row_at_unchecked(int row) const
    {
        Q_ASSERT(render_snapshot_row_is_viewable(m_snapshot, row));

        if (m_snapshot.lazy_row_payloads != nullptr) {
            const Terminal_render_snapshot_lazy_payloads* payloads =
                m_snapshot.lazy_row_payloads.get();
            if (payloads == nullptr ||
                static_cast<std::size_t>(row) >= payloads->rows.size())
            {
                return empty_row_at(row);
            }

            const Terminal_render_snapshot_lazy_row_payload& lazy_row =
                payloads->rows[static_cast<std::size_t>(row)];
            if (lazy_row.cells_in_receiving_namespace != nullptr) {
                return Terminal_render_snapshot_row_content(
                    m_snapshot,
                    row,
                    lazy_row.cells_in_receiving_namespace->begin(),
                    lazy_row.cells_in_receiving_namespace->end());
            }

            if (const Terminal_render_snapshot_immutable_row_payload* payload =
                    lazy_row.source.payload_or_null())
            {
                return Terminal_render_snapshot_row_content(
                    m_snapshot,
                    row,
                    payload->cells.begin(),
                    payload->cells.end());
            }

            if (lazy_row.source_snapshot != nullptr) {
                const Terminal_render_snapshot_row_content_view source_rows(
                    *lazy_row.source_snapshot);
                const Terminal_render_snapshot_row_content source_row =
                    source_rows.row_at(lazy_row.source_snapshot_row);
                return Terminal_render_snapshot_row_content(
                    m_snapshot,
                    row,
                    source_row.begin(),
                    source_row.end());
            }

            return empty_row_at(row);
        }

        const std::int64_t target_row = row;
        const auto first = std::lower_bound(
            m_snapshot.cells.begin(),
            m_snapshot.cells.end(),
            target_row,
            [](const Terminal_render_cell& cell, std::int64_t row) {
                return cell.position.row < row;
            });
        const auto last = std::lower_bound(
            first,
            m_snapshot.cells.end(),
            target_row + 1,
            [](const Terminal_render_cell& cell, std::int64_t row) {
                return cell.position.row < row;
            });
        return Terminal_render_snapshot_row_content(m_snapshot, row, first, last);
    }

    const Terminal_render_snapshot& m_snapshot;
};

inline std::shared_ptr<const Terminal_render_snapshot_row_payload_owner>
render_snapshot_row_payload_owner_from_snapshot(
    const Terminal_render_snapshot&                         snapshot,
    Terminal_render_snapshot_row_payload_namespace          metadata_namespace,
    std::uint64_t                                           retained_generation)
{
    const Terminal_render_snapshot_row_content_view rows(snapshot);
    std::vector<Terminal_render_snapshot_immutable_row_payload> payloads;
    payloads.reserve(static_cast<std::size_t>(rows.row_count()));

    for (const Terminal_render_snapshot_row_content row : rows) {
        Terminal_render_snapshot_immutable_row_payload payload;
        payload.identity.row                   = row.row();
        payload.identity.row_origin_generation = snapshot.metadata.row_origin_generation;
        payload.identity.metadata_namespace    = metadata_namespace;
        if (const Terminal_render_line_provenance* provenance =
                row.provenance_or_null())
        {
            payload.identity.provenance = *provenance;
        }
        payload.cells.assign(row.begin(), row.end());
        payloads.push_back(std::move(payload));
    }

    return std::make_shared<const Terminal_render_snapshot_row_payload_owner>(
        retained_generation,
        std::move(payloads));
}

inline QString selected_text_from_render_snapshot_row(
    const Terminal_render_snapshot_row_content& row,
    int                                         first_column,
    int                                         end_column,
    bool                                        trim_trailing_spaces)
{
    return row.text(first_column, end_column, trim_trailing_spaces);
}

inline Terminal_selection_result selected_text_from_render_snapshot(
    const Terminal_render_snapshot& snapshot,
    const Terminal_selection_range& selection)
{
    if (snapshot.grid_size.rows <= 0 || snapshot.grid_size.columns <= 0 ||
        selection.mode == Terminal_selection_mode::NONE)
    {
        return {Terminal_selection_result_code::INVALID_RANGE, {}};
    }

    const terminal_grid_position_t start = normalized_selection_start(selection);
    const terminal_grid_position_t end   = normalized_selection_end(selection);
    const int first_visible_logical_row  = render_snapshot_first_visible_logical_row(snapshot);

    if (start.row    < first_visible_logical_row                            ||
        end.row      >= first_visible_logical_row + snapshot.grid_size.rows ||
        start.column < 0                                                    ||
        start.column > snapshot.grid_size.columns                           ||
        end.column   < 0                                                    ||
        end.column   > snapshot.grid_size.columns)
    {
        return {Terminal_selection_result_code::INVALID_RANGE, {}};
    }

    const Terminal_render_snapshot_row_content_view rows(snapshot);
    QString text;
    for (int logical_row = start.row; logical_row <= end.row; ++logical_row) {
        const int first_column = logical_row == start.row ? start.column : 0;
        const int end_column =
            logical_row == end.row ? end.column : snapshot.grid_size.columns;
        if (end_column < first_column) {
            return {Terminal_selection_result_code::INVALID_RANGE, {}};
        }

        if (logical_row > start.row) {
            text += QLatin1Char('\n');
        }

        text += selected_text_from_render_snapshot_row(
            rows.row_at(logical_row - first_visible_logical_row),
            first_column,
            end_column,
            end_column == snapshot.grid_size.columns);
    }

    return {Terminal_selection_result_code::OK, text};
}

inline Terminal_render_cell_text_category render_cell_text_category_for_validation(
    const QString& text)
{
    if (text.isEmpty()) {
        return Terminal_render_cell_text_category::EMPTY;
    }

    constexpr unsigned int printable_ascii_first = 0x20U;
    constexpr unsigned int printable_ascii_last  = 0x7eU;

    unsigned int outside_printable_ascii = 0U;
    unsigned int non_ascii               = 0U;
    const qsizetype text_size            = text.size();
    const QChar* characters              = text.data();
    for (qsizetype index = 0; index < text_size; ++index) {
        const unsigned int code_unit = characters[index].unicode();
        outside_printable_ascii |= static_cast<unsigned int>(
            code_unit - printable_ascii_first >
                printable_ascii_last - printable_ascii_first);
        non_ascii |= code_unit;
    }

    if (outside_printable_ascii == 0U) {
        return Terminal_render_cell_text_category::PRINTABLE_ASCII;
    }

    return (non_ascii & ~0x7fU) != 0U
        ? Terminal_render_cell_text_category::NON_ASCII
        : Terminal_render_cell_text_category::OTHER_ASCII;
}

inline bool render_cell_text_category_is_valid(const Terminal_render_cell& cell)
{
    return
        cell.text.is_valid() &&
        (cell.text_category == Terminal_render_cell_text_category::UNKNOWN ||
            cell.text_category == cell.text.category());
}

inline bool render_snapshot_cell_is_strictly_after(
    const Terminal_render_cell& cell,
    const Terminal_render_cell& previous)
{
    return
        cell.position.row > previous.position.row ||
        (cell.position.row    == previous.position.row &&
         cell.position.column >  previous.position.column);
}

inline Terminal_render_snapshot_validation validate_render_snapshot_metadata(
    const Terminal_render_snapshot& snapshot)
{
    if (snapshot.grid_size.rows <= 0 || snapshot.grid_size.columns <= 0) {
        return {Terminal_render_snapshot_status::INVALID_GRID_SIZE};
    }

    if (snapshot.styles.empty()) {
        return {Terminal_render_snapshot_status::INVALID_STYLE_ID};
    }

    if (!(snapshot.styles.front() == make_default_terminal_text_style())) {
        return {Terminal_render_snapshot_status::INVALID_STYLE_ID};
    }

    if (snapshot.viewport.visible_rows             <= 0                                 ||
        snapshot.viewport.visible_rows             != snapshot.grid_size.rows           ||
        snapshot.viewport.scrollback_rows          <  0                                 ||
        snapshot.viewport.offset_from_tail         <  0                                 ||
        snapshot.viewport.offset_from_tail         >  snapshot.viewport.scrollback_rows ||
        (snapshot.viewport.active_buffer == Terminal_buffer_id::ALTERNATE &&
            (snapshot.viewport.scrollback_rows != 0 ||
                snapshot.viewport.offset_from_tail != 0)))
    {
        return {Terminal_render_snapshot_status::INVALID_VIEWPORT};
    }

    const bool public_projection_basis =
        snapshot.basis == Terminal_render_snapshot_basis::PUBLIC_PROJECTION;
    const bool scroll_purpose =
        snapshot.purpose == Terminal_render_snapshot_purpose::SCROLL;
    if (public_projection_basis != scroll_purpose) {
        return {Terminal_render_snapshot_status::INVALID_SNAPSHOT_BASIS_PURPOSE};
    }
    if (public_projection_basis &&
        snapshot.viewport.active_buffer != Terminal_buffer_id::PRIMARY)
    {
        return {Terminal_render_snapshot_status::INVALID_VIEWPORT};
    }
    if (public_projection_basis &&
        (snapshot.dirty_row_ranges.size() != 1U ||
            snapshot.dirty_row_ranges.front().first_row != 0 ||
            snapshot.dirty_row_ranges.front().row_count != snapshot.grid_size.rows))
    {
        return {Terminal_render_snapshot_status::INVALID_DIRTY_ROW_RANGE};
    }

    if (!snapshot.visible_line_provenance.empty() &&
        !render_snapshot_visible_line_provenance_is_valid(snapshot))
    {
        return {Terminal_render_snapshot_status::INVALID_LINE_PROVENANCE};
    }

    if (snapshot.visible_line_provenance.empty() && !snapshot.selection_spans.empty()) {
        return {Terminal_render_snapshot_status::INVALID_LINE_PROVENANCE};
    }

    std::int64_t next_dirty_row = 0;
    for (const Terminal_render_dirty_row_range& range : snapshot.dirty_row_ranges) {
        const std::int64_t first_row = range.first_row;
        const std::int64_t row_count = range.row_count;
        const std::int64_t end_row   = first_row + row_count;
        if (first_row < 0              ||
            row_count <= 0             ||
            first_row < next_dirty_row ||
            end_row   >  snapshot.grid_size.rows)
        {
            return {Terminal_render_snapshot_status::INVALID_DIRTY_ROW_RANGE};
        }
        next_dirty_row = static_cast<int>(end_row);
    }

    return {};
}

inline Terminal_render_snapshot_validation validate_render_snapshot_lazy_payloads(
    const Terminal_render_snapshot& snapshot)
{
    const Terminal_render_snapshot_lazy_payloads* payloads =
        snapshot.lazy_row_payloads.get();
    if (payloads == nullptr) {
        return {};
    }

    if (!snapshot.cells.empty()) {
        return {Terminal_render_snapshot_status::INVALID_CELL_ORDER};
    }

    if (payloads->rows.size() != static_cast<std::size_t>(snapshot.grid_size.rows)) {
        return {Terminal_render_snapshot_status::INVALID_LINE_PROVENANCE};
    }

    for (int row = 0; row < snapshot.grid_size.rows; ++row) {
        const Terminal_render_snapshot_lazy_row_payload& lazy_row =
            payloads->rows[static_cast<std::size_t>(row)];
        const Terminal_render_snapshot_immutable_row_payload* payload =
            lazy_row.source.payload_or_null();
        Terminal_render_snapshot_row_payload_identity identity;
        Terminal_render_snapshot_row_content::const_iterator source_first;
        Terminal_render_snapshot_row_content::const_iterator source_last;
        std::size_t source_cell_count = 0U;
        bool source_valid = false;
        if (payload != nullptr) {
            identity          = payload->identity;
            source_first      = payload->cells.begin();
            source_last       = payload->cells.end();
            source_cell_count = payload->cells.size();
            source_valid      = true;
        }
        else
        if (lazy_row.source_snapshot != nullptr) {
            const Terminal_render_snapshot_row_content_view source_rows(
                *lazy_row.source_snapshot);
            const Terminal_render_snapshot_row_content source_row =
                source_rows.row_at(lazy_row.source_snapshot_row);
            identity.row                   = lazy_row.source_snapshot_row;
            identity.row_origin_generation =
                lazy_row.source_snapshot->metadata.row_origin_generation;
            identity.metadata_namespace =
                lazy_row.source_snapshot_metadata_namespace;
            source_first      = source_row.begin();
            source_last       = source_row.end();
            source_cell_count = source_row.cell_count();
            source_valid      = source_row.valid();
            if (!lazy_row.source_snapshot->visible_line_provenance.empty() &&
                render_snapshot_row_is_viewable(
                    *lazy_row.source_snapshot,
                    lazy_row.source_snapshot_row))
            {
                identity.provenance =
                    lazy_row.source_snapshot->visible_line_provenance[
                        static_cast<std::size_t>(lazy_row.source_snapshot_row)];
            }
        }

        if (!source_valid ||
            identity.row != row ||
            identity.row_origin_generation !=
                snapshot.metadata.row_origin_generation)
        {
            return {Terminal_render_snapshot_status::INVALID_LINE_PROVENANCE};
        }

        if (!snapshot.visible_line_provenance.empty() &&
            !(identity.provenance ==
                snapshot.visible_line_provenance[static_cast<std::size_t>(row)]))
        {
            return {Terminal_render_snapshot_status::INVALID_LINE_PROVENANCE};
        }

        if (lazy_row.receiving_namespace.style_table_generation !=
            payloads->receiving_namespace.style_table_generation)
        {
            return {Terminal_render_snapshot_status::INVALID_STYLE_ID};
        }

        if (lazy_row.receiving_namespace.hyperlink_namespace_generation !=
            payloads->receiving_namespace.hyperlink_namespace_generation)
        {
            return {Terminal_render_snapshot_status::INVALID_HYPERLINK_METADATA};
        }

        if (lazy_row.cells_in_receiving_namespace == nullptr) {
            if (identity.metadata_namespace.style_table_generation !=
                lazy_row.receiving_namespace.style_table_generation)
            {
                return {Terminal_render_snapshot_status::INVALID_STYLE_ID};
            }

            if (identity.metadata_namespace.hyperlink_namespace_generation !=
                lazy_row.receiving_namespace.hyperlink_namespace_generation)
            {
                return {Terminal_render_snapshot_status::INVALID_HYPERLINK_METADATA};
            }
        }
        else {
            const std::vector<Terminal_render_cell>& remapped_cells =
                *lazy_row.cells_in_receiving_namespace;
            if (remapped_cells.size() != source_cell_count) {
                return {Terminal_render_snapshot_status::INVALID_LINE_PROVENANCE};
            }

            auto source_cell = source_first;
            for (const Terminal_render_cell& remapped_cell : remapped_cells) {
                const Terminal_render_cell& source_cell_ref = *source_cell;
                if (!render_snapshot_cells_equal_except_metadata_ids(
                        source_cell_ref,
                        remapped_cell))
                {
                    return {Terminal_render_snapshot_status::INVALID_LINE_PROVENANCE};
                }

                if ((source_cell_ref.hyperlink_id == 0U) !=
                    (remapped_cell.hyperlink_id == 0U))
                {
                    return {Terminal_render_snapshot_status::INVALID_HYPERLINK_METADATA};
                }
                ++source_cell;
            }
            if (source_cell != source_last) {
                return {Terminal_render_snapshot_status::INVALID_LINE_PROVENANCE};
            }
        }
    }

    return {};
}

inline Terminal_render_snapshot_validation validate_render_snapshot_flat_cells(
    const Terminal_render_snapshot& snapshot)
{
    for (std::size_t i = 0; i < snapshot.cells.size(); ++i) {
        const Terminal_render_cell& cell = snapshot.cells[i];
        if (cell.position.row    < 0  || cell.position.row    >= snapshot.grid_size.rows ||
            cell.position.column < 0  || cell.position.column >= snapshot.grid_size.columns)
        {
            return {Terminal_render_snapshot_status::INVALID_CELL_POSITION};
        }

        // Snapshot cells are stored row-major with strictly ascending columns
        // within each row. Producers emit cells in this order, and flat
        // materialization boundaries rely on it. Enforce the contract here so a
        // violating snapshot fails loudly instead of mis-rendering.
        if (i > 0 &&
            !render_snapshot_cell_is_strictly_after(cell, snapshot.cells[i - 1]))
        {
            return {Terminal_render_snapshot_status::INVALID_CELL_ORDER};
        }

        if (static_cast<std::size_t>(cell.style_id) >= snapshot.styles.size()) {
            return {Terminal_render_snapshot_status::INVALID_STYLE_ID};
        }

        if (cell.wide_continuation) {
            if (!cell.text.is_empty() || cell.display_width != 0 || cell.position.column == 0) {
                return {Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION};
            }
            if (!render_cell_text_category_is_valid(cell)) {
                return {Terminal_render_snapshot_status::INVALID_CELL_TEXT_CATEGORY};
            }
            continue;
        }

        if (cell.display_width <= 0 ||
            cell.display_width >  snapshot.grid_size.columns - cell.position.column)
        {
            return {Terminal_render_snapshot_status::INVALID_CELL_WIDTH};
        }

        if (!render_cell_text_category_is_valid(cell)) {
            return {Terminal_render_snapshot_status::INVALID_CELL_TEXT_CATEGORY};
        }
    }

    return {};
}

inline Terminal_render_snapshot_validation validate_render_snapshot_row_view_cells(
    const Terminal_render_snapshot_row_content_view& rows)
{
    const Terminal_render_snapshot& snapshot = rows.snapshot();
    const Terminal_render_cell* previous = nullptr;
    for (const Terminal_render_snapshot_row_content row : rows) {
        for (const Terminal_render_cell& cell : row) {
            if (cell.position.row    != row.row() ||
                cell.position.row    < 0          ||
                cell.position.row    >= snapshot.grid_size.rows ||
                cell.position.column < 0  || cell.position.column >= snapshot.grid_size.columns)
            {
                return {Terminal_render_snapshot_status::INVALID_CELL_POSITION};
            }

            if (previous != nullptr &&
                !render_snapshot_cell_is_strictly_after(cell, *previous))
            {
                return {Terminal_render_snapshot_status::INVALID_CELL_ORDER};
            }

            if (static_cast<std::size_t>(cell.style_id) >= snapshot.styles.size()) {
                return {Terminal_render_snapshot_status::INVALID_STYLE_ID};
            }

            if (cell.wide_continuation) {
                if (!cell.text.is_empty() ||
                    cell.display_width != 0 ||
                    cell.position.column == 0)
                {
                    return {Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION};
                }
                if (!render_cell_text_category_is_valid(cell)) {
                    return {Terminal_render_snapshot_status::INVALID_CELL_TEXT_CATEGORY};
                }
                previous = &cell;
                continue;
            }

            if (cell.display_width <= 0 ||
                cell.display_width >  snapshot.grid_size.columns - cell.position.column)
            {
                return {Terminal_render_snapshot_status::INVALID_CELL_WIDTH};
            }

            if (!render_cell_text_category_is_valid(cell)) {
                return {Terminal_render_snapshot_status::INVALID_CELL_TEXT_CATEGORY};
            }
            previous = &cell;
        }
    }

    return {};
}

inline Terminal_render_snapshot_validation validate_render_snapshot_wide_cells(
    const Terminal_render_snapshot& snapshot)
{
    const Terminal_render_cell* active_base                = nullptr;
    int                         active_end_column          = 0;
    int                         next_continuation_column   = 0;
    bool                        active_style_mismatch_seen = false;

    for (const Terminal_render_cell& cell : snapshot.cells) {
        if (active_base != nullptr &&
            cell.position.row != active_base->position.row)
        {
            return {Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION};
        }

        if (active_base != nullptr && cell.position.column >= active_end_column) {
            return {Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION};
        }

        if (cell.wide_continuation) {
            if (active_base == nullptr ||
                cell.position.column != next_continuation_column)
            {
                return {Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION};
            }

            if (active_base->style_id     != cell.style_id ||
                active_base->hyperlink_id != cell.hyperlink_id)
            {
                active_style_mismatch_seen = true;
            }

            ++next_continuation_column;
            if (next_continuation_column >= active_end_column) {
                if (active_style_mismatch_seen) {
                    return {Terminal_render_snapshot_status::INVALID_WIDE_CELL_STYLE};
                }

                active_base                = nullptr;
                active_style_mismatch_seen = false;
            }
            continue;
        }

        if (active_base != nullptr &&
            cell.position.column < active_end_column)
        {
            return {Terminal_render_snapshot_status::INVALID_CELL_OVERLAP};
        }

        if (cell.display_width > 1) {
            active_base                = &cell;
            active_end_column          = cell.position.column + cell.display_width;
            next_continuation_column   = cell.position.column + 1;
            active_style_mismatch_seen = false;
        }
    }

    if (active_base != nullptr) {
        return {Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION};
    }

    return {};
}

inline Terminal_render_snapshot_validation validate_render_snapshot_row_view_wide_cells(
    const Terminal_render_snapshot_row_content_view& rows)
{
    const Terminal_render_cell* active_base                = nullptr;
    int                         active_end_column          = 0;
    int                         next_continuation_column   = 0;
    bool                        active_style_mismatch_seen = false;

    for (const Terminal_render_snapshot_row_content row : rows) {
        for (const Terminal_render_cell& cell : row) {
            if (active_base != nullptr &&
                cell.position.row != active_base->position.row)
            {
                return {Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION};
            }

            if (active_base != nullptr && cell.position.column >= active_end_column) {
                return {Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION};
            }

            if (cell.wide_continuation) {
                if (active_base == nullptr ||
                    cell.position.column != next_continuation_column)
                {
                    return {Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION};
                }

                if (active_base->style_id     != cell.style_id ||
                    active_base->hyperlink_id != cell.hyperlink_id)
                {
                    active_style_mismatch_seen = true;
                }

                ++next_continuation_column;
                if (next_continuation_column >= active_end_column) {
                    if (active_style_mismatch_seen) {
                        return {Terminal_render_snapshot_status::INVALID_WIDE_CELL_STYLE};
                    }

                    active_base                = nullptr;
                    active_style_mismatch_seen = false;
                }
                continue;
            }

            if (active_base != nullptr &&
                cell.position.column < active_end_column)
            {
                return {Terminal_render_snapshot_status::INVALID_CELL_OVERLAP};
            }

            if (cell.display_width > 1) {
                active_base                = &cell;
                active_end_column          = cell.position.column + cell.display_width;
                next_continuation_column   = cell.position.column + 1;
                active_style_mismatch_seen = false;
            }
        }
    }

    if (active_base != nullptr) {
        return {Terminal_render_snapshot_status::INVALID_WIDE_CELL_CONTINUATION};
    }

    return {};
}

inline Terminal_render_snapshot_validation validate_render_snapshot_cell_hyperlinks(
    const Terminal_render_snapshot&  snapshot,
    const std::set<std::uint64_t>&   hyperlink_ids)
{
    for (const Terminal_render_cell& cell : snapshot.cells) {
        if (cell.hyperlink_id                     != 0U &&
            hyperlink_ids.find(cell.hyperlink_id) == hyperlink_ids.end())
        {
            return {Terminal_render_snapshot_status::INVALID_HYPERLINK_METADATA};
        }
    }

    return {};
}

inline Terminal_render_snapshot_validation validate_render_snapshot_row_view_hyperlinks(
    const Terminal_render_snapshot_row_content_view& rows,
    const std::set<std::uint64_t>&                   hyperlink_ids)
{
    for (const Terminal_render_snapshot_row_content row : rows) {
        for (const Terminal_render_cell& cell : row) {
            if (cell.hyperlink_id                     != 0U &&
                hyperlink_ids.find(cell.hyperlink_id) == hyperlink_ids.end())
            {
                return {Terminal_render_snapshot_status::INVALID_HYPERLINK_METADATA};
            }
        }
    }

    return {};
}

inline Terminal_render_snapshot_validation validate_render_snapshot_overlay_metadata(
    const Terminal_render_snapshot& snapshot)
{
    if (snapshot.cursor.visible &&
        (snapshot.cursor.position.row < 0 || snapshot.cursor.position.row >= snapshot.grid_size.rows ||
         snapshot.cursor.position.column <  0 ||
         snapshot.cursor.position.column >= snapshot.grid_size.columns))
    {
        return {Terminal_render_snapshot_status::INVALID_CURSOR_POSITION};
    }

    for (const Terminal_render_selection_span& span : snapshot.selection_spans) {
        if (span.row < 0          || span.row >= snapshot.grid_size.rows ||
            span.first_column < 0 || span.column_count <= 0              ||
            span.first_column + span.column_count > snapshot.grid_size.columns)
        {
            return {Terminal_render_snapshot_status::INVALID_SELECTION_SPAN};
        }
    }

    return {};
}

inline Terminal_render_snapshot_validation validate_render_snapshot(
    const Terminal_render_snapshot& snapshot)
{
    std::set<std::uint64_t> hyperlink_ids;
    const Terminal_render_snapshot_validation metadata =
        validate_render_snapshot_metadata(snapshot);
    if (metadata.status != Terminal_render_snapshot_status::OK) {
        return metadata;
    }

    if (!collect_render_snapshot_hyperlink_ids(snapshot, hyperlink_ids)) {
        return {Terminal_render_snapshot_status::INVALID_HYPERLINK_METADATA};
    }

    const Terminal_render_snapshot_validation lazy_payloads =
        validate_render_snapshot_lazy_payloads(snapshot);
    if (lazy_payloads.status != Terminal_render_snapshot_status::OK) {
        return lazy_payloads;
    }

    if (snapshot.lazy_row_payloads == nullptr) {
        const Terminal_render_snapshot_validation flat_cells =
            validate_render_snapshot_flat_cells(snapshot);
        if (flat_cells.status != Terminal_render_snapshot_status::OK) {
            return flat_cells;
        }

        const Terminal_render_snapshot_validation wide_cells =
            validate_render_snapshot_wide_cells(snapshot);
        if (wide_cells.status != Terminal_render_snapshot_status::OK) {
            return wide_cells;
        }

        const Terminal_render_snapshot_validation cell_hyperlinks =
            validate_render_snapshot_cell_hyperlinks(snapshot, hyperlink_ids);
        if (cell_hyperlinks.status != Terminal_render_snapshot_status::OK) {
            return cell_hyperlinks;
        }
    }
    else {
        const Terminal_render_snapshot_row_content_view rows(snapshot);
        const Terminal_render_snapshot_validation row_cells =
            validate_render_snapshot_row_view_cells(rows);
        if (row_cells.status != Terminal_render_snapshot_status::OK) {
            return row_cells;
        }

        const Terminal_render_snapshot_validation wide_cells =
            validate_render_snapshot_row_view_wide_cells(rows);
        if (wide_cells.status != Terminal_render_snapshot_status::OK) {
            return wide_cells;
        }

        const Terminal_render_snapshot_validation cell_hyperlinks =
            validate_render_snapshot_row_view_hyperlinks(rows, hyperlink_ids);
        if (cell_hyperlinks.status != Terminal_render_snapshot_status::OK) {
            return cell_hyperlinks;
        }
    }

    const Terminal_render_snapshot_validation overlay_metadata =
        validate_render_snapshot_overlay_metadata(snapshot);
    if (overlay_metadata.status != Terminal_render_snapshot_status::OK) {
        return overlay_metadata;
    }

    return {};
}

}
