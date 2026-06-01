#pragma once

#include "vnm_terminal/internal/parser_action.h"
#include "vnm_terminal/internal/render_snapshot.h"
#include "vnm_terminal/internal/utf8_scan.h"
#include "vnm_terminal/internal/terminal_input_mode.h"
#include "vnm_terminal/internal/terminal_style.h"
#include <QByteArray>
#include <QByteArrayView>
#include <QChar>
#include <QString>
#include <QStringView>
#include <QtGlobal>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace vnm_terminal::internal {

class Terminal_history_ring;
class Terminal_history_row_traversal;
struct Terminal_history_row_record;

constexpr int         k_terminal_screen_model_max_rows    = 4096;
constexpr int         k_terminal_screen_model_max_columns = 4096;
constexpr std::size_t k_terminal_screen_model_max_cells   = 1024U * 1024U;

inline bool is_terminal_screen_model_grid_size_supported(terminal_grid_size_t grid_size)
{
    if (grid_size.rows <= 0 || grid_size.columns <= 0) {
        return false;
    }

    if (grid_size.rows    > k_terminal_screen_model_max_rows ||
        grid_size.columns > k_terminal_screen_model_max_columns)
    {
        return false;
    }

    const std::size_t rows    = static_cast<std::size_t>(grid_size.rows);
    const std::size_t columns = static_cast<std::size_t>(grid_size.columns);
    return rows <= k_terminal_screen_model_max_cells / columns;
}

struct Terminal_screen_model_config
{
    terminal_grid_size_t   grid_size;
    int                    scrollback_limit                         = 1000;
    int                    tab_width                                = 8;
    bool                   recover_scrollback_from_primary_repaints = false;
    bool                   retain_structural_actions                = true;
};

enum class Terminal_screen_model_config_status
{
    OK,
    INVALID_GRID_SIZE,
    INVALID_SCROLLBACK_LIMIT,
    INVALID_TAB_WIDTH,
};

Terminal_screen_model_config_status validate_terminal_screen_model_config(
    const Terminal_screen_model_config&    config);

enum class Terminal_retained_line_provenance_source
{
    TERMINAL_STORAGE,
    RECOVERED_PRIMARY_REPAINT,
};

struct Terminal_retained_line_provenance
{
    std::uint64_t                            retained_line_id   = 0U;
    std::uint64_t                            content_generation = 0U;
    Terminal_retained_line_provenance_source source =
        Terminal_retained_line_provenance_source::TERMINAL_STORAGE;
};

enum class Terminal_retained_row_style_lifetime
{
    SESSION_LIFETIME_STYLE_ID,
};

enum class Terminal_retained_row_wrap_state
{
    HARD_BOUNDARY,
};

struct terminal_retained_row_record_metadata_t
{
    int                                   source_width = 0;
    Terminal_retained_row_style_lifetime style_lifetime =
        Terminal_retained_row_style_lifetime::SESSION_LIFETIME_STYLE_ID;
    Terminal_retained_row_wrap_state     wrap_state =
        Terminal_retained_row_wrap_state::HARD_BOUNDARY;
};

struct Terminal_retained_line_lookup_result
{
    Terminal_history_resolution_status resolution_status =
        Terminal_history_resolution_status::INVALID_HANDLE;
    bool exact_match = false;
    int  exact_logical_row = 0;
    bool nearest_successor = false;
    int  nearest_successor_logical_row = 0;
    bool nearest_predecessor = false;
    int  nearest_predecessor_logical_row = 0;
    bool retained_line_id_found = false;
    int  retained_line_id_match_count = 0;
    bool retained_line_content_generation_mismatch = false;
};

enum class Terminal_backing_delta_kind
{
    BACKING_UNCHANGED,
    PRIMARY_HISTORY_APPENDED,
    PRIMARY_HISTORY_EVICTED,
    PRIMARY_HISTORY_CLEARED,
    PRIMARY_HISTORY_DISCARDED,
    ACTIVE_GRID_RESIZED,
    COLUMN_REFLOWED,
    MODE_TRANSITIONED,
};

struct terminal_backing_delta_t
{
    Terminal_backing_delta_kind kind =
        Terminal_backing_delta_kind::BACKING_UNCHANGED;
    Terminal_buffer_id          buffer_id = Terminal_buffer_id::PRIMARY;
    Terminal_buffer_id          active_buffer_before = Terminal_buffer_id::PRIMARY;
    Terminal_buffer_id          active_buffer_after = Terminal_buffer_id::PRIMARY;
    terminal_grid_size_t        grid_size_before;
    terminal_grid_size_t        grid_size_after;
    int                         scrollback_rows_before = 0;
    int                         scrollback_rows_after = 0;
    int                         appended_scrollback_rows = 0;
    int                         evicted_scrollback_rows = 0;
    int                         discarded_scrollback_rows = 0;
};

enum class Terminal_recovery_proposal_reason
{
    PRIMARY_REPAINT_SHIFTED_VISIBLE_ROWS,
};

enum class Terminal_recovery_proposal_status
{
    ACCEPTED,
};

