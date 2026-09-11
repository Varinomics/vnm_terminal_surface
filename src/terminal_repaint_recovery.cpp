#include "vnm_terminal/internal/terminal_repaint_recovery.h"

#include <algorithm>
#include <array>

namespace vnm_terminal::internal {

namespace {

bool row_has_visible_text(const QString& row)
{
    return !row.isEmpty();
}

// All callers only distinguish fewer than two, two, and at least three
// distinct texts. Saturate at three without allocating or copying QStrings.
// The input rows stay alive and immutable for the duration of the match.
struct distinct_row_texts_t
{
    std::array<const QString*, 3> texts = {};
    std::size_t                   count = 0U;

    void insert(const QString& text)
    {
        if (count == texts.size()) {
            return;
        }
        for (std::size_t index = 0U; index < count; ++index) {
            if (*texts[index] == text) {
                return;
            }
        }
        texts[count++] = &text;
    }

    std::size_t size() const
    {
        return count;
    }
};

}

terminal_repaint_recovery_shift_result_t primary_repaint_recovery_shift_result(
    const terminal_repaint_recovery_shift_input_t& input)
{
    terminal_repaint_recovery_shift_result_t result;
    if (!input.candidate_active              ||
        !input.primary_buffer_active         ||
        !input.scrollback_rows_unchanged     ||
        input.candidate_rows.size() != input.current_rows.size())
    {
        return result;
    }

    if (input.line_start_clear_before_text &&
        input.explicit_non_home_repaint_address)
    {
        return result;
    }

    constexpr int k_min_meaningful_matches                 = 2;
    constexpr int k_preferred_meaningful_matches           = 3;
    constexpr int k_min_partial_blank_rewritten_tail_rows  = 2;
    constexpr int k_min_anchored_meaningful_matches        = 3;

    const int row_count = static_cast<int>(input.candidate_rows.size());
    int best_shift              = 0;
    int best_meaningful_matches = 0;
    int best_matched_prefix     = 0;
    bool best_full_match         = false;
    bool repeated_row_ambiguity  = false;

    int available_meaningful_matches = static_cast<int>(std::count_if(
        input.candidate_rows.begin(),
        input.candidate_rows.end(),
        row_has_visible_text));
    bool displaced_has_text = false;
    for (int shift = 1; shift < row_count; ++shift) {
        const bool newly_displaced_has_text = row_has_visible_text(
            input.candidate_rows[static_cast<std::size_t>(shift - 1)]);
        displaced_has_text = displaced_has_text || newly_displaced_has_text;
        available_meaningful_matches -= newly_displaced_has_text ? 1 : 0;
        const int required_meaningful_matches = std::min(
            k_preferred_meaningful_matches,
            available_meaningful_matches);
        if (required_meaningful_matches < k_min_meaningful_matches) {
            continue;
        }

        int                  matched_prefix     = 0;
        int                  meaningful_matches = 0;
        distinct_row_texts_t distinct_matched_texts;
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
                distinct_matched_texts.insert(candidate_text);
            }
            ++matched_prefix;
        }

        if (meaningful_matches >= k_min_meaningful_matches &&
            static_cast<int>(distinct_matched_texts.size()) < k_min_meaningful_matches)
        {
            repeated_row_ambiguity = true;
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
        // is weaker evidence than a text-bearing one, so accept either a full
        // surviving-suffix match or a preferred-strength partial match. The
        // latter is still limited to a bounded rewritten tail so broad layout
        // repaints, such as a startup banner moving upward, do not manufacture
        // scrollback from a top spacer.
        const bool full_surviving_suffix_matched =
            matched_prefix == row_count - shift;
        const int unmatched_tail_rows = row_count - shift - matched_prefix;
        const int max_partial_blank_rewritten_tail_rows =
            std::max(k_min_partial_blank_rewritten_tail_rows, row_count / 4);
        const bool strong_partial_blank_shift =
            meaningful_matches >= k_preferred_meaningful_matches &&
            static_cast<int>(distinct_matched_texts.size()) >=
                k_preferred_meaningful_matches &&
            unmatched_tail_rows <= max_partial_blank_rewritten_tail_rows;
        if (!displaced_has_text &&
            !full_surviving_suffix_matched &&
            !strong_partial_blank_shift)
        {
            continue;
        }

        if (meaningful_matches > best_meaningful_matches ||
            (meaningful_matches == best_meaningful_matches &&
                matched_prefix > best_matched_prefix))
        {
            best_meaningful_matches = meaningful_matches;
            best_matched_prefix     = matched_prefix;
            best_shift              = shift;
            best_full_match         = full_surviving_suffix_matched;
        }
    }

