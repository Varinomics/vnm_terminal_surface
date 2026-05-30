#pragma once

#include "vnm_terminal/internal/render_snapshot.h"
#include <optional>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vnm_terminal::internal {

class Terminal_screen_model;

struct Terminal_public_projection_row
{
    // Cell positions are row-relative inside this copied row. Stable row
    // identity is carried by public_row.
    std::int64_t                         public_row = 0;
    Terminal_render_line_provenance      provenance;
    terminal_history_handle_t            history_handle;
    // Duplicate visual fragment ordinal within the retained line identified by
    // provenance.
    int                                  visual_fragment_index = 0;
    // Viewport-only fallback captures can only prove the ordinal relative to the
    // copied viewport. Full-row captures prove the true retained-line ordinal.
    bool                                 visual_fragment_index_is_exact = false;
    std::vector<Terminal_render_cell>    cells;
};

struct Terminal_public_projection_row_metadata
{
    Terminal_render_line_provenance      provenance;
    terminal_history_handle_t            history_handle;
    // Duplicate visual fragment ordinal within the retained line identified by
    // provenance.
    int                                  visual_fragment_index = 0;
    bool                                 visual_fragment_index_is_exact = false;
};

struct Terminal_public_viewport_anchor
{
    std::uint64_t      public_projection_generation = 0U;
    Terminal_buffer_id active_buffer = Terminal_buffer_id::PRIMARY;
    std::uint64_t      active_buffer_epoch = 0U;
    std::uint64_t      geometry_generation = 0U;
    terminal_history_handle_t history_handle;
    // Duplicate visual fragment ordinal within the retained line.
    int                visual_fragment_index = 0;
    bool               visual_fragment_index_is_exact = false;
    int                viewport_row = 0;
    bool               sticky_tail = false;
};

struct Terminal_public_release_intent
{
    bool                                            has_public_viewport = false;
    bool                                            public_projection_valid = false;
    bool                                            deferred_intent_recorded = false;
    std::uint64_t                                   public_projection_generation = 0U;
    Terminal_viewport_state                         public_viewport;
    std::uint64_t                                   active_buffer_epoch = 0U;
    bool                                            sticky_tail = false;
    std::optional<Terminal_public_viewport_anchor>  detached_anchor;
    Terminal_public_scroll_diagnostic_reason        diagnostic_reason =
        Terminal_public_scroll_diagnostic_reason::NONE;
    Terminal_public_projection_disable_reason       public_projection_disable_reason =
        Terminal_public_projection_disable_reason::NONE;
    std::optional<int>                              deferred_offset_from_tail;
    int                                             deferred_line_delta = 0;
    Terminal_hidden_row_eligibility                 hidden_row_eligibility =
        Terminal_hidden_row_eligibility::NOT_EVALUATED;
    Terminal_hidden_row_clamp_reason                hidden_row_clamp_reason =
        Terminal_hidden_row_clamp_reason::NONE;
};

struct Terminal_public_viewport_scroll_result
{
    Terminal_viewport_scroll_result viewport_result;
    bool                            invalidated_public_projection = false;
    bool                            deferred_release_intent_recorded = false;
};

class Terminal_public_projection
{
public:
    static Terminal_public_projection capture_from_safe_model(
        std::uint64_t                          generation,
        const Terminal_render_snapshot&        safe_basis,
        terminal_selection_content_basis_t     content_basis,
        std::uint64_t                          active_buffer_epoch);
    static Terminal_public_projection capture_primary_full_rows_from_safe_model(
        std::uint64_t                          generation,
        const Terminal_render_snapshot&        safe_basis,
        terminal_selection_content_basis_t     content_basis,
        std::uint64_t                          active_buffer_epoch,
        // The model is only a valid source at the DECSET 2026 entry boundary,
        // after safe-prefix publication and before any synchronized-output
        // payload bytes are parsed.
        const Terminal_screen_model&           safe_model);
    static Terminal_public_projection with_copied_rows_for_testing(
        const Terminal_public_projection&       source,
        int                                     first_copied_public_row,
        std::vector<Terminal_public_projection_row>
                                                rows);

    std::uint64_t generation() const { return m_generation; }
    terminal_selection_content_basis_t content_basis() const { return m_content_basis; }
    std::uint64_t active_buffer_epoch() const { return m_active_buffer_epoch; }
    Terminal_render_snapshot_basis source_basis() const { return m_source_basis; }
    Terminal_render_snapshot_purpose source_purpose() const { return m_source_purpose; }
    Terminal_render_snapshot_status basis_validation_status() const
    {
        return m_basis_validation_status;
    }

    Terminal_buffer_id active_buffer() const { return m_viewport.active_buffer; }
    terminal_grid_size_t grid_size() const { return m_grid_size; }
    Terminal_viewport_state viewport() const { return m_viewport; }
    const Terminal_color_state& color_state() const { return m_color_state; }
    const std::vector<Terminal_text_style>& styles() const { return m_styles; }
    const std::vector<Terminal_render_hyperlink_metadata>& hyperlinks() const
    {
        return m_hyperlinks;
    }
    const Terminal_render_cursor& cursor() const { return m_cursor; }
    const Ime_preedit_state& ime_preedit() const { return m_ime_preedit; }
    const Terminal_mode_state& modes() const { return m_modes; }
    const Terminal_render_metadata& metadata() const { return m_metadata; }
    const Terminal_public_scroll_diagnostics& public_scroll_diagnostics() const
    {
        return m_public_scroll_diagnostics;
    }
    const std::vector<Terminal_render_dirty_row_range>&
        safe_basis_viewport_dirty_row_ranges() const
    {
        return m_safe_basis_viewport_dirty_row_ranges;
    }
    const std::vector<Terminal_render_selection_span>&
        safe_basis_viewport_selection_spans() const
    {
        return m_safe_basis_viewport_selection_spans;
    }
    const std::vector<Terminal_public_projection_row>& rows() const { return m_rows; }