struct terminal_recovery_proposal_t
{
    Terminal_recovery_proposal_reason           reason =
        Terminal_recovery_proposal_reason::PRIMARY_REPAINT_SHIFTED_VISIBLE_ROWS;
    Terminal_recovery_proposal_status           status =
        Terminal_recovery_proposal_status::ACCEPTED;
    Terminal_retained_line_provenance_source    provenance_source =
        Terminal_retained_line_provenance_source::RECOVERED_PRIMARY_REPAINT;
    int                                         candidate_visible_rows = 0;
    int                                         recovered_row_count = 0;
    bool                                        visible_row_identity_ambiguous = false;
};

struct Terminal_screen_model_result
{
    std::vector<Parser_action> actions;
    std::vector<int>           dirty_rows;
    std::vector<terminal_backing_delta_t>
                               backing_deltas;
    std::vector<terminal_recovery_proposal_t>
                               recovery_proposals;
    bool                       dirty_rows_have_stable_mutation_identity = true;
    bool                       terminal_content_changed     = false;
    bool                       active_buffer_changed        = false;
    bool                       grid_reflow_changed          = false;
    bool                       viewport_changed              = false;
    bool                       mode_state_changed            = false;
    bool                       mouse_reporting_mode_changed  = false;
    bool                       alternate_scroll_mode_changed = false;
    int                        scrollback_rows               = 0;
    int                        evicted_scrollback_rows       = 0;
};

struct Terminal_screen_model_dirty_row_stats
{
    bool                       enabled                     = false;
    std::uint64_t              mark_requests               = 0U;
    std::uint64_t              duplicate_mark_requests     = 0U;
    std::uint64_t              out_of_bounds_mark_requests = 0U;
    std::uint64_t              unique_pending_row_marks    = 0U;
    std::uint64_t              mark_all_dirty_calls        = 0U;
    std::uint64_t              dirty_rows_snapshot_calls   = 0U;
    std::uint64_t              dirty_rows_snapshot_rows    = 0U;
    std::uint64_t              collect_synchronized_calls  = 0U;
    std::uint64_t              collect_synchronized_rows   = 0U;
    std::uint64_t              publish_pending_calls       = 0U;
    std::uint64_t              published_unique_rows       = 0U;
    std::uint64_t              release_synchronized_calls  = 0U;
    std::uint64_t              released_synchronized_rows  = 0U;
    std::uint64_t              max_pending_dirty_rows      = 0U;
    std::uint64_t              max_synchronized_dirty_rows = 0U;
};

struct Terminal_screen_model_dirty_row_bucket_stats
{
    std::uint64_t              start_ms                    = 0U;
    std::uint64_t              end_ms                      = 0U;
    std::uint64_t              mark_requests               = 0U;
    std::uint64_t              duplicate_mark_requests     = 0U;
    std::uint64_t              out_of_bounds_mark_requests = 0U;
    std::uint64_t              unique_pending_row_marks    = 0U;
    std::uint64_t              mark_all_dirty_calls        = 0U;
    std::uint64_t              dirty_rows_snapshot_calls   = 0U;
    std::uint64_t              dirty_rows_snapshot_rows    = 0U;
    std::uint64_t              collect_synchronized_calls  = 0U;
    std::uint64_t              collect_synchronized_rows   = 0U;
    std::uint64_t              publish_pending_calls       = 0U;
    std::uint64_t              published_unique_rows       = 0U;
    std::uint64_t              release_synchronized_calls  = 0U;
    std::uint64_t              released_synchronized_rows  = 0U;
    std::uint64_t              max_pending_dirty_rows      = 0U;
    std::uint64_t              max_synchronized_dirty_rows = 0U;
};

struct Terminal_screen_model_dirty_row_timeline
{
    std::uint64_t              bucket_width_ms             = 100U;
    std::vector<Terminal_screen_model_dirty_row_bucket_stats>
                               buckets;
};

struct Terminal_screen_model_profile_stats
{
    bool                       enabled                                  = false;
    std::uint64_t              print_text_calls                         = 0U;
    std::uint64_t              printable_ascii_span_calls               = 0U;
    std::uint64_t              printable_ascii_span_characters          = 0U;
    std::uint64_t              printable_ascii_cells_written            = 0U;
    std::uint64_t              max_printable_ascii_span_characters      = 0U;
    std::uint64_t              printable_ascii_local_cells_inspected     = 0U;
    std::uint64_t              scalar_span_local_cells_inspected         = 0U;
    std::uint64_t              row_content_generation_comparisons       = 0U;
    std::uint64_t              row_content_generation_comparison_cells  = 0U;
    std::uint64_t              row_content_generation_advances          = 0U;
    std::uint64_t              wide_boundary_repairs_from_text_writes   = 0U;
    std::uint64_t              dirty_marks_from_text_writes             = 0U;
    std::uint64_t              line_wraps_from_text_writes              = 0U;
    std::uint64_t              scrollback_appends_from_text_writes      = 0U;
    std::uint64_t              render_snapshot_requests                 = 0U;
    std::uint64_t              render_snapshots_constructed             = 0U;
    std::uint64_t              render_snapshot_rows_visited             = 0U;
    std::uint64_t              render_snapshot_rows_materialized        = 0U;
    std::uint64_t              render_snapshot_rows_borrowed            = 0U;
    std::uint64_t              render_snapshot_rows_owned               = 0U;
    std::uint64_t              render_snapshot_cells_scanned            = 0U;
    std::uint64_t              render_snapshot_cells_emitted            = 0U;
    std::uint64_t              render_snapshot_dirty_rows_requested     = 0U;
    std::uint64_t              render_snapshot_dirty_rows_visible       = 0U;
    std::uint64_t              render_snapshot_full_repaint_fallbacks   = 0U;
    std::uint64_t              render_snapshot_viewport_fallbacks       = 0U;
    std::uint64_t              render_snapshot_zero_dirty_publications  = 0U;
    std::uint64_t              max_render_snapshot_rows_visited         = 0U;
    std::uint64_t              max_render_snapshot_cells_emitted        = 0U;
};

