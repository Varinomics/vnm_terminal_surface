#include "vnm_terminal/internal/terminal_repaint_recovery.h"

#include <algorithm>

namespace vnm_terminal::internal {

namespace {

bool row_has_visible_text(const QString& row)
{
    return !row.isEmpty();
}

}

int primary_repaint_recovery_shift_rows(
    const terminal_repaint_recovery_shift_input_t& input)
{
    if (!input.candidate_active              ||
        !input.primary_buffer_active         ||
        !input.scrollback_rows_unchanged     ||
        input.candidate_rows.size() != input.current_rows.size())
    {
        return 0;
    }

    if (input.line_start_clear_before_text &&
        input.explicit_non_home_repaint_address)
    {
        return 0;
    }

    constexpr int k_min_meaningful_matches       = 2;
    constexpr int k_preferred_meaningful_matches = 3;

    const int row_count = static_cast<int>(input.candidate_rows.size());
    int best_shift              = 0;
    int best_meaningful_matches = 0;
    int best_matched_prefix     = 0;

    for (int shift = 1; shift < row_count; ++shift) {
        bool displaced_has_text = false;
        for (int row = 0; row < shift; ++row) {
            displaced_has_text = displaced_has_text ||
                row_has_visible_text(input.candidate_rows[static_cast<std::size_t>(row)]);
        }

        int available_meaningful_matches = 0;
        for (int row = 0; row + shift < row_count; ++row) {
            if (row_has_visible_text(
                    input.candidate_rows[static_cast<std::size_t>(row + shift)]))
            {
                ++available_meaningful_matches;
            }
        }
        const int required_meaningful_matches = std::min(
            k_preferred_meaningful_matches,
            available_meaningful_matches);
        if (required_meaningful_matches < k_min_meaningful_matches) {
            continue;
        }

        int                  matched_prefix     = 0;
        int                  meaningful_matches = 0;
        std::vector<QString> distinct_matched_texts;
        while (matched_prefix + shift < row_count) {
            const QString& current_text =
                input.current_rows[static_cast<std::size_t>(matched_prefix)];
            const QString& candidate_text =
                input.candidate_rows[static_cast<std::size_t>(matched_prefix + shift)];
            if (current_text != candidate_text) {
                break;
            }

            if (!candidate_text.isEmpty()) {
                ++meaningful_matches;
                if (std::find(
                        distinct_matched_texts.begin(),
                        distinct_matched_texts.end(),
                        candidate_text) == distinct_matched_texts.end())
                {
                    distinct_matched_texts.push_back(candidate_text);
                }
            }
            ++matched_prefix;
        }

        if (meaningful_matches < required_meaningful_matches ||
            static_cast<int>(distinct_matched_texts.size()) < k_min_meaningful_matches)
        {
            continue;
        }

        // The rows that prove an upward shift are the surviving rows that
        // matched after the shift, not the displaced payload itself, so a blank
        // displaced row is still recoverable - dropping it is what makes blank
        // separator lines vanish from reconstructed scrollback. A blank payload
        // is weaker evidence than a text-bearing one, so recover it only when
        // the entire surviving suffix matched; a partial prefix match is enough
        // only when the displaced payload carries its own text.
        if (!displaced_has_text && matched_prefix != row_count - shift) {
            continue;
        }

        if (meaningful_matches > best_meaningful_matches ||
            (meaningful_matches == best_meaningful_matches &&
                matched_prefix > best_matched_prefix))
        {
            best_meaningful_matches = meaningful_matches;
            best_matched_prefix     = matched_prefix;
            best_shift              = shift;
        }
    }

    return best_shift;
}

} // namespace vnm_terminal::internal
