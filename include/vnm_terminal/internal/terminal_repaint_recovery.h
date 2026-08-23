#pragma once

#include <QString>

#include <vector>

namespace vnm_terminal::internal {

struct terminal_repaint_recovery_shift_input_t
{
    std::vector<QString> candidate_rows;
    std::vector<QString> current_rows;
    bool                 candidate_active = false;
    bool                 primary_buffer_active = false;
    bool                 scrollback_rows_unchanged = false;
    bool                 line_start_clear_before_text = false;
    bool                 explicit_non_home_repaint_address = false;
    bool                 continuing_repaint_episode = false;
};

enum class Terminal_repaint_recovery_match_kind
{
    NONE,
    FULL,
    PARTIAL,
    PARTIAL_ANCHORED,
};

enum class Terminal_repaint_recovery_rejection_kind
{
    NONMATCHING,
    REPEATED_ROW_AMBIGUOUS,
};

struct terminal_repaint_recovery_shift_result_t
{
    int                                         shifted_rows = 0;
    int                                         matched_prefix_rows = 0;
    int                                         unmatched_tail_rows = 0;
    int                                         anchored_suffix_rows = 0;
    Terminal_repaint_recovery_match_kind        match_kind =
        Terminal_repaint_recovery_match_kind::NONE;
    Terminal_repaint_recovery_rejection_kind    rejection_kind =
        Terminal_repaint_recovery_rejection_kind::NONMATCHING;
};

terminal_repaint_recovery_shift_result_t primary_repaint_recovery_shift_result(
    const terminal_repaint_recovery_shift_input_t& input);

int primary_repaint_recovery_shift_rows(
    const terminal_repaint_recovery_shift_input_t& input);

} // namespace vnm_terminal::internal