class Terminal_byte_stream_parser
{
public:
    std::vector<Parser_action> ingest(QByteArrayView bytes);

private:
    enum class String_state_result
    {
        NOT_STRING,
        CONSUMED,
    };

    std::vector<Parser_action> ingest_buffer(
        QByteArrayView                 bytes);

    String_state_result try_start_string(
        QByteArrayView                 bytes,
        qsizetype&                     offset,
        std::vector<Parser_action>&    actions);

    String_state_result try_consume_escape_or_csi(
        QByteArrayView                 bytes,
        qsizetype&                     offset,
        std::vector<Parser_action>&    actions);

    void continue_string(
        QByteArrayView                 bytes,
        qsizetype&                     offset,
        std::vector<Parser_action>&    actions);

    void start_string(
        Parser_sequence_family         family,
        QByteArrayView                 bytes,
        qsizetype                      payload_begin,
        qsizetype&                     offset,
        std::vector<Parser_action>&    actions);

    qsizetype find_string_terminator(
        QByteArrayView                 bytes,
        Parser_sequence_family         family,
        qsizetype                      payload_begin,
        Parser_string_terminator&      terminator);

    bool append_string_payload(
        Parser_sequence_family         family,
        QByteArrayView                 payload,
        std::vector<Parser_action>&    actions);

    void finish_string(
        Parser_sequence_family         family,
        Parser_string_terminator       terminator,
        std::vector<Parser_action>&    actions);

    void finish_csi_sequence(
        QByteArrayView                 bytes,
        qsizetype                      csi_begin,
        qsizetype                      final_offset,
        std::vector<Parser_action>&    actions);

    void handle_osc_payload(
        QByteArray                     payload,
        std::vector<Parser_action>&    actions);

    bool should_buffer_incomplete_utf8(
        QByteArrayView                 bytes,
        qsizetype                      offset) const;

    void emit_unsupported_control(
        QString                        source_sequence,
        Parser_sequence_family         family,
        std::vector<Parser_action>&    actions);

    void continue_discarded_csi(
        QByteArrayView                 bytes,
        qsizetype&                     offset);

    QByteArray                 m_pending_prefix;
    QByteArray                 m_string_payload;
    Parser_sequence_family     m_string_family                 = Parser_sequence_family::NONE;
    bool                       m_string_over_limit             = false;
    Terminal_utf8_scan_state   m_string_utf8_scan_state;
    bool                       m_discarding_csi                = false;
    std::uint64_t              m_next_host_request_id          = 1U;
};

class Terminal_screen_model
{
public:
    explicit Terminal_screen_model(Terminal_screen_model_config config);

    Terminal_screen_model_result ingest(QByteArrayView bytes);
    Terminal_screen_model_result resize(terminal_grid_size_t grid_size);
    Terminal_screen_model_result set_scrollback_limit(int limit);
    void set_primary_repaint_recovery_enabled(bool enabled);
    Terminal_screen_model_result force_release_synchronized_output();

    Terminal_render_snapshot render_snapshot(
        std::uint64_t                  sequence) const;

    Terminal_render_snapshot render_snapshot(
        const Terminal_render_snapshot_request&        request) const;

    Terminal_selection_result selected_text(
        const Terminal_selection_range&                selection) const;

    Terminal_selection_result selected_text(
        Terminal_buffer_id                             buffer_id,
        const Terminal_selection_range&                selection,
        std::span<const terminal_selection_line_lease_t>
                                                       descriptors) const;

    std::vector<terminal_selection_line_lease_t> selection_line_leases(
        Terminal_buffer_id                             buffer_id,
        const Terminal_selection_range&                range) const;

    QString visible_text() const;
    QString row_text(int row) const;
    void apply_action(const Parser_action& action);

    terminal_grid_position_t cursor_position() const;
    terminal_grid_size_t grid_size() const;
    Terminal_buffer_id active_buffer_id() const;
    const Terminal_mode_state& mode_state() const;
    Terminal_input_mode_state input_mode_state() const;
    const QString& title() const;
    const QString& icon_name() const;
    int scrollback_size() const;
    bool retained_line_descriptors_match(
        Terminal_buffer_id             buffer_id,
        const Terminal_selection_range& range,
        std::span<const terminal_selection_line_lease_t>
                                       descriptors) const;
    Terminal_retained_line_lookup_result retained_line_lookup(
        Terminal_buffer_id             buffer_id,
        terminal_history_handle_t      history_handle) const;
    std::optional<terminal_history_handle_t> retained_history_handle_at_logical_row(
        Terminal_buffer_id             buffer_id,
        int                            logical_row) const;
    void discard_retained_lookup_cache_for_testing() const;
    Terminal_retained_line_provenance retained_line_provenance_for_testing(
        Terminal_buffer_id             buffer_id,
        int                            logical_row) const;
    std::optional<terminal_retained_row_record_metadata_t>
        retained_row_record_metadata_for_testing(
            Terminal_buffer_id         buffer_id,
            int                        logical_row) const;
    void set_dirty_row_stats_enabled(bool enabled);
    Terminal_screen_model_dirty_row_stats dirty_row_stats() const;
    Terminal_screen_model_dirty_row_timeline dirty_row_timeline() const;
    void set_profile_stats_enabled(bool enabled);
    Terminal_screen_model_profile_stats profile_stats() const;

private:
    struct Cell
    {
        QString                        text              = QStringLiteral(" ");
        int                            display_width     = 1;
        bool                           wide_continuation = false;
        bool                           occupied          = false;
        Terminal_style_id              style_id          = k_default_terminal_style_id;
        std::uint64_t                  hyperlink_id      = 0U;
    };

