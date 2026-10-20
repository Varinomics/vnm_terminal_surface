#pragma once

#include "vnm_terminal/terminal_touch_selection.h"

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QtGlobal>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace vnm_terminal {

inline constexpr std::uint32_t k_terminal_canvas_frame_api_version = 4U;
inline constexpr std::uint16_t k_terminal_canvas_content_extent_version = 2U;
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
inline constexpr std::uint16_t k_terminal_canvas_wide_styles_version = 1U;
inline constexpr std::size_t k_terminal_canvas_max_wide_styles =
    k_terminal_canvas_max_cells + 1U;
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
// In version 2, content_bottom_row_exclusive is a frame-relative exclusive row,
// never a global logical row. It is exactly the maximum of one row, the last
// exported semantic cell row plus one, and a visible in-range cursor row plus one.
// Visibility is the terminal cursor mode, never the blinking paint phase: hidden
// cursors must not add blank rows, and blinking must not move the content anchor.
// An out-of-range cursor contributes nothing. The bottom is in [1, frame.rows],
// scrollback_rows is in
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
    bool operator==(const Terminal_canvas_style&) const = default;
};

inline constexpr std::uint16_t k_terminal_canvas_color_references_version = 1U;
inline constexpr std::uint16_t k_terminal_canvas_color_default = 256U;
inline constexpr std::uint16_t k_terminal_canvas_color_rgba = 257U;

struct terminal_canvas_style_color_refs_t
{
    std::uint16_t foreground = k_terminal_canvas_color_default;
    std::uint16_t background = k_terminal_canvas_color_default;

    bool operator==(const terminal_canvas_style_color_refs_t&) const = default;
};

// Entries share the enclosing frame's style indices. Selectors 0..255 refer
// to palette slots; RGBA preserves the corresponding resolved style color.
// The first entry is the reserved default style and uses DEFAULT for both
// channels. Known records cover every style; invalid records invalidate the
// enclosing frame. The record shares the frame's publication identity and is
// installed or cleared with it. Unknown versions leave only local color-scheme
// selection unavailable.
struct Terminal_canvas_color_references
{
    std::uint16_t record_version = k_terminal_canvas_color_references_version;
    std::vector<terminal_canvas_style_color_refs_t> styles;

    bool operator==(const Terminal_canvas_color_references&) const = default;
};

// A wide palette needs one style per exported cell plus the reserved default.
// Its marker and complete known color references belong to the same frame;
// removing either would lose the admission contract or color selectors.
// An unknown version makes this canvas frame unavailable.
struct Terminal_canvas_wide_styles
{
    std::uint16_t record_version = k_terminal_canvas_wide_styles_version;
    bool operator==(const Terminal_canvas_wide_styles&) const = default;
};

struct Terminal_canvas_cell
{
    int           row           = 0;
    int           column        = 0;
    int           display_width = 1;
    std::uint16_t style_index   = 0U;
    QString       text;
    bool operator==(const Terminal_canvas_cell&) const = default;
};

struct Terminal_canvas_cursor
{
    int                          row           = 0;
    int                          column        = 0;
    Terminal_canvas_cursor_shape shape         = Terminal_canvas_cursor_shape::BLOCK;
    bool                         visible       = false;
    bool                         blink_enabled = false;
};

inline constexpr std::uint16_t k_terminal_canvas_images_version = 1U;
inline constexpr int k_terminal_canvas_image_column_limit = 4'096;

enum class Terminal_canvas_images_status
{
    AVAILABLE,
    INVALID,
    OVER_LIMIT,
};

// One row's immutable RGBA8888 premultiplied pixels, measured against the
// physical cell where they were placed. A narrower receiving grid clips the
// slice; it does not change its original scale or first column. Revisions
// identify source pixels and may repeat across independent source processes.
struct Terminal_canvas_image_slice
{
    int           row               = 0;
    int           first_column      = 0;
    int           cell_pixel_width  = 0;
    int           cell_pixel_height = 0;
    std::uint64_t revision          = 0U;
    QImage        pixels;
    bool operator==(const Terminal_canvas_image_slice&) const = default;
};

// The record shares the enclosing frame's identity and is installed or cleared
// with it. Unknown versions leave only images unavailable. A known unavailable
// status carries no rows, so a failed transfer cannot masquerade as a partial
// image. Transport owners impose their own decoded-byte budgets.
struct Terminal_canvas_images
{
    std::uint16_t record_version = k_terminal_canvas_images_version;
    Terminal_canvas_images_status status = Terminal_canvas_images_status::AVAILABLE;
    std::vector<Terminal_canvas_image_slice> rows;
    bool operator==(const Terminal_canvas_images&) const = default;
};

