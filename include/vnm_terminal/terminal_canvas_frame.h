#pragma once

#include <QString>
#include <QtGlobal>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class VNM_TerminalSurface;

namespace vnm_terminal {

inline constexpr std::uint32_t k_terminal_canvas_frame_api_version = 1U;
inline constexpr int           k_terminal_canvas_max_rows          = 200;
inline constexpr int           k_terminal_canvas_max_columns       = 300;
inline constexpr std::size_t   k_terminal_canvas_max_cells         = 32'768U;
inline constexpr std::size_t   k_terminal_canvas_max_styles        = 256U;

enum class Terminal_canvas_export_status
{
    OK,
    NO_FRAME,
    WRONG_THREAD,
    INVALID_SOURCE_FRAME,
    ROW_LIMIT_EXCEEDED,
    COLUMN_LIMIT_EXCEEDED,
    CELL_LIMIT_EXCEEDED,
    STYLE_LIMIT_EXCEEDED,
};

enum class Terminal_canvas_cursor_shape
{
    BLOCK,
    BAR,
    UNDERLINE,
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
    std::uint64_t                    sequence               = 0U;
    std::uint64_t                    publication_generation = 0U;
    std::uint64_t                    row_origin_generation  = 0U;
    quint32                          default_foreground_rgba = 0xffffffffU;
    quint32                          default_background_rgba = 0xff000000U;
    quint32                          cursor_rgba             = 0xffffffffU;
    bool                             reverse_video           = false;
    std::vector<Terminal_canvas_style> styles;
    std::vector<Terminal_canvas_cell>  cells;
    Terminal_canvas_cursor             cursor;
};

struct Terminal_canvas_export_result
{
    Terminal_canvas_export_status              status = Terminal_canvas_export_status::NO_FRAME;
    std::shared_ptr<const Terminal_canvas_frame> frame;
};

// Call on the surface's owning thread. The returned frame owns all of its data;
// later surface publications cannot mutate a previously returned frame.
Terminal_canvas_export_result export_terminal_canvas_frame(
    const VNM_TerminalSurface& surface);

} // namespace vnm_terminal