    struct Terminal_screen_row
    {
        std::vector<Cell>                  cells;
        Terminal_retained_line_provenance  retained_line_provenance;
    };

    struct retained_row_record_t
    {
        Terminal_screen_row                         row;
        std::map<std::uint64_t, QByteArray>          hyperlink_identity_keys;
        terminal_retained_row_record_metadata_t      metadata;
    };

    struct retained_history_append_result_t
    {
        terminal_history_handle_t       history_handle;
        int                             evicted_rows = 0;
    };

    struct active_grid_row_t
    {
        int value = 0;
    };

    struct primary_backing_row_t
    {
        int value = 0;
    };

    struct viewport_row_t
    {
        int value = 0;
    };

    struct Viewport_row_cells
    {
        const std::vector<Cell>*                  borrowed_cells = nullptr;
        std::vector<Cell>                         owned_cells;
        std::optional<std::map<std::uint64_t, QByteArray>>
                                                owned_hyperlink_identity_keys;

        const std::vector<Cell>& cells() const
        {
            return borrowed_cells != nullptr ? *borrowed_cells : owned_cells;
        }

        const std::map<std::uint64_t, QByteArray>* hyperlink_identity_keys() const
        {
            return owned_hyperlink_identity_keys.has_value()
                ? &*owned_hyperlink_identity_keys
                : nullptr;
        }

        bool materialized() const { return borrowed_cells == nullptr; }
    };

    struct retained_lookup_cache_entry_t
    {
        int                       logical_row = 0;
        terminal_history_handle_t history_handle;
        int                       row_sequence_match_count = 0;
    };

    struct retained_lookup_cache_t
    {
        bool                      valid = false;
        std::map<std::uint64_t, retained_lookup_cache_entry_t>
                                  by_row_sequence;
        std::vector<terminal_history_handle_t>
                                  by_logical_row;

        bool invalidated() const
        {
            return
                !valid                 &&
                by_row_sequence.empty() &&
                by_logical_row.empty();
        }
    };

    struct Retained_history_storage
    {
        Retained_history_storage();
        ~Retained_history_storage();

        Retained_history_storage(const Retained_history_storage& other);
        Retained_history_storage& operator=(const Retained_history_storage& other);

        void reset();

        std::unique_ptr<Terminal_history_ring>
                                  ring;
        std::unique_ptr<Terminal_history_row_traversal>
                                  traversal;
        mutable std::vector<terminal_history_handle_t>
                                  logical_rows;
        mutable terminal_history_handle_t
                                  latest_history_handle;
    };

    struct snapshot_row_t
    {
        int value = 0;
    };

    struct saved_cursor_state_t
    {
        terminal_grid_position_t       position;
        Terminal_text_style            style;
        Terminal_style_id              style_id     = k_default_terminal_style_id;
        bool                           pending_wrap = false;
        bool                           origin_mode  = false;
        bool                           valid        = false;
    };

    struct screen_buffer_state_t
    {
        std::vector<Terminal_screen_row> rows;
        saved_cursor_state_t           saved_cursor;
        terminal_grid_position_t       cursor;
        int                            scroll_top     = 0;
        int                            scroll_bottom  = 0;
        bool                           origin_mode    = false;
        bool                           pending_wrap   = false;
    };

    struct Primary_backing_buffer
    {
        Primary_backing_buffer();
        ~Primary_backing_buffer();

        Primary_backing_buffer(const Primary_backing_buffer& other);
        Primary_backing_buffer& operator=(const Primary_backing_buffer& other);

        screen_buffer_state_t& active_grid_state();
        const screen_buffer_state_t& active_grid_state() const;
        bool retained_history_empty() const;
        int retained_history_size() const;
        std::optional<retained_row_record_t> materialize_retained_history_record(
            std::size_t index) const;
        std::optional<terminal_history_handle_t> retained_history_handle(
            std::size_t index) const;

        retained_history_append_result_t append_retained_history_record(
            retained_row_record_t row);
        int discard_oldest_retained_history_records(int row_count);
        void clear_retained_history();
        void rebuild_retained_history_rows() const;
        int prune_retained_history_rows_outside_live_window() const;

        screen_buffer_state_t          active_grid;
        Retained_history_storage
                                       retained_history;
    };

    struct Alternate_active_grid
    {
        screen_buffer_state_t& active_grid_state();
        const screen_buffer_state_t& active_grid_state() const;

        screen_buffer_state_t          active_grid;
    };