    int safe_basis_scrollback_depth() const { return m_safe_basis_scrollback_depth; }
    int safe_basis_active_grid_rows() const { return m_safe_basis_active_grid_rows; }
    std::size_t stored_row_count() const { return m_rows.size(); }
    std::size_t copied_row_bound() const { return m_copied_row_bound; }
    std::size_t row_capture_snapshot_count() const { return m_row_capture_snapshot_count; }
    int first_copied_public_row() const { return m_first_copied_public_row; }
    bool rows_are_safe_basis_viewport_only() const
    {
        return m_rows_are_safe_basis_viewport_only;
    }

private:
    std::uint64_t                                      m_generation = 0U;
    terminal_selection_content_basis_t                 m_content_basis;
    std::uint64_t                                      m_active_buffer_epoch = 0U;
    Terminal_render_snapshot_basis                     m_source_basis =
        Terminal_render_snapshot_basis::LIVE_CONTENT;
    Terminal_render_snapshot_purpose                   m_source_purpose =
        Terminal_render_snapshot_purpose::CONTENT;
    Terminal_render_snapshot_status                    m_basis_validation_status =
        Terminal_render_snapshot_status::OK;
    terminal_grid_size_t                               m_grid_size;
    Terminal_viewport_state                            m_viewport;
    Terminal_color_state                               m_color_state;
    std::vector<Terminal_text_style>                   m_styles;
    std::vector<Terminal_render_hyperlink_metadata>    m_hyperlinks;
    Terminal_render_cursor                             m_cursor;
    Ime_preedit_state                                  m_ime_preedit;
    Terminal_mode_state                                m_modes;
    Terminal_render_metadata                           m_metadata;
    Terminal_public_scroll_diagnostics                 m_public_scroll_diagnostics;
    std::vector<Terminal_render_dirty_row_range>
                                                       m_safe_basis_viewport_dirty_row_ranges;
    std::vector<Terminal_render_selection_span>
                                                       m_safe_basis_viewport_selection_spans;
    std::vector<Terminal_public_projection_row>        m_rows;
    int                                                m_safe_basis_scrollback_depth = 0;
    int                                                m_safe_basis_active_grid_rows = 0;
    int                                                m_first_copied_public_row = 0;
    std::size_t                                        m_copied_row_bound = 0U;
    std::size_t                                        m_row_capture_snapshot_count = 0U;
    bool                                               m_rows_are_safe_basis_viewport_only = true;
};

class Terminal_public_viewport_controller
{
public:
    void reset();
    void initialize_from_projection(const Terminal_public_projection& projection);
    void initialize_from_copied_rows(
        Terminal_viewport_state                         viewport,
        std::uint64_t                                   public_projection_generation,
        terminal_selection_content_basis_t              content_basis,
        std::uint64_t                                   active_buffer_epoch,
        int                                             first_copied_public_row,
        std::vector<Terminal_public_projection_row_metadata>
                                                        copied_row_metadata);

    bool has_public_viewport() const { return m_release_intent.has_public_viewport; }
    bool public_projection_valid() const { return m_release_intent.public_projection_valid; }
    const Terminal_viewport_state& viewport() const { return m_release_intent.public_viewport; }
    const Terminal_public_release_intent& release_intent() const { return m_release_intent; }

    Terminal_public_viewport_scroll_result scroll_lines(int line_delta);
    Terminal_public_viewport_scroll_result scroll_to_offset_from_tail(int offset_from_tail);
    Terminal_public_viewport_scroll_result scroll_to_tail();
    void invalidate(
        Terminal_public_projection_disable_reason reason,
        Terminal_public_scroll_diagnostic_reason  diagnostic_reason =
            Terminal_public_scroll_diagnostic_reason::NONE);
    void record_selection_mutation_unsupported();

private:
    int max_offset_from_tail() const;
    int deferred_scroll_base_offset_from_tail() const;
    int deferred_line_delta_from_public_viewport(int offset_from_tail) const;
    int first_public_row_for_offset(int offset_from_tail) const;
    bool viewport_rows_are_copied_at_offset(int offset_from_tail) const;
    void refresh_detached_anchor();
    void set_sticky_tail(bool sticky_tail);
    Terminal_public_viewport_scroll_result apply_offset(
        int    target_offset_from_tail,
        bool   sticky_tail_after_request,
        bool   request_records_detached_intent);
    Terminal_public_viewport_scroll_result record_deferred_offset_intent(
        int    offset_from_tail,
        int    line_delta,
        bool   sticky_tail_after_request);

    terminal_selection_content_basis_t              m_content_basis;
    std::uint64_t                                   m_active_buffer_epoch = 0U;
    int                                             m_first_copied_public_row = 0;
    std::vector<Terminal_public_projection_row_metadata>
                                                    m_copied_row_metadata;
    Terminal_public_release_intent                  m_release_intent;
};

}
