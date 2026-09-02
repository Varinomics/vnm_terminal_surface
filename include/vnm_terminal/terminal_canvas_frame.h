#pragma once

#include <QString>
#include <QtGlobal>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace vnm_terminal {

inline constexpr std::uint32_t k_terminal_canvas_frame_api_version = 4U;
inline constexpr std::uint16_t k_terminal_canvas_content_extent_version = 1U;
inline constexpr qreal k_terminal_canvas_max_font_pixel_size = 1'024.0;
inline constexpr std::size_t   k_terminal_canvas_max_cells         = 32'768U;
// A canvas axis is bounded by the same allocation budget as the complete
// grid. The compact worker wire uses unsigned 16-bit row/gap fields, and the
// cell budget is the tighter bound. Requiring the grid area to fit below
// prevents either axis from becoming an independent, lower presentation cap.
inline constexpr int k_terminal_canvas_wire_axis_max =
    static_cast<int>(std::numeric_limits<std::uint16_t>::max());
inline constexpr int k_terminal_canvas_axis_max =
    k_terminal_canvas_max_cells <
            static_cast<std::size_t>(k_terminal_canvas_wire_axis_max)
        ? static_cast<int>(k_terminal_canvas_max_cells)
        : k_terminal_canvas_wire_axis_max;
inline constexpr int k_terminal_canvas_max_rows = k_terminal_canvas_axis_max;
inline constexpr int k_terminal_canvas_max_columns = k_terminal_canvas_axis_max;
inline constexpr std::size_t   k_terminal_canvas_max_styles        = 256U;
inline constexpr qsizetype k_terminal_canvas_max_cell_text_utf16_code_units =
    4'096;
// The 96 KiB text budget leaves the surrounding 128 KiB wire envelope room
// for geometry, styles, cursor state, framing, and other capability metadata.
inline constexpr qsizetype k_terminal_canvas_max_frame_text_utf8_bytes =
    96 * 1'024;

inline constexpr bool terminal_canvas_grid_fits_cell_budget(
    int rows,
    int columns)
{
    return rows > 0 && columns > 0 &&
        static_cast<std::size_t>(rows) <=
            k_terminal_canvas_max_cells / static_cast<std::size_t>(columns);
}

enum class Terminal_canvas_cursor_shape
{
    BLOCK,
    BAR,
    UNDERLINE,
};

enum class Terminal_canvas_buffer : std::uint8_t
{
    PRIMARY_BUFFER,
    ALTERNATE_BUFFER,
};

// Semantic extent for one enclosing frame. Interpret fields only when
// record_version is known; an unknown version leaves only this capability
// unavailable and does not invalidate the base frame. Incoherent fields in a
// known record version invalidate the complete enclosing frame.
//
// In version 1, content_bottom_row_exclusive is a frame-relative exclusive row,
// never a global logical row. It is exactly the maximum of one row, the last
// exported semantic cell row plus one, and an in-range cursor row plus one.
// Cursor paint visibility is irrelevant, and an out-of-range cursor contributes
// nothing. The bottom is in [1, frame.rows], scrollback_rows is in
// [0, INT_MAX - (frame.rows - 1)], and offset_from_tail is in
// [0, scrollback_rows]. PRIMARY_BUFFER carries that shared viewport state.
// ALTERNATE_BUFFER requires zero scrollback and offset; it retains the honestly
// derived bottom even though presentation anchors the full frame grid.
struct terminal_canvas_content_extent_t
{
    std::uint16_t         record_version =
        k_terminal_canvas_content_extent_version;
    int                   content_bottom_row_exclusive = 1;
    int                   scrollback_rows = 0;
    int                   offset_from_tail = 0;
    Terminal_canvas_buffer active_buffer =
        Terminal_canvas_buffer::PRIMARY_BUFFER;
};

enum class Terminal_canvas_style_attribute : std::uint16_t
{
    BOLD      = 1U << 0U,
    FAINT     = 1U << 1U,
    ITALIC    = 1U << 2U,
    UNDERLINE = 1U << 3U,
    BLINK     = 1U << 4U,
    INVERSE   = 1U << 5U,
    INVISIBLE = 1U << 6U,
    STRIKE    = 1U << 7U,
};

struct Terminal_canvas_style
{
    // Colors are resolved from terminal default, palette, or truecolor
    // references. Presentation attributes and reverse_video remain separate
    // so a renderer applies FAINT/INVERSE/INVISIBLE exactly once.
    quint32       foreground_rgba = 0xffffffffU;
    quint32       background_rgba = 0xff000000U;
    std::uint16_t attributes      = 0U;
};

struct Terminal_canvas_cell
{
    int           row           = 0;
    int           column        = 0;
    int           display_width = 1;
    std::uint16_t style_index   = 0U;
    QString       text;
};

struct Terminal_canvas_cursor
{
    int                          row           = 0;
    int                          column        = 0;
    Terminal_canvas_cursor_shape shape         = Terminal_canvas_cursor_shape::BLOCK;
    bool                         visible       = false;
    bool                         blink_enabled = false;
};

struct Terminal_canvas_frame
{
    std::uint32_t                    api_version = k_terminal_canvas_frame_api_version;
    int                              rows        = 0;
    int                              columns     = 0;
    qreal                            font_size   = 13.0;
    QString                          font_family;
    QString                          font_style;
    int                              font_weight = 400;
    bool                             font_italic = false;
    qreal                            cell_width  = 0.0;
    qreal                            cell_height = 0.0;
    qreal                            content_width  = 0.0;
    qreal                            content_height = 0.0;
    std::uint64_t                    sequence               = 0U;
    std::uint64_t                    publication_generation = 0U;
    std::uint64_t                    row_origin_generation  = 0U;
    quint32                          default_foreground_rgba = 0xffffffffU;
    quint32                          default_background_rgba = 0xff000000U;
    quint32                          cursor_rgba             = 0xffffffffU;
    bool                             reverse_video           = false;
    // This record, cells, and cursor share the frame's sequence and generations
    // and must be installed, replaced, or cleared atomically. Absence makes only
    // the semantic-extent capability unavailable.
    std::optional<terminal_canvas_content_extent_t> content_extent;
    std::vector<Terminal_canvas_style> styles;
    std::vector<Terminal_canvas_cell>  cells;
    Terminal_canvas_cursor             cursor;
};

} // namespace vnm_terminal