    struct ingest_publication_t
    {
        std::set<int>                  dirty_rows;
        bool                           dirty_rows_have_stable_mutation_identity = true;
        bool                           terminal_content_changed     = false;
        bool                           active_buffer_changed        = false;
        bool                           grid_reflow_changed          = false;
        bool                           viewport_changed              = false;
        bool                           mode_state_changed            = false;
        bool                           mouse_reporting_mode_changed  = false;
        bool                           alternate_scroll_mode_changed = false;
    };

    struct primary_repaint_recovery_candidate_t
    {
        std::vector<Terminal_screen_row> rows;
        std::map<std::uint64_t, QByteArray>
                                     hyperlink_identity_keys;
        int                              scrollback_rows                 = 0;
        int                              unmatched_finish_budget         = 0;
        int                              pending_non_home_addressed_row  = -1;
        bool                             line_start_clear_before_text    = false;
        bool                             explicit_non_home_repaint_address = false;
        bool                             visible_row_identity_ambiguous = false;
        bool                             active                          = false;
    };

    struct primary_repaint_recovery_proposal_t
    {
        std::vector<Terminal_screen_row> rows;
        std::map<std::uint64_t, QByteArray>
                                     hyperlink_identity_keys;
        terminal_recovery_proposal_t     metadata;
    };

    screen_buffer_state_t make_empty_buffer_state();
    screen_buffer_state_t capture_current_buffer_state() const;
    void restore_buffer_state(const screen_buffer_state_t& state);
    void save_active_buffer_state();
    screen_buffer_state_t& active_buffer_state();
    const screen_buffer_state_t& active_buffer_state() const;
    std::vector<Terminal_screen_row>& active_grid_rows();
    const std::vector<Terminal_screen_row>& active_grid_rows() const;
    void resize_buffer_state(screen_buffer_state_t& state, terminal_grid_size_t grid_size);
    void resize_rows(std::vector<Terminal_screen_row>& rows, terminal_grid_size_t grid_size);
    std::uint64_t next_retained_line_id();
    void replace_retained_line_id(
        Terminal_screen_row&                    row,
        Terminal_retained_line_provenance_source source =
            Terminal_retained_line_provenance_source::TERMINAL_STORAGE);
    void replace_visible_retained_line_ids();
    void replace_row_with_erased_retained_line(Terminal_screen_row& row);

    bool cells_have_same_selection_content(
        const Cell&                    left,
        const Cell&                    right) const;

    bool rows_have_same_selection_content(
        const std::vector<Cell>&       left,
        const std::vector<Cell>&       right) const;

    void advance_row_content_generation_if_changed(
        Terminal_screen_row&           row,
        const std::vector<Cell>&       before_cells);

    void advance_row_content_generation_with_change_flag(
        Terminal_screen_row&           row,
        bool                           selection_content_changed);

    bool printable_ascii_cell_changes_selection_content(
        const Terminal_screen_row&     row,
        int                            column,
        QChar                          text) const;

    bool printable_ascii_span_changes_selection_content(
        const Terminal_screen_row&     row,
        int                            first_column,
        QStringView                    text) const;

    bool scalar_span_changes_selection_content(
        const Terminal_screen_row&     row,
        terminal_grid_position_t       position,
        QStringView                    text,
        int                            display_width) const;

    bool scalar_span_clear_changes_selection_content(
        const Terminal_screen_row&     row,
        terminal_grid_position_t       position) const;

    std::vector<bool> default_tab_stops(int column_count) const;
    void reset_grid();

    void apply_action(
        const Parser_action&           action,
        std::vector<Parser_action>&    generated_actions,
        ingest_publication_t*          publication);

    void apply_control_sequence(
        const Parser_control_sequence& sequence,
        std::vector<Parser_action>&    generated_actions,
        ingest_publication_t*          publication);

    void apply_sgr_sequence(
        const Terminal_sgr_sequence&   sequence);

    void apply_sgr_operation(
        const Terminal_sgr_operation&  operation);

    Terminal_style_id intern_style(
        const Terminal_text_style&     style);

    Parser_action make_color_query_reply(
        const Terminal_color_query&    query) const;

    Parser_action make_unsupported_control_diagnostic(
        const Parser_control_sequence& sequence) const;

    Parser_action make_private_mode_diagnostic(
        int                            mode,
        const Parser_control_sequence& sequence) const;

    bool apply_grid_resize(
        terminal_grid_size_t           grid_size,
        bool                           guard_scrollback_clear);

    void reset_scroll_region();
    void reset_tab_stops();

    void put_scalar(
        QString                        text);

    void put_text(
        QString                        text);

    void put_printable_ascii_text(
        QStringView                    text);

    void write_printable_ascii_span(
        int                            row,
        int                            first_column,
        QStringView                    text);

    void write_printable_ascii_span_content(
        Terminal_screen_row&           row,
        int                            first_column,
        QStringView                    text);

    void write_printable_ascii_cell_content(
        Terminal_screen_row&           row,
        int                            column,
        QChar                          text);

    void put_spacing_scalar(
        QString                        text,
        int                            display_width);

    void append_zero_width_scalar(
        QString                        text);

    void install_cell_span(
        terminal_grid_position_t       position,
        QString                        text,
        int                            display_width,
        Terminal_style_id              style_id,
        std::uint64_t                  hyperlink_id);

