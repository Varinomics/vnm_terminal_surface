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
};

int primary_repaint_recovery_shift_rows(
    const terminal_repaint_recovery_shift_input_t& input);

} // namespace vnm_terminal::internal