    // A continuing repaint can preserve a fixed footer while shifting the
    // content above it. This is deliberately a separate, stronger exception
    // to the bounded partial-tail policy above: it proves a dense one-row body
    // shift, exactly one rewritten seam, and a distinct same-coordinate footer.
    if (best_shift == 0                         &&
        input.continuing_repaint_episode        &&
        row_count > 1                           &&
        input.candidate_rows.front().isEmpty())
    {
        int                  matched_prefix = 0;
        int                  meaningful_matches = 0;
        distinct_row_texts_t distinct_matched_texts;
        while (matched_prefix + 1 < row_count) {
            const QString& current_text =
                input.current_rows[static_cast<std::size_t>(matched_prefix)];
            const QString& candidate_text =
                input.candidate_rows[static_cast<std::size_t>(matched_prefix + 1)];
            if (current_text != candidate_text) {
                break;
            }
            if (!candidate_text.isEmpty()) {
                ++meaningful_matches;
                distinct_matched_texts.insert(candidate_text);
            }
            ++matched_prefix;
        }

        int                  anchored_suffix_start = row_count;
        int                  anchored_meaningful_matches = 0;
        distinct_row_texts_t distinct_anchored_texts;
        while (anchored_suffix_start > 0) {
            const int row = anchored_suffix_start - 1;
            const QString& current_text =
                input.current_rows[static_cast<std::size_t>(row)];
            const QString& candidate_text =
                input.candidate_rows[static_cast<std::size_t>(row)];
            if (current_text != candidate_text) {
                break;
            }
            if (!candidate_text.isEmpty()) {
                ++anchored_meaningful_matches;
                distinct_anchored_texts.insert(candidate_text);
            }
            --anchored_suffix_start;
        }

        const bool dense_shifted_prefix =
            matched_prefix * 3 >= (row_count - 1) * 2;
        const bool strong_shifted_prefix =
            meaningful_matches >= k_min_anchored_meaningful_matches &&
            static_cast<int>(distinct_matched_texts.size()) >=
                k_min_anchored_meaningful_matches;
        const bool strong_anchored_suffix =
            anchored_meaningful_matches >= k_min_anchored_meaningful_matches &&
            static_cast<int>(distinct_anchored_texts.size()) >=
                k_min_anchored_meaningful_matches;
        const bool exactly_one_seam =
            anchored_suffix_start == matched_prefix + 1;
        if (dense_shifted_prefix       &&
            strong_shifted_prefix      &&
            strong_anchored_suffix     &&
            exactly_one_seam)
        {
            result.shifted_rows        = 1;
            result.matched_prefix_rows = matched_prefix;
            result.unmatched_tail_rows = row_count - 1 - matched_prefix;
            result.anchored_suffix_rows = row_count - anchored_suffix_start;
            result.match_kind =
                Terminal_repaint_recovery_match_kind::PARTIAL_ANCHORED;
            return result;
        }
    }

    result.shifted_rows = best_shift;
    if (best_shift > 0) {
        result.matched_prefix_rows = best_matched_prefix;
        result.unmatched_tail_rows = row_count - best_shift - best_matched_prefix;
        result.match_kind = best_full_match
            ? Terminal_repaint_recovery_match_kind::FULL
            : Terminal_repaint_recovery_match_kind::PARTIAL;
    }
    else if (repeated_row_ambiguity) {
        result.rejection_kind =
            Terminal_repaint_recovery_rejection_kind::REPEATED_ROW_AMBIGUOUS;
    }
    return result;
}

int primary_repaint_recovery_shift_rows(
    const terminal_repaint_recovery_shift_input_t& input)
{
    return primary_repaint_recovery_shift_result(input).shifted_rows;
}

} // namespace vnm_terminal::internal