    void place_cell_text(
        terminal_grid_position_t       position,
        QString                        text,
        int                            display_width);

    void clear_cell_span(
        terminal_grid_position_t       position);

    void clear_cell_at(
        terminal_grid_position_t       position);

    int cell_base_column_in_row(
        const Terminal_screen_row&     row,
        int                            column) const;

    void clear_cell_span_content(
        Terminal_screen_row&           row,
        int                            column);

    void clear_cell_at_content(
        Terminal_screen_row&           row,
        int                            column);

    Cell erased_cell() const;
    void fill_row_with_erased_cells(std::vector<Cell>& row) const;
    void erase_cell_at(terminal_grid_position_t position);
    void erase_row_range(int row, int first_column, int last_column);
    void clear_screen_before_cursor();
    void clear_screen_after_cursor();
    void erase_visible_screen();

    void erase_in_display(
        int                            mode);

    void erase_in_line(
        int                            mode);

    void erase_characters(
        int                            count);

    void insert_cells(
        int                            count);

    void delete_cells(
        int                            count);

    void insert_lines(
        int                            count);

    void delete_lines(
        int                            count);

    terminal_grid_position_t cell_base_position(
        terminal_grid_position_t       position) const;

    void set_cursor_after_cell(
        terminal_grid_position_t       position,
        int                            display_width);

    void set_cursor_position(
        int                            row,
        int                            column);

    void set_cursor_address(
        int                            row_parameter,
        int                            column_parameter);

    void move_cursor_relative(
        int                            row_delta,
        int                            column_delta);

    void set_scroll_region(
        int                            top_parameter,
        int                            bottom_parameter);

    void set_origin_mode(
        bool                           enabled);

    void set_autowrap_mode(
        bool                           enabled);

    void set_application_keypad_mode(
        bool                           enabled);

    void set_hyperlink(
        QByteArray                     identity_key);

    void set_synchronized_output_mode(
        bool                           enabled,
        ingest_publication_t*          publication);

    void apply_dec_private_mode(
        int                            mode,
        bool                           enabled,
        std::vector<Parser_action>&    generated_actions,
        const Parser_control_sequence& sequence,
        ingest_publication_t*          publication);

    void apply_mouse_tracking_mode(
        Terminal_mouse_tracking_mode   target,
        bool                           enabled);

    int dec_private_mode_status(
        int                            mode) const;

    void enter_alternate_screen(
        bool                           clear_alternate,
        int                            active_mode);

    bool leave_alternate_screen(
        bool                           clear_alternate);

    void save_cursor();
    void restore_cursor();
    void clear_current_tab_stop();
    void clear_all_tab_stops();
    void append_scrollback_row(
        const Terminal_screen_row&     row,
        Terminal_retained_line_provenance_source source =
            Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
        const std::map<std::uint64_t, QByteArray>* hyperlink_identity_keys =
            nullptr);
    void scroll_up_region(int top, int bottom, bool append_scrollback, int count = 1);
    void scroll_down_region(int top, int bottom, int count = 1);
    void reverse_index();
    void arm_resize_repaint_clear_guard();
    void cancel_resize_repaint_clear_guard();
    void cancel_resize_repaint_clear_guard_before_visible_clear();
    void advance_resize_repaint_clear_guard();
    void note_resize_repaint_visible_clear();
    bool consume_resize_repaint_scrollback_clear_guard();
    void arm_primary_repaint_recovery_resize_guard();
    void cancel_primary_repaint_recovery_resize_guard();
    void advance_primary_repaint_recovery_resize_guard();
    void begin_primary_repaint_recovery_candidate();
    void finish_primary_repaint_recovery_candidate(bool discard_if_no_match);
    void cancel_primary_repaint_recovery_candidate();
    void accept_primary_repaint_recovery_proposal(
        const primary_repaint_recovery_proposal_t&     proposal);

    std::optional<primary_repaint_recovery_proposal_t>
        primary_repaint_recovery_proposal(
            const primary_repaint_recovery_candidate_t&    candidate) const;

    int primary_repaint_recovery_shift_rows(
        const primary_repaint_recovery_candidate_t&    candidate) const;

    bool row_has_visible_text(
        const Terminal_screen_row&     row) const;

