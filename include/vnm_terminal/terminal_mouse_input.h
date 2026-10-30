#pragma once

#include <QMetaType>
#include <QString>
#include <cstdint>
#include <optional>

namespace vnm_terminal {

enum class Terminal_mouse_buffer { PRIMARY_SCREEN, ALTERNATE_SCREEN };
enum class Terminal_tap_policy { HOST, APPLICATION, CONSUME };

// Mapping identity, independent of text changes and mouse reporting modes.
struct terminal_mouse_source_t
{
    std::uint64_t session_epoch         = 0U;
    std::uint64_t mapping_generation    = 0U;
    std::uint64_t row_origin_generation = 0U;
    int rows            = 0;
    int columns         = 0;
    int scrollback_rows = 0;
    Terminal_mouse_buffer buffer = Terminal_mouse_buffer::PRIMARY_SCREEN;
    bool operator==(const terminal_mouse_source_t&) const = default;
};

// A resolved native cell survives text output and history eviction. Origin
// fields still bind the original frame's logical-to-native mapping, but do
// not change the coordinate system of the resulting screen-relative cell.
inline bool terminal_mouse_mapping_matches(
    const terminal_mouse_source_t& left,
    const terminal_mouse_source_t& right)
{
    return left.session_epoch == right.session_epoch &&
        left.mapping_generation == right.mapping_generation &&
        left.buffer == right.buffer && left.rows == right.rows && left.columns == right.columns;
}

inline constexpr std::uint16_t k_terminal_canvas_mouse_input_version = 1U;

// Optional attachment to one immutable canvas publication. Unknown/invalid
// records disable clicking only; absence must never inherit an earlier record.
struct terminal_canvas_mouse_input_t
{
    std::uint16_t record_version = k_terminal_canvas_mouse_input_version;
    terminal_mouse_source_t source;
    bool operator==(const terminal_canvas_mouse_input_t&) const = default;
};

struct terminal_mouse_target_t
{
    terminal_mouse_source_t source;
    std::uint64_t frame_sequence         = 0U;
    std::uint64_t publication_generation = 0U;
    int display_row = 0;
    int row         = 0;
    int column      = 0;
    bool operator==(const terminal_mouse_target_t&) const = default;
};

struct terminal_tap_t
{
    Terminal_tap_policy policy = Terminal_tap_policy::HOST;
    std::optional<terminal_mouse_target_t> target;
};

struct terminal_mouse_click_request_t
{
    std::uint64_t request_id = 0U;
    terminal_mouse_target_t target;
};

enum class Terminal_mouse_click_status
{
    ACCEPTED,
    NO_REPORT,
    STALE_SOURCE,
    UNAVAILABLE,
    INVALID_REQUEST,
    REFUSED,
    INDETERMINATE,
};

struct Terminal_mouse_click_result
{
    std::uint64_t request_id = 0U;
    Terminal_mouse_click_status status = Terminal_mouse_click_status::UNAVAILABLE;
    QString reason;
};

} // namespace vnm_terminal

Q_DECLARE_METATYPE(vnm_terminal::terminal_tap_t)
Q_DECLARE_METATYPE(vnm_terminal::Terminal_tap_policy)
Q_DECLARE_METATYPE(vnm_terminal::terminal_mouse_click_request_t)
Q_DECLARE_METATYPE(vnm_terminal::Terminal_mouse_click_result)