inline bool terminal_canvas_images_are_valid(
    const Terminal_canvas_images& images,
    int                           frame_rows)
{
    if (images.record_version != k_terminal_canvas_images_version) {
        return true;
    }
    switch (images.status) {
        case Terminal_canvas_images_status::INVALID:
        case Terminal_canvas_images_status::OVER_LIMIT:
            return images.rows.empty();
        case Terminal_canvas_images_status::AVAILABLE:
            break;
        default:
            return false;
    }
    int previous_row = -1;
    for (const Terminal_canvas_image_slice& slice : images.rows) {
        if (slice.row <= previous_row || slice.row >= frame_rows ||
            slice.revision == 0U || slice.pixels.isNull() ||
            slice.pixels.format() != QImage::Format_RGBA8888_Premultiplied ||
            slice.pixels.bytesPerLine() != static_cast<qint64>(slice.pixels.width()) * 4 ||
            slice.cell_pixel_width <= 0 || slice.cell_pixel_height <= 0 ||
            slice.pixels.height() > slice.cell_pixel_height || slice.first_column < 0)
        {
            return false;
        }
        const std::int64_t column_span =
            (static_cast<std::int64_t>(slice.pixels.width()) + slice.cell_pixel_width - 1) /
            slice.cell_pixel_width;
        if (slice.first_column + column_span > k_terminal_canvas_image_column_limit) {
            return false;
        }
        previous_row = slice.row;
    }
    return true;
}

inline constexpr std::uint16_t k_terminal_canvas_leading_rows_version = 1U;

enum class Terminal_canvas_leading_rows_status
{
    AVAILABLE,
    INVALID,
    OVER_LIMIT,
};

// These retained rows precede the unchanged frame. A receiving presentation
// may fit their suffix into its unused top padding without moving the native
// screen. Selection describes the full frame with all leading rows prepended;
// its source geometry and retained row identities remain those of this frame.
struct Terminal_canvas_leading_rows
{
    std::uint16_t record_version = k_terminal_canvas_leading_rows_version;
    Terminal_canvas_leading_rows_status status = Terminal_canvas_leading_rows_status::AVAILABLE;
    int rows = 0;
    std::vector<Terminal_canvas_style> styles;
    std::optional<Terminal_canvas_color_references> color_references;
    std::vector<Terminal_canvas_cell> cells;
    std::optional<Terminal_canvas_images> images;
    std::optional<Terminal_canvas_selection> selection;
    std::optional<Terminal_canvas_wide_styles> wide_styles;
    bool operator==(const Terminal_canvas_leading_rows&) const = default;
};

inline bool terminal_canvas_leading_rows_are_valid(
    const Terminal_canvas_leading_rows& leading,
    int                                frame_rows,
    int                                frame_columns)
{
    if (leading.record_version != k_terminal_canvas_leading_rows_version) {
        return true;
    }
    switch (leading.status) {
        case Terminal_canvas_leading_rows_status::INVALID:
        case Terminal_canvas_leading_rows_status::OVER_LIMIT:
            return leading.rows == 0 && leading.styles.empty() && leading.cells.empty() &&
                !leading.color_references && !leading.images && !leading.selection && !leading.wide_styles;
        case Terminal_canvas_leading_rows_status::AVAILABLE:
            break;
        default:
            return false;
    }
    if (leading.rows <= 0 || leading.rows >= frame_rows ||
        !terminal_canvas_grid_fits_cell_budget(leading.rows, frame_columns) ||
        leading.cells.size() > (std::size_t)leading.rows * (std::size_t)frame_columns ||
        leading.styles.empty() || leading.styles.size() > k_terminal_canvas_max_wide_styles)
    {
        return false;
    }
    if (leading.wide_styles) {
        if (leading.wide_styles->record_version == k_terminal_canvas_wide_styles_version &&
            (!leading.color_references ||
                leading.color_references->record_version != k_terminal_canvas_color_references_version))
        {
            return false;
        }
    }
    else
    if (leading.styles.size() > k_terminal_canvas_max_styles) {
        return false;
    }
    constexpr std::uint16_t known_attributes =
        (std::uint16_t)Terminal_canvas_style_attribute::BOLD |
        (std::uint16_t)Terminal_canvas_style_attribute::FAINT |
        (std::uint16_t)Terminal_canvas_style_attribute::ITALIC |
        (std::uint16_t)Terminal_canvas_style_attribute::UNDERLINE |
        (std::uint16_t)Terminal_canvas_style_attribute::BLINK |
        (std::uint16_t)Terminal_canvas_style_attribute::INVERSE |
        (std::uint16_t)Terminal_canvas_style_attribute::INVISIBLE |
        (std::uint16_t)Terminal_canvas_style_attribute::STRIKE;
    if (leading.styles.front().attributes != 0U) {
        return false;
    }
    for (const auto& style : leading.styles) {
        if ((style.attributes & ~known_attributes) != 0U) {
            return false;
        }
    }
    if (leading.color_references &&
        leading.color_references->record_version == k_terminal_canvas_color_references_version)
    {
        if (leading.color_references->styles.size() != leading.styles.size()) {
            return false;
        }
        for (const auto& refs : leading.color_references->styles) {
            if (refs.foreground > k_terminal_canvas_color_rgba || refs.background > k_terminal_canvas_color_rgba) {
                return false;
            }
        }
        const auto& defaults = leading.color_references->styles.front();
        if (defaults.foreground != k_terminal_canvas_color_default ||
            defaults.background != k_terminal_canvas_color_default)
        {
            return false;
        }
    }
    int previous_row = -1;
    int previous_end = 0;
    qsizetype text_bytes = 0;
    for (const auto& cell : leading.cells) {
        if (cell.row < 0 || cell.row >= leading.rows || cell.row < previous_row ||
            cell.column < 0 || cell.display_width < 1 ||
            cell.display_width > frame_columns || cell.column > frame_columns - cell.display_width ||
            (cell.row == previous_row && cell.column < previous_end) ||
            cell.style_index >= leading.styles.size() ||
            cell.text.size() > k_terminal_canvas_max_cell_text_utf16_code_units)
        {
            return false;
        }
        text_bytes += cell.text.toUtf8().size();
        if (text_bytes > k_terminal_canvas_max_frame_text_utf8_bytes) {
            return false;
        }
        previous_row = cell.row;
        previous_end = cell.column + cell.display_width;
    }
    return (!leading.images || terminal_canvas_images_are_valid(*leading.images, leading.rows)) &&
        (!leading.selection || terminal_canvas_selection_is_valid(*leading.selection, frame_rows, frame_columns));
}