    void carriage_return();
    void line_feed();
    void backspace();
    void horizontal_tab();
    void mark_cursor_dirty();
    void mark_dirty(int row);
    void mark_terminal_content_changed();
    void mark_active_buffer_changed();
    void mark_grid_reflow_changed();
    void mark_viewport_changed();
    void mark_mode_state_changed();
    void mark_mouse_reporting_mode_changed();
    void mark_alternate_scroll_mode_changed();
    void mark_all_dirty();
    void repair_wide_spans_in_row(std::vector<Cell>& row, int column_count) const;
    void clear_dirty();
    void clear_backing_deltas();
    void clear_recovery_proposals();
    int compatibility_evicted_scrollback_rows() const;
    void record_backing_delta(terminal_backing_delta_t delta);
    void record_active_grid_delta(
        Terminal_backing_delta_kind    kind,
        terminal_grid_size_t           grid_size_before,
        terminal_grid_size_t           grid_size_after);
    void record_mode_transition_delta(
        Terminal_buffer_id             active_buffer_before,
        Terminal_buffer_id             active_buffer_after);
    void record_primary_history_delta(
        Terminal_backing_delta_kind    kind,
        int                            scrollback_rows_before,
        int                            scrollback_rows_after,
        int                            appended_scrollback_rows,
        int                            evicted_scrollback_rows,
        int                            discarded_scrollback_rows);
    void evict_oldest_scrollback_rows(int row_count);
    Terminal_screen_model_dirty_row_bucket_stats& dirty_row_stats_bucket() const;
    void update_pending_dirty_row_stats_watermark();
    void update_synchronized_dirty_row_stats_watermark();
    std::vector<int> dirty_rows() const;
    void collect_synchronized_changes();
    void publish_pending_changes(ingest_publication_t& publication);
    void release_synchronized_changes(ingest_publication_t& publication);
    std::uint64_t next_hyperlink_id();
    std::uint64_t active_hyperlink_id_for_identity(const QByteArray& identity_key);
    const QByteArray* active_hyperlink_identity_key(std::uint64_t hyperlink_id) const;
    void retain_referenced_active_hyperlink_ids();

    retained_row_record_t seal_retained_row_record(
        const Terminal_screen_row&     screen_row,
        Terminal_retained_line_provenance_source source,
        const std::map<std::uint64_t, QByteArray>* hyperlink_identity_keys);

    static Terminal_history_row_record history_row_record_from_retained_record(
        const retained_row_record_t&   retained_record);

    static retained_row_record_t retained_row_record_from_history_row_record(
        const Terminal_history_row_record& history_record);

    void materialize_retained_row_hyperlinks(
        retained_row_record_t&         row,
        const std::map<std::uint64_t, QByteArray>* preserved_identity_keys =
            nullptr) const;

    int active_grid_row_count() const;
    int primary_backing_row_count() const;
    int primary_backing_active_grid_first_row() const;
    bool active_grid_row_is_valid(active_grid_row_t row) const;
    bool primary_backing_row_is_valid(primary_backing_row_t row) const;
    bool viewport_row_is_valid(viewport_row_t row) const;
    const std::vector<Terminal_screen_row>& primary_active_grid_rows() const;
    const std::vector<Terminal_screen_row>& alternate_active_grid_rows() const;
    primary_backing_row_t primary_backing_row_from_active(active_grid_row_t row) const;
    std::optional<active_grid_row_t> active_grid_row_from_primary_backing(
        primary_backing_row_t          row) const;
    std::optional<primary_backing_row_t> primary_backing_row_from_viewport(
        const Terminal_viewport_state& viewport,
        viewport_row_t                 row) const;
    std::optional<viewport_row_t> viewport_row_from_primary_backing(
        const Terminal_viewport_state& viewport,
        primary_backing_row_t          row) const;
    viewport_row_t viewport_row_from_primary_backing_unbounded(
        const Terminal_viewport_state& viewport,
        primary_backing_row_t          row) const;
    snapshot_row_t snapshot_row_from_viewport(viewport_row_t row) const;
    std::optional<Terminal_screen_row> primary_backing_row(
        primary_backing_row_t          row) const;
    const Terminal_screen_row* alternate_active_row(
        active_grid_row_t              row) const;

    std::vector<int> viewport_dirty_rows(
        const Terminal_viewport_state& viewport,
        const std::vector<int>&        dirty_rows) const;

    std::vector<Cell> visual_row_projection_for_current_geometry(
        const std::vector<Cell>&       row) const;

    const std::vector<Cell>& row_cells_for_current_geometry(
        const std::vector<Cell>&       row,
        std::vector<Cell>&             visual_projection) const;

    void append_snapshot_cells_from_row(
        Terminal_render_snapshot&      snapshot,
        const std::vector<Cell>&       row,
        int                            snapshot_row,
        std::vector<std::uint64_t>&    row_referenced_hyperlink_ids,
        bool                           collect_hyperlink_ids) const;

    QString row_text_from_cells(
        const std::vector<Cell>&       row,
        int                            first_column,
        int                            end_column) const;

    std::optional<std::vector<Cell>> logical_row_cells(
        Terminal_buffer_id             buffer_id,
        int                            logical_row) const;

    std::optional<Viewport_row_cells> viewport_row_cells(
        const Terminal_viewport_state& viewport,
        int                            viewport_row) const;

    std::optional<Terminal_retained_line_provenance> viewport_row_provenance(
        const Terminal_viewport_state& viewport,
        int                            viewport_row) const;

    std::optional<std::map<std::uint64_t, QByteArray>>
        viewport_row_retained_hyperlink_identity_keys(
            const Terminal_viewport_state& viewport,
            int                            viewport_row) const;

    bool retained_line_descriptor_logical_row(
        Terminal_buffer_id             buffer_id,
        terminal_selection_line_lease_t descriptor,
        int&                           logical_row) const;

    void invalidate_retained_lookup_caches() const;
    void rebuild_retained_lookup_cache(
        Terminal_buffer_id             buffer_id) const;
    const retained_lookup_cache_t& retained_lookup_cache(
        Terminal_buffer_id             buffer_id) const;
    retained_lookup_cache_t& mutable_retained_lookup_cache(
        Terminal_buffer_id             buffer_id) const;
    std::optional<terminal_history_handle_t> retained_lookup_cache_live_handle(
        Terminal_buffer_id             buffer_id,
        int                            logical_row) const;
    Terminal_history_resolution_status retained_lookup_cache_entry_status(
        Terminal_buffer_id             buffer_id,
        const retained_lookup_cache_entry_t& entry) const;
    std::optional<terminal_history_handle_t> retained_lookup_cache_handle_at_logical_row(
        Terminal_buffer_id             buffer_id,
        int                            logical_row) const;