inline constexpr std::uint16_t k_terminal_canvas_presentation_version = 1U;

enum class Terminal_canvas_presentation_policy : std::uint8_t
{
    NATIVE,
    AUTOMATIC,
    COMPANION,
};

// The frame's presentation mapping record: which native row each display row
// presents. Interpret it only when record_version is known and the content
// extent is available; otherwise only this capability is unavailable and a
// receiver shows the native frame. Incoherent fields in a known version
// invalidate the complete enclosing frame.
//
// The frame's own rows are native logical rows from the content extent's
// scrollback_rows - offset_from_tail on. Display row d shows stream row
// scrollback_rows - offset_from_tail + d: NATIVE shows native rows as they
// are, AUTOMATIC shifts them down by hidden_rows, and COMPANION inserts
// hidden_rows padding rows before native row insertion_row. Coverage is the
// first covered_rows of the frame's own rows and the leading_rows rows the
// leading-rows record supplies directly above them. A frozen frame shows
// requested rows above its coverage as unused slots; a frame that lacks any
// other requested row is invalid.
struct Terminal_canvas_presentation
{
    std::uint16_t                       record_version = k_terminal_canvas_presentation_version;
    Terminal_canvas_presentation_policy policy         = Terminal_canvas_presentation_policy::NATIVE;
    int                                 hidden_rows    = 0;
    int                                 insertion_row  = 0;
    int                                 covered_rows   = 0;
    int                                 leading_rows   = 0;
    bool                                frozen         = false;
    bool operator==(const Terminal_canvas_presentation&) const = default;
};

// Field coherence of a known record against its frame; the receiver checks
// that the coverage supplies every requested row.
inline bool terminal_canvas_presentation_is_valid(
    const Terminal_canvas_presentation& presentation,
    int                                 frame_rows,
    int                                 scrollback_rows,
    int                                 offset_from_tail)
{
    if (presentation.record_version != k_terminal_canvas_presentation_version) {
        return true;
    }
    switch (presentation.policy) {
        case Terminal_canvas_presentation_policy::NATIVE:
            if (presentation.hidden_rows != 0) {
                return false;
            }
            break;
        case Terminal_canvas_presentation_policy::AUTOMATIC:
        case Terminal_canvas_presentation_policy::COMPANION:
            break;
        default:
            return false;
    }
    return
        presentation.hidden_rows   >= 0 && presentation.hidden_rows   <= frame_rows      &&
        presentation.insertion_row >= 0 && presentation.insertion_row <= scrollback_rows &&
        presentation.covered_rows  >= 0 && presentation.covered_rows  <= frame_rows      &&
        presentation.leading_rows  >= 0 && presentation.leading_rows  <  frame_rows      &&
        presentation.leading_rows  <= scrollback_rows - offset_from_tail;
}

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
    std::optional<Terminal_canvas_color_references> color_references;
    std::optional<Terminal_canvas_images> images;
    std::optional<Terminal_canvas_selection> selection;
    std::vector<Terminal_canvas_style> styles;
    std::vector<Terminal_canvas_cell>  cells;
    Terminal_canvas_cursor             cursor;
    std::optional<Terminal_canvas_wide_styles> wide_styles;
    std::optional<Terminal_canvas_leading_rows> leading_rows;
    std::optional<Terminal_canvas_presentation> presentation;
};

// Zero rejects an unsupported marker instead of interpreting it as the
// unmarked 256-style contract. Callers still validate cells and color references.
inline std::size_t terminal_canvas_style_limit(const Terminal_canvas_frame& frame)
{
    if (!frame.wide_styles) {
        return k_terminal_canvas_max_styles;
    }
    if (frame.wide_styles->record_version != k_terminal_canvas_wide_styles_version) {
        return 0U;
    }
    return frame.cells.size() < k_terminal_canvas_max_cells
        ? frame.cells.size() + 1U
        : k_terminal_canvas_max_wide_styles;
}

} // namespace vnm_terminal