    bool selection_line_lease_logical_rows(
        Terminal_buffer_id             buffer_id,
        const Terminal_selection_range& range,
        std::span<const terminal_selection_line_lease_t>
                                       descriptors,
        std::vector<int>&              logical_rows) const;

    Terminal_selection_result selected_text_from_logical_rows(
        Terminal_buffer_id             buffer_id,
        const Terminal_selection_range& selection,
        std::span<const int>           logical_rows) const;

    bool render_selection_request_logical_rows(
        const Terminal_render_selection_request& request,
        Terminal_buffer_id             buffer_id,
        std::vector<int>&              logical_rows) const;

    void append_hyperlink_metadata_for_cells(
        std::vector<Terminal_render_hyperlink_metadata>& metadata,
        const std::vector<Terminal_render_cell>&         cells,
        std::size_t                                      first_cell,
        const std::map<std::uint64_t, QByteArray>*       row_local_identity_keys)
        const;

    void append_hyperlink_metadata_for_ids(
        std::vector<Terminal_render_hyperlink_metadata>& metadata,
        std::span<const std::uint64_t>                   hyperlink_ids,
        const std::map<std::uint64_t, QByteArray>*       row_local_identity_keys)
        const;

    Terminal_screen_model_config    m_config;
    Terminal_byte_stream_parser     m_parser;
    Terminal_color_state            m_color_state;
    Terminal_text_style             m_current_style = make_default_terminal_text_style();
    Terminal_style_id               m_current_style_id = k_default_terminal_style_id;
    std::vector<Terminal_text_style>
                                    m_styles;
    Primary_backing_buffer          m_primary_backing;
    Alternate_active_grid           m_alternate_grid;
    std::set<int>                   m_dirty_rows;
    std::vector<terminal_backing_delta_t>
                                    m_backing_deltas;
    std::vector<terminal_recovery_proposal_t>
                                    m_recovery_proposals;
    int                             m_last_dirty_row = -1;
    std::vector<bool>               m_tab_stops;
    saved_cursor_state_t            m_saved_cursor;
    terminal_grid_position_t        m_cursor;
    QString                         m_title;
    QString                         m_icon_name;
    Terminal_buffer_id              m_active_buffer_id = Terminal_buffer_id::PRIMARY;
    Terminal_mode_state             m_modes;
    // DECPAM/DECPNM/DECNKM affect input encoding only, so they stay outside
    // the render snapshot mode state and do not invalidate render output.
    bool                            m_application_keypad = false;
    int                             m_active_alternate_mode = 0;
    std::uint64_t                   m_current_hyperlink_id = 0U;
    std::uint64_t                   m_next_hyperlink_id = 1U;
    std::uint64_t                   m_next_retained_line_id = 1U;
    mutable retained_lookup_cache_t m_primary_retained_lookup_cache;
    mutable retained_lookup_cache_t m_alternate_retained_lookup_cache;
    std::map<QByteArray, std::uint64_t>
                                     m_active_hyperlink_ids;
    int                             m_scroll_top = 0;
    int                             m_scroll_bottom = 0;
    bool                            m_origin_mode = false;
    bool                            m_dec_1049_saved_primary_cursor = false;
    bool                            m_pending_wrap = false;
    bool                            m_viewport_changed = false;
    bool                            m_terminal_content_changed = false;
    bool                            m_active_buffer_changed = false;
    bool                            m_grid_reflow_changed = false;
    bool                            m_mode_state_changed = false;
    bool                            m_mouse_reporting_mode_changed = false;
    bool                            m_alternate_scroll_mode_changed = false;
    std::set<int>                   m_synchronized_dirty_rows;
    mutable Terminal_screen_model_dirty_row_stats
                                    m_dirty_row_stats;
    mutable Terminal_screen_model_dirty_row_timeline
                                    m_dirty_row_timeline;
    mutable std::chrono::steady_clock::time_point
                                    m_dirty_row_stats_start_time;
    mutable Terminal_screen_model_profile_stats
                                    m_profile_stats;
    int                             m_printable_text_profile_depth = 0;
    bool                            m_synchronized_viewport_changed = false;
    bool                            m_synchronized_terminal_content_changed = false;
    bool                            m_synchronized_active_buffer_changed = false;
    bool                            m_synchronized_grid_reflow_changed = false;
    bool                            m_synchronized_mode_state_changed = false;
    bool                            m_synchronized_mouse_reporting_mode_changed = false;
    bool                            m_synchronized_alternate_scroll_mode_changed = false;
    int                             m_scrollback_evicted_rows = 0;
    int                             m_resize_repaint_clear_guard_remaining = 0;
    bool                            m_resize_repaint_clear_guard_saw_visible_clear = false;
    int                             m_primary_repaint_recovery_resize_guard_remaining = 0;
    primary_repaint_recovery_candidate_t
                                    m_primary_repaint_recovery_candidate;
};

}
