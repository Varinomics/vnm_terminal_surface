#include "vnm_terminal/internal/terminal_screen_model.h"

#include "vnm_terminal/internal/interaction_trace.h"
#include "vnm_terminal/internal/csi_parameter_parsing.h"
#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/sixel_decoder.h"
#include "vnm_terminal/internal/terminal_color_scheme.h"
#include "vnm_terminal/internal/terminal_history_row_record_codec.h"
#include "vnm_terminal/internal/terminal_repaint_recovery.h"
#include "vnm_terminal/internal/unicode_width.h"

#include <QByteArray>
#include <QChar>
#include <QDateTime>
#include <QImage>
#include <QSize>
#include <QStringList>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace vnm_terminal::internal {

const char* terminal_selection_attachment_resolution_status_token(
    Terminal_selection_attachment_resolution_status status) noexcept
{
    switch (status) {
        case Terminal_selection_attachment_resolution_status::TRANSLATED:
            return "translated";
        case Terminal_selection_attachment_resolution_status::INVALID_LEASE:
            return "invalid-lease";
        case Terminal_selection_attachment_resolution_status::SOURCE_MISMATCH:
            return "source-mismatch";
        case Terminal_selection_attachment_resolution_status::BUFFER_MISMATCH:
            return "buffer-mismatch";
        case Terminal_selection_attachment_resolution_status::EPOCH_MISMATCH:
            return "epoch-mismatch";
        case Terminal_selection_attachment_resolution_status::GRID_REFLOW_MISMATCH:
            return "grid-reflow-mismatch";
        case Terminal_selection_attachment_resolution_status::GRID_SIZE_MISMATCH:
            return "grid-size-mismatch";
        case Terminal_selection_attachment_resolution_status::CAPABILITY_UNAVAILABLE:
            return "capability-unavailable";
        case Terminal_selection_attachment_resolution_status::MISSING_LINE:
            return "missing-line";
        case Terminal_selection_attachment_resolution_status::CONTENT_GENERATION_MISMATCH:
            return "content-generation-mismatch";
        case Terminal_selection_attachment_resolution_status::DUPLICATE_RESOLUTION:
            return "duplicate-resolution";
        case Terminal_selection_attachment_resolution_status::REORDERED_RESOLUTION:
            return "reordered-resolution";
        case Terminal_selection_attachment_resolution_status::NONCONTIGUOUS_RESOLUTION:
            return "noncontiguous-resolution";
        case Terminal_selection_attachment_resolution_status::INCONSISTENT_ROW_DELTA:
            return "inconsistent-row-delta";
        case Terminal_selection_attachment_resolution_status::ENDPOINT_UNREPRESENTABLE:
            return "endpoint-unrepresentable";
    }

    return "unknown";
}

namespace {

constexpr std::size_t k_printable_ascii_count =
    k_printable_ascii_last - k_printable_ascii_first + 1U;
constexpr int k_primary_repaint_recovery_resize_guard_action_budget = 64;

template <typename T>
constexpr bool k_unhandled_screen_mutation = false;

enum class Dec_private_mode_notify
{
    MODE_STATE,
    MOUSE_REPORTING,
    ALTERNATE_SCROLL,
};

struct Simple_dec_private_mode
{
    int                            mode;
    bool Terminal_mode_state::*    field;
    Dec_private_mode_notify        notify;
};

constexpr std::array<Simple_dec_private_mode, 7> k_simple_dec_private_modes = {{
    { 1,    &Terminal_mode_state::application_cursor_keys, Dec_private_mode_notify::MODE_STATE       },
    { 5,    &Terminal_mode_state::reverse_video,           Dec_private_mode_notify::MODE_STATE       },
    { 25,   &Terminal_mode_state::cursor_visible,          Dec_private_mode_notify::MODE_STATE       },
    { 1004, &Terminal_mode_state::focus_reporting,         Dec_private_mode_notify::MODE_STATE       },
    { 1006, &Terminal_mode_state::sgr_mouse_encoding,      Dec_private_mode_notify::MOUSE_REPORTING  },
    { 1007, &Terminal_mode_state::alternate_scroll,        Dec_private_mode_notify::ALTERNATE_SCROLL },
    { 2004, &Terminal_mode_state::bracketed_paste,         Dec_private_mode_notify::MODE_STATE       },
}};

// Modes whose DECRQM status is permanently RESET (4) and whose application
// reports them as unsupported. 2027 also rejects on apply, but its DECRQM
// query path emits a diagnostic instead of a status reply, so it is handled
// in the bespoke switch.
constexpr std::array<int, 3> k_permanently_reset_dec_private_modes = {3, 1005, 1015};

bool is_printable_ascii(QChar character)
{
    const ushort codepoint = character.unicode();
    return codepoint >= k_printable_ascii_first && codepoint <= k_printable_ascii_last;
}

bool is_single_width_non_ascii_bmp(QChar character)
{
    const ushort codepoint = character.unicode();
    if (is_printable_ascii(character) ||
        character.isHighSurrogate()   ||
        character.isLowSurrogate())
    {
        return false;
    }

    if (codepoint == 0x2588U) {
        return true;
    }

    return width_for_codepoint(static_cast<char32_t>(codepoint)).cells == 1;
}

terminal_history_handle_t retained_history_handle_from_provenance(
    const Terminal_retained_line_provenance& provenance)
{
    return terminal_history_handle_from_retained_identity(
        provenance.retained_line_id,
        provenance.content_generation);
}

bool is_retained_identity_compatibility_handle(terminal_history_handle_t handle)
{
    return
        handle.epoch         == k_terminal_history_retained_identity_epoch &&
        handle.byte_sequence == handle.row_sequence &&
        handle.record_bytes  == k_terminal_history_retained_identity_record_bytes;
}

Terminal_history_resolution_status retained_history_handle_match_status(
    terminal_history_handle_t actual,
    terminal_history_handle_t expected)
{
    if (actual.epoch != expected.epoch) {
        return Terminal_history_resolution_status::STALE_EPOCH;
    }

    if (is_retained_identity_compatibility_handle(expected) &&
        actual.row_sequence == expected.row_sequence)
    {
        if (actual.content_generation != expected.content_generation) {
            return Terminal_history_resolution_status::CONTENT_GENERATION_MISMATCH;
        }

        return Terminal_history_resolution_status::OK;
    }

    if (expected.epoch         == k_terminal_history_retained_identity_epoch &&
        expected.byte_sequence == expected.row_sequence                    &&
        actual.row_sequence    == expected.row_sequence)
    {
        return Terminal_history_resolution_status::RECORD_SIZE_MISMATCH;
    }

    if (actual.byte_sequence != expected.byte_sequence) {
        return Terminal_history_resolution_status::STALE_BYTE_SEQUENCE;
    }

    if (actual.row_sequence != expected.row_sequence) {
        return Terminal_history_resolution_status::STALE_ROW_SEQUENCE;
    }

    if (actual.record_bytes != expected.record_bytes) {
        return Terminal_history_resolution_status::RECORD_SIZE_MISMATCH;
    }

    if (actual.content_generation != expected.content_generation) {
        return Terminal_history_resolution_status::CONTENT_GENERATION_MISMATCH;
    }

    return Terminal_history_resolution_status::OK;
}

std::unique_ptr<Terminal_history_ring> make_retained_history_ring(
    std::size_t capacity_bytes)
{
    auto ring = std::make_unique<Terminal_history_ring>(
        terminal_history_ring_config_t{
            capacity_bytes,
            0U,
        });
    if (!ring->ok()) {
        throw std::runtime_error("terminal retained history ring initialization failed");
    }

    return ring;
}

[[noreturn]] void throw_retained_history_storage_failure()
{
    throw std::runtime_error("terminal retained history ring storage operation failed");
}

const QString& printable_ascii_cell_text(QChar character)
{
    Q_ASSERT(is_printable_ascii(character));

    static const std::array<QString, k_printable_ascii_count> strings{
        QStringLiteral(" "),
        QStringLiteral("!"),
        QStringLiteral("\""),
        QStringLiteral("#"),
        QStringLiteral("$"),
        QStringLiteral("%"),
        QStringLiteral("&"),
        QStringLiteral("'"),
        QStringLiteral("("),
        QStringLiteral(")"),
        QStringLiteral("*"),
        QStringLiteral("+"),
        QStringLiteral(","),
        QStringLiteral("-"),
        QStringLiteral("."),
        QStringLiteral("/"),
        QStringLiteral("0"),
        QStringLiteral("1"),
        QStringLiteral("2"),
        QStringLiteral("3"),
        QStringLiteral("4"),
        QStringLiteral("5"),
        QStringLiteral("6"),
        QStringLiteral("7"),
        QStringLiteral("8"),
        QStringLiteral("9"),
        QStringLiteral(":"),
        QStringLiteral(";"),
        QStringLiteral("<"),
        QStringLiteral("="),
        QStringLiteral(">"),
        QStringLiteral("?"),
        QStringLiteral("@"),
        QStringLiteral("A"),
        QStringLiteral("B"),
        QStringLiteral("C"),
        QStringLiteral("D"),
        QStringLiteral("E"),
        QStringLiteral("F"),
        QStringLiteral("G"),
        QStringLiteral("H"),
        QStringLiteral("I"),
        QStringLiteral("J"),
        QStringLiteral("K"),
        QStringLiteral("L"),
        QStringLiteral("M"),
        QStringLiteral("N"),
        QStringLiteral("O"),
        QStringLiteral("P"),
        QStringLiteral("Q"),
        QStringLiteral("R"),
        QStringLiteral("S"),
        QStringLiteral("T"),
        QStringLiteral("U"),
        QStringLiteral("V"),
        QStringLiteral("W"),
        QStringLiteral("X"),
        QStringLiteral("Y"),
        QStringLiteral("Z"),
        QStringLiteral("["),
        QStringLiteral("\\"),
        QStringLiteral("]"),
        QStringLiteral("^"),
        QStringLiteral("_"),
        QStringLiteral("`"),
        QStringLiteral("a"),
        QStringLiteral("b"),
        QStringLiteral("c"),
        QStringLiteral("d"),
        QStringLiteral("e"),
        QStringLiteral("f"),
        QStringLiteral("g"),
        QStringLiteral("h"),
        QStringLiteral("i"),
        QStringLiteral("j"),
        QStringLiteral("k"),
        QStringLiteral("l"),
        QStringLiteral("m"),
        QStringLiteral("n"),
        QStringLiteral("o"),
        QStringLiteral("p"),
        QStringLiteral("q"),
        QStringLiteral("r"),
        QStringLiteral("s"),
        QStringLiteral("t"),
        QStringLiteral("u"),
        QStringLiteral("v"),
        QStringLiteral("w"),
        QStringLiteral("x"),
        QStringLiteral("y"),
        QStringLiteral("z"),
        QStringLiteral("{"),
        QStringLiteral("|"),
        QStringLiteral("}"),
        QStringLiteral("~"),
    };

    return strings[static_cast<std::size_t>(
        character.unicode() - k_printable_ascii_first)];
}

// Undrawn image pixels are all zero, whatever the byte order.
bool image_block_has_drawn_pixel(const QImage& pixels, int x_first, int x_end)
{
    for (int y = 0; y < pixels.height(); ++y) {
        const auto* line = reinterpret_cast<const std::uint32_t*>(pixels.constScanLine(y));
        if (std::any_of(line + x_first, line + x_end, [](std::uint32_t pixel) { return pixel != 0U; })) {
            return true;
        }
    }
    return false;
}

// Where a band and its row's earlier image end up in their composite, worked
// out before anything is allocated, so a composite over the decoded-size cap
// is never made. An earlier image placed on another cell is resampled to this
// one, the size a renderer draws it at, so a row's image keeps one cell size.
struct composite_layout_t
{
    int           first_column   = 0;
    int           earlier_width  = 0;
    int           earlier_height = 0;
    int           earlier_x      = 0;
    int           band_x         = 0;
    int           width          = 0;
    int           height         = 0;
    std::int64_t  bytes          = 0;
};

composite_layout_t composite_image_layout(
    const Terminal_image_slice& earlier,
    const QImage&               band,
    int                         band_first_column,
    terminal_cell_pixel_size_t  cell)
{
    composite_layout_t layout;
    layout.earlier_width  = earlier.pixels.width();
    layout.earlier_height = earlier.pixels.height();
    if (earlier.cell_pixel_size != cell) {
        const double x_scale = static_cast<double>(cell.width)  / earlier.cell_pixel_size.width;
        const double y_scale = static_cast<double>(cell.height) / earlier.cell_pixel_size.height;
        layout.earlier_width  = std::max(1, static_cast<int>(std::lround(layout.earlier_width * x_scale)));
        layout.earlier_height = std::clamp(
            static_cast<int>(std::lround(layout.earlier_height * y_scale)),
            1,
            cell.height);
    }

    layout.first_column = std::min(earlier.first_column, band_first_column);
    layout.earlier_x    = (earlier.first_column - layout.first_column) * cell.width;
    layout.band_x       = (band_first_column    - layout.first_column) * cell.width;
    layout.width        = std::max(layout.earlier_x + layout.earlier_width, layout.band_x + band.width());
    layout.height       = std::max(layout.earlier_height, band.height());
    layout.bytes        = std::int64_t{layout.width} * layout.height * 4;
    return layout;
}

// Draws a band over its row's earlier image, drawn pixels over earlier ones,
// into one image over the union of their columns.
QImage composite_image_band(
    const Terminal_image_slice& earlier,
    const QImage&               band,
    const composite_layout_t&   layout)
{
    QImage earlier_pixels = earlier.pixels;
    if (earlier_pixels.width()  != layout.earlier_width ||
        earlier_pixels.height() != layout.earlier_height)
    {
        earlier_pixels = earlier_pixels
            .scaled(
                layout.earlier_width,
                layout.earlier_height,
                Qt::IgnoreAspectRatio,
                Qt::FastTransformation)
            .convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    }

    const int earlier_x = layout.earlier_x;
    const int band_x    = layout.band_x;
    QImage combined(layout.width, layout.height, QImage::Format_RGBA8888_Premultiplied);
    // QImage reports a failed allocation with a null image, not an exception.
    if (combined.isNull() || earlier_pixels.isNull()) {
        throw std::bad_alloc();
    }
    combined.fill(0U);

    for (int y = 0; y < earlier_pixels.height(); ++y) {
        std::memcpy(
            combined.scanLine(y) + static_cast<std::size_t>(earlier_x) * sizeof(std::uint32_t),
            earlier_pixels.constScanLine(y),
            static_cast<std::size_t>(earlier_pixels.width()) * sizeof(std::uint32_t));
    }
    for (int y = 0; y < band.height(); ++y) {
        auto*       target = reinterpret_cast<std::uint32_t*>(combined.scanLine(y)) + band_x;
        const auto* source = reinterpret_cast<const std::uint32_t*>(band.constScanLine(y));
        for (int x = 0; x < band.width(); ++x) {
            if (source[x] != 0U) {
                target[x] = source[x];
            }
        }
    }
    return combined;
}

// A pixel row y of an image decoded at `decoded_aspect` rows per sixel pixel,
// at `aspect` rows instead: every run of `decoded_aspect` rows keeps its
// first `aspect` rows.
std::int64_t sixel_row_at_aspect(std::int64_t y, int decoded_aspect, int aspect)
{
    return y / decoded_aspect * aspect + std::min<std::int64_t>(y % decoded_aspect, aspect);
}

// Placement's charges to a Sixel_work_budget, whose unit is about one pixel
// written or copied: the fixed part of placing one band, and of one region
// scroll with its history append, per step and per region row. Measured
// against the decoder's pixel work.
constexpr std::uint64_t k_sixel_band_step_units   = 2048U;
constexpr std::uint64_t k_sixel_scroll_step_units = 24576U;
constexpr std::uint64_t k_sixel_scroll_row_units  = 64U;

// The decoder's raster at a smaller aspect ratio, which is the raster it
// would have decoded at that ratio.
QImage sixel_raster_at_aspect(const QImage& raster, int decoded_aspect, int aspect)
{
    const int height = static_cast<int>(sixel_row_at_aspect(raster.height(), decoded_aspect, aspect));
    QImage result(raster.width(), height, raster.format());
    // QImage reports a failed allocation with a null image, not an exception.
    if (result.isNull()) {
        throw std::bad_alloc();
    }
    for (int y = 0; y < height; ++y) {
        std::memcpy(
            result.scanLine(y),
            raster.constScanLine(y / aspect * decoded_aspect + y % aspect),
            static_cast<std::size_t>(raster.width()) * sizeof(std::uint32_t));
    }
    return result;
}

bool action_is_session_visible(const Parser_action& action)
{
    switch (parser_action_kind(action)) {
        case Parser_action_kind::TERMINAL_REPLY:
        case Parser_action_kind::HOST_REQUEST:
            return true;
        case Parser_action_kind::NOTIFICATION:
    {
                            const Parser_notification& notification =
                                std::get<Parser_notification>(action.payload);
                            return notification.kind != Parser_notification_kind::OUTPUT_ACTIVITY;
        }
        case Parser_action_kind::SCREEN_MUTATION:
        case Parser_action_kind::STYLE_MUTATION:
        case Parser_action_kind::CONTROL_SEQUENCE:
        case Parser_action_kind::TERMINAL_QUERY:
        case Parser_action_kind::DIAGNOSTIC:
            return false;
    }

    return false;
}

Parser_action make_text_area_resize_notification_action(int rows, int columns)
{
    return {
        Parser_notification{
            Parser_notification_kind::TEXT_AREA_RESIZE_REQUESTED,
            {},
            rows,
            columns,
        },
    };
}

QByteArray four_digit_hex(int value)
{
    QByteArray hex = QByteArray::number(value, 16);
    while (hex.size() < 4) {
        hex.prepend('0');
    }
    return hex;
}

QByteArray color_reply_payload(quint32 rgba)
{
    const int red   = static_cast<int>((rgba >> 16U) & 0xffU);
    const int green = static_cast<int>((rgba >> 8U) & 0xffU);
    const int blue  = static_cast<int>(rgba & 0xffU);

    return
        QByteArrayLiteral("rgb:")   +
        four_digit_hex(red * 257)   +
        '/'                         +
        four_digit_hex(green * 257) +
        '/'                         +
        four_digit_hex(blue * 257);
}

// The sixel geometry XTSMGRAPHICS reports: the text area in pixels, so that an
// image of that size shows whole, scaled down with its shape kept where it holds
// more pixels than the decoded-size cap allows, so that such an image decodes.
QSize sixel_graphics_geometry(
    terminal_grid_size_t       grid_size,
    terminal_cell_pixel_size_t cell,
    std::size_t                limit_bytes)
{
    const std::int64_t limit_pixels =
        static_cast<std::int64_t>(limit_bytes) / k_sixel_bytes_per_pixel;
    const std::int64_t width  = std::int64_t{grid_size.columns} * cell.width;
    const std::int64_t height = std::int64_t{grid_size.rows}    * cell.height;
    if (width <= limit_pixels / height) {
        return QSize(static_cast<int>(width), static_cast<int>(height));
    }

    const double scale = std::sqrt(
        static_cast<double>(limit_pixels) /
        (static_cast<double>(width) * static_cast<double>(height)));
    const std::int64_t fitted_width = std::clamp<std::int64_t>(
        static_cast<std::int64_t>(static_cast<double>(width) * scale),
        1,
        std::min(width, limit_pixels));
    const std::int64_t fitted_height = std::min(height, limit_pixels / fitted_width);
    return QSize(static_cast<int>(fitted_width), static_cast<int>(fitted_height));
}

}

const char* terminal_recovery_attempt_status_token(
    Terminal_recovery_attempt_status status) noexcept
{
    switch (status) {
        case Terminal_recovery_attempt_status::ACCEPTED:  return "accepted";
        case Terminal_recovery_attempt_status::REJECTED:  return "rejected";
        case Terminal_recovery_attempt_status::CANCELLED: return "cancelled";
    }
    return "unknown";
}

const char* terminal_recovery_attempt_reason_token(
    Terminal_recovery_attempt_reason reason) noexcept
{
    switch (reason) {
        case Terminal_recovery_attempt_reason::FULL_SHIFT_MATCH:
            return "full-shift-match";
        case Terminal_recovery_attempt_reason::PARTIAL_SHIFT_MATCH:
            return "partial-shift-match";
        case Terminal_recovery_attempt_reason::PARTIAL_ANCHORED_SHIFT_MATCH:
            return "partial-anchored-shift-match";
        case Terminal_recovery_attempt_reason::NONMATCHING:
            return "nonmatching";
        case Terminal_recovery_attempt_reason::REPEATED_ROW_AMBIGUOUS:
            return "repeated-row-ambiguous";
        case Terminal_recovery_attempt_reason::RECOVERY_DISABLED:
            return "recovery-disabled";
        case Terminal_recovery_attempt_reason::CANDIDATE_INVALIDATED:
            return "candidate-invalidated";
    }
    return "unknown";
}

Terminal_text_area_resize_request_status terminal_text_area_resize_request_status(
    const std::vector<Sgr_parameter_group>&    groups,
    terminal_grid_size_t&                      grid_size)
{
    int mode = 0;
    if (groups.size() != 3U                       ||
        !csi_parameter_value(groups, 0U, 0, mode) ||
        mode != 8)
    {
        return Terminal_text_area_resize_request_status::NOT_REQUESTED;
    }

    int rows    = 0;
    int columns = 0;
    if (!csi_parameter_value(groups, 1U, 0, rows) ||
        !csi_parameter_value(groups, 2U, 0, columns))
    {
        return Terminal_text_area_resize_request_status::MALFORMED;
    }
    if (rows <= 0 || columns <= 0) {
        return Terminal_text_area_resize_request_status::UNSUPPORTED;
    }

    grid_size = terminal_grid_size_t{rows, columns};
    return is_terminal_screen_model_grid_size_supported(grid_size)
        ? Terminal_text_area_resize_request_status::SUPPORTED
        : Terminal_text_area_resize_request_status::UNSUPPORTED;
}

Terminal_screen_model_config_status validate_terminal_screen_model_config(
    const Terminal_screen_model_config& config)
{
    if (!is_terminal_screen_model_grid_size_supported(config.grid_size)) {
        return Terminal_screen_model_config_status::INVALID_GRID_SIZE;
    }

    if (config.scrollback_limit < 0) {
        return Terminal_screen_model_config_status::INVALID_SCROLLBACK_LIMIT;
    }

    const std::size_t retained_capacity = config.retained_history_capacity_bytes;
    if (retained_capacity < k_terminal_min_retained_history_capacity_bytes ||
        retained_capacity > k_terminal_max_retained_history_capacity_bytes)
    {
        return Terminal_screen_model_config_status::INVALID_RETAINED_HISTORY_CAPACITY;
    }

    if (config.tab_width <= 0) {
        return Terminal_screen_model_config_status::INVALID_TAB_WIDTH;
    }

    if (config.cell_pixel_size.has_value() &&
        !is_valid_cell_pixel_size(*config.cell_pixel_size))
    {
        return Terminal_screen_model_config_status::INVALID_CELL_PIXEL_SIZE;
    }

    return Terminal_screen_model_config_status::OK;
}

Terminal_screen_model::Terminal_screen_model(Terminal_screen_model_config config)
:
    m_config(config),
    m_color_state(make_terminal_color_state(default_color_scheme())),
    m_current_style(make_default_terminal_text_style()),
    m_styles{make_default_terminal_text_style()},
    m_style_ids_by_value{{
        terminal_text_style_lookup_key(make_default_terminal_text_style()),
        k_default_terminal_style_id,
    }}
{
    if (validate_terminal_screen_model_config(m_config) !=
        Terminal_screen_model_config_status::OK)
    {
        throw std::invalid_argument("invalid Terminal_screen_model_config");
    }

    m_primary_backing.retained_history.capacity_bytes =
        terminal_history_ring_aligned_capacity(
            m_config.retained_history_capacity_bytes);
    m_parser.set_sixel_raster_limit_bytes(sixel_raster_limit_bytes());

    reset_grid();
    refresh_active_grid_retained_lookup_indexes();
}

Terminal_screen_model::Resize_transition_scope::Resize_transition_scope(
    Terminal_screen_model& model,
    const terminal_screen_model_resize_transition_sink_t* sink,
    ingest_publication_t& publication)
:
    m_model(model)
{
    Q_ASSERT(m_model.m_resize_transition_sink == nullptr);
    Q_ASSERT(m_model.m_resize_transition_publication == nullptr);
    Q_ASSERT(!m_model.m_pending_resize_transition.has_value());
    Q_ASSERT(!m_model.m_skip_next_resize_transition_accumulation);
    if (sink != nullptr) {
        Q_ASSERT(sink->consume != nullptr);
    }
    m_model.m_resize_transition_sink        = sink;
    m_model.m_resize_transition_publication = &publication;
    m_model.m_resize_transition_ordinal     = 0U;
    m_model.m_text_area_resize_request_ordinal = 0U;
    m_model.m_skip_next_resize_transition_accumulation = false;
}

Terminal_screen_model::Resize_transition_scope::~Resize_transition_scope()
{
    m_model.m_pending_resize_transition.reset();
    m_model.m_resize_transition_publication = nullptr;
    m_model.m_resize_transition_sink        = nullptr;
    m_model.m_resize_transition_ordinal     = 0U;
    m_model.m_text_area_resize_request_ordinal = 0U;
    m_model.m_skip_next_resize_transition_accumulation = false;
}

Terminal_screen_model_result Terminal_screen_model::ingest(
    QByteArrayView bytes,
    const terminal_screen_model_resize_transition_sink_t* resize_transition_sink,
    Sixel_work_budget* sixel_work_budget)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_screen_model::ingest");

    Terminal_screen_model_result result;
    m_scrollback_evicted_rows = 0;
    clear_backing_deltas();
    clear_recovery_proposals();
    clear_selection_continuity();
    clear_dirty();

    ingest_publication_t publication;
    ingest_publication_t resize_transition_publication;
    Resize_transition_scope resize_transition_scope(
        *this,
        resize_transition_sink,
        resize_transition_publication);

    struct Budget_scope
    {
        Sixel_work_budget*& slot;
        ~Budget_scope() { slot = nullptr; }
    } budget_scope{m_sixel_work_budget};
    m_sixel_work_budget = sixel_work_budget;

    // A placement left waiting by an earlier call continues first, and
    // nothing is parsed until it ends. Once it has started, its changes are
    // held like synchronized output and released when it ends.
    qsizetype parsed   = 0;
    bool      deferred = false;
    if (m_sixel_placement.has_value()) {
        Q_ASSERT(bytes.empty());
        clear_dirty();
        advance_sixel_placement(result.actions);
        accumulate_pending_changes(resize_transition_publication);
        deferred = m_sixel_placement.has_value();
        if (deferred || m_modes.synchronized_output) {
            collect_synchronized_changes();
            m_sixel_placement_held = m_sixel_placement_held || deferred;
        }
        else
        if (m_sixel_placement_held) {
            collect_synchronized_changes();
            release_synchronized_changes(publication);
        }
        else {
            publish_pending_changes(publication);
        }
        if (!deferred) {
            m_sixel_placement_held = false;
        }
    }

    // The parser stops after each sixel image, so each image is applied and
    // its raster released before the next one is decoded: however many images
    // one chunk describes, at most one decoded raster is alive at a time.
    // Only a model that retains its structural actions keeps them all. The
    // parser also stops where its budget runs out, and a placement that does
    // not fit the budget waits; either ends this call.
    if (!deferred) do {
        std::vector<Parser_action> parser_actions;
        {
            VNM_TERMINAL_PROFILE_SCOPE("Terminal_screen_model::parser_ingest");
            parser_actions = m_parser.ingest(bytes, parsed, sixel_work_budget);
        }

        VNM_TERMINAL_PROFILE_SCOPE("Terminal_screen_model::apply_parser_actions");
        if (m_config.retain_structural_actions && result.actions.empty()) {
            result.actions.reserve(parser_actions.size());
        }

        for (const Parser_action& action : parser_actions) {
            const std::size_t result_action_begin = result.actions.size();
            if (m_config.retain_structural_actions || action_is_session_visible(action)) {
                result.actions.push_back(action);
            }
            clear_dirty();
            apply_action(action, result.actions, &publication);
            advance_primary_repaint_recovery_resize_guard();
            std::optional<terminal_grid_size_t> text_area_resize_request;
            for (std::size_t index = result_action_begin;
                 index < result.actions.size();
                 ++index)
            {
                const Parser_action& result_action = result.actions[index];
                if (parser_action_kind(result_action) != Parser_action_kind::NOTIFICATION) {
                    continue;
                }
                const Parser_notification& notification =
                    std::get<Parser_notification>(result_action.payload);
                if (notification.kind ==
                    Parser_notification_kind::TEXT_AREA_RESIZE_REQUESTED)
                {
                    Q_ASSERT(!text_area_resize_request.has_value());
                    text_area_resize_request = terminal_grid_size_t{
                        notification.rows,
                        notification.columns,
                    };
                }
            }
            const bool text_area_resize_changed_model_grid =
                text_area_resize_request.has_value() &&
                m_pending_resize_transition.has_value() &&
                grid_sizes_match(
                    m_pending_resize_transition->second,
                    *text_area_resize_request);
            if (m_pending_resize_transition.has_value()) {
                emit_pending_resize_transition();
            }
            else {
                accumulate_pending_changes(resize_transition_publication);
            }
            if (text_area_resize_request.has_value()) {
                emit_text_area_resize_request(
                    *text_area_resize_request,
                    text_area_resize_changed_model_grid);
            }

            if (m_modes.synchronized_output) {
                collect_synchronized_changes();
            }
            else {
                publish_pending_changes(publication);
            }
        }
        deferred = m_parser.sixel_work_deferred() || m_sixel_placement.has_value();
    }
    while (!deferred && parsed < bytes.size());

    // The chunk's end is where an unbudgeted ingest would have finished it.
    if (!deferred && m_primary_repaint_recovery_candidate.active && m_modes.cursor_visible) {
        clear_dirty();
        finish_primary_repaint_recovery_candidate(false);
        accumulate_pending_changes(resize_transition_publication);
        if (m_modes.synchronized_output) {
            collect_synchronized_changes();
        }
        else {
            publish_pending_changes(publication);
        }
    }

    result_change_overrides_t overrides;
    overrides.dirty_rows_have_stable_mutation_identity =
        publication.dirty_rows_have_stable_mutation_identity;
    assign_trailing_changes(
        resize_transition_publication,
        resize_transition_sink != nullptr
            ? resize_transition_sink->trailing_changes
            : nullptr);
    adopt_publication_changes(std::move(publication));

    {
        VNM_TERMINAL_PROFILE_SCOPE("Terminal_screen_model::finalize_ingest_result");
        result = finalize_result(std::move(result), overrides);
    }
    result.consumed_bytes     = parsed;
    result.sixel_work_pending = deferred;
    return result;
}

Terminal_screen_model_result Terminal_screen_model::force_release_synchronized_output(
    terminal_screen_model_trailing_changes_t* trailing_changes)
{
    Terminal_screen_model_result result;
    m_scrollback_evicted_rows = 0;
    clear_backing_deltas();
    clear_recovery_proposals();
    clear_selection_continuity();
    clear_dirty();

    ingest_publication_t publication;
    ingest_publication_t resize_transition_publication;
    Resize_transition_scope resize_transition_scope(
        *this,
        nullptr,
        resize_transition_publication);

    // A forced release must not show a partly placed image, so a pending
    // placement completes first; the changes it held are released with it.
    if (m_sixel_placement.has_value()) {
        advance_sixel_placement(result.actions);
        collect_synchronized_changes();
        if (!m_modes.synchronized_output) {
            release_synchronized_changes(publication);
        }
        m_sixel_placement_held = false;
    }
    set_synchronized_output_mode(false, &publication);
    assign_trailing_changes(resize_transition_publication, trailing_changes);

    result_change_overrides_t overrides;
    overrides.dirty_rows_have_stable_mutation_identity =
        publication.dirty_rows_have_stable_mutation_identity;
    adopt_publication_changes(std::move(publication));

    return finalize_result(std::move(result), overrides);
}

void Terminal_screen_model::apply_action(const Parser_action& action)
{
    std::vector<Parser_action> generated_actions;
    apply_action(action, generated_actions, nullptr);
    advance_primary_repaint_recovery_resize_guard();
    refresh_active_grid_retained_lookup_indexes();
}

void Terminal_screen_model::apply_action(
    const Parser_action&           action,
    std::vector<Parser_action>&    generated_actions,
    ingest_publication_t*          publication)
{
    const Parser_action_kind kind = parser_action_kind(action);
    if (kind == Parser_action_kind::STYLE_MUTATION) {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_screen_model::apply_action::style_mutation");
        apply_sgr_sequence(std::get<Terminal_sgr_sequence>(action.payload));
        return;
    }

    if (kind == Parser_action_kind::TERMINAL_QUERY) {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_screen_model::apply_action::terminal_query");
        const Terminal_color_query& query = std::get<Terminal_color_query>(action.payload);
        if (query.kind != Terminal_color_query_kind::PALETTE_INDEX ||
            palette_index_is_valid(query.palette_index))
        {
            generated_actions.push_back(make_color_query_reply(query));
        }
        return;
    }

    if (kind == Parser_action_kind::NOTIFICATION) {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_screen_model::apply_action::notification");
        const Parser_notification& notification =
            std::get<Parser_notification>(action.payload);
        if (notification.kind == Parser_notification_kind::TEXT_AREA_RESIZE_REQUESTED) {
            apply_grid_resize({notification.rows, notification.columns}, false);
        }
        return;
    }

    if (kind == Parser_action_kind::CONTROL_SEQUENCE) {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_screen_model::apply_action::control_sequence");
        apply_control_sequence(
            std::get<Parser_control_sequence>(action.payload),
            generated_actions,
            publication);
        return;
    }

    if (kind != Parser_action_kind::SCREEN_MUTATION) {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_screen_model::apply_action::ignored_action");
        return;
    }

    std::visit(
        [this, &generated_actions](const auto& mutation) {
            using mutation_t = std::decay_t<decltype(mutation)>;
            if constexpr (std::is_same_v<mutation_t, Screen_print_text_mutation>) {
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_screen_model::apply_action::print_text");
                if (mutation.printable_ascii_only) {
#if VNM_TERMINAL_PROFILING_ENABLED
                    if (m_profile_stats.enabled) {
                        ++m_profile_stats.print_text_calls;
                    }
#endif
                    put_printable_ascii_text(QStringView(mutation.text));
                }
                else {
                    put_text(mutation.text);
                }
            }
            else
            if constexpr (std::is_same_v<mutation_t, Screen_carriage_return_mutation>) {
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_screen_model::apply_action::carriage_return");
                carriage_return();
            }
            else
            if constexpr (std::is_same_v<mutation_t, Screen_line_feed_mutation>) {
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_screen_model::apply_action::line_feed");
                line_feed();
            }
            else
            if constexpr (std::is_same_v<mutation_t, Screen_backspace_mutation>) {
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_screen_model::apply_action::backspace");
                backspace();
            }
            else
            if constexpr (std::is_same_v<mutation_t, Screen_horizontal_tab_mutation>) {
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_screen_model::apply_action::horizontal_tab");
                horizontal_tab();
            }
            else
            if constexpr (std::is_same_v<mutation_t, Screen_set_title_mutation>) {
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_screen_model::apply_action::set_title");
                m_title = mutation.title;
            }
            else
            if constexpr (std::is_same_v<mutation_t, Screen_set_icon_name_mutation>) {
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_screen_model::apply_action::set_icon_name");
                m_icon_name = mutation.icon_name;
            }
            else
            if constexpr (std::is_same_v<mutation_t, Screen_set_hyperlink_mutation>) {
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_screen_model::apply_action::set_hyperlink");
                set_hyperlink(mutation.identity_key);
            }
            else
            if constexpr (std::is_same_v<mutation_t, Screen_bell_mutation>) {
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_screen_model::apply_action::bell");
                static_cast<void>(mutation);
            }
            else
            if constexpr (std::is_same_v<mutation_t, Screen_sixel_image_mutation>) {
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_screen_model::apply_action::sixel_image");
                place_sixel_image(mutation, generated_actions);
            }
            else {
                static_assert(
                    k_unhandled_screen_mutation<mutation_t>,
                    "Unhandled screen mutation");
            }
        },
        std::get<Screen_mutation>(action.payload));
}

void Terminal_screen_model::apply_control_sequence(
    const Parser_control_sequence& sequence,
    std::vector<Parser_action>&    generated_actions,
    ingest_publication_t*          publication)
{
    const auto unsupported = [&]() {
        generated_actions.push_back(make_unsupported_control_diagnostic(sequence));
    };

    const auto malformed = [&]() {
        generated_actions.push_back(make_malformed_recovery_diagnostic(
            source_name_for_family(sequence.family) + QStringLiteral(" parameter"),
            sequence.family,
            Parser_recovery_strategy::DISCARD_SEQUENCE));
    };

    if (sequence.action != Parser_control_sequence_action::DISPATCH) {
        unsupported();
        return;
    }

    if (sequence.family == Parser_sequence_family::ESC) {
        if (sequence.final_bytes == QByteArrayLiteral("D")) {
            line_feed();
            return;
        }
        if (sequence.final_bytes == QByteArrayLiteral("E")) {
            carriage_return();
            line_feed();
            return;
        }
        if (sequence.final_bytes == QByteArrayLiteral("M")) {
            reverse_index();
            return;
        }
        if (sequence.final_bytes == QByteArrayLiteral("7")) {
            save_cursor();
            return;
        }
        if (sequence.final_bytes == QByteArrayLiteral("8")) {
            restore_cursor();
            return;
        }
        if (sequence.final_bytes == QByteArrayLiteral("=")) {
            set_application_keypad_mode(true);
            return;
        }
        if (sequence.final_bytes == QByteArrayLiteral(">")) {
            set_application_keypad_mode(false);
            return;
        }

        unsupported();
        return;
    }

    if (sequence.family != Parser_sequence_family::CSI || sequence.final_bytes.size() != 1) {
        unsupported();
        return;
    }

    const bool has_no_prefix =
        sequence.private_marker.isEmpty() && sequence.intermediates.isEmpty();
    const unsigned char final_byte = static_cast<unsigned char>(sequence.final_bytes.front());

    if (has_no_prefix && sequence.payload.isEmpty() && final_byte == 'K') {
        // CSI K is the dominant control sequence in full-screen redraws. Its
        // omitted parameter means erase-in-line mode 0; handle that form
        // before constructing the general parameter-dispatch helpers.
        erase_in_line(0);
        return;
    }

    std::vector<Sgr_parameter_group> groups;
    const bool empty_parameter = sequence.payload.isEmpty();
    const auto parse_simple_parameters = [&]() {
        groups.clear();
        if (empty_parameter) {
            // An omitted CSI parameter is one default-valued parameter. Keep
            // that representation implicit: the common parameterless forms
            // (notably CSI K emitted by full-screen TUIs) do not need to
            // allocate a group and atom merely to recover their default.
            return true;
        }
        if (!parse_simple_csi_parameter_groups(sequence.payload, groups)) {
            malformed();
            return false;
        }
        return true;
    };

    const auto parameter_count = [&]() -> std::size_t {
        return empty_parameter ? 1U : groups.size();
    };

    const auto parameter_value = [&](std::size_t index, int default_value, int& value) {
        if (empty_parameter) {
            value = default_value;
            return true;
        }
        return csi_parameter_value(groups, index, default_value, value);
    };

    const auto single_parameter = [&](int default_value, int& value) {
        if (!parse_simple_parameters()) {
            return false;
        }
        if (parameter_count() > 1U) {
            malformed();
            return false;
        }
        return parameter_value(0U, default_value, value);
    };

    const auto single_count = [&](int& value) {
        if (!single_parameter(1, value)) {
            return false;
        }
        if (value < 1) {
            value = 1;
        }
        return true;
    };

    switch (final_byte) {
        case 'A':
        case 'B':
        case 'C':
        case 'D':
    {
                            int count = 1;
                            if (!has_no_prefix) {
                                malformed();
                                return;
                            }
                            if (!single_count(count)) {
                                return;
                            }

                            if (final_byte == 'A') { move_cursor_relative(-count, 0); } else
                            if (final_byte == 'B') { move_cursor_relative(count, 0);  } else
                            if (final_byte == 'C') { move_cursor_relative(0, count);  }
                            else {
                                move_cursor_relative(0, -count);
                            }
                            return;
        }
        case 'H':
        case 'f':
    {
                            if (!has_no_prefix) {
                                malformed();
                                return;
                            }
                            if (!parse_simple_parameters()) {
                                return;
                            }
                            if (parameter_count() > 2U) {
                                malformed();
                                return;
                            }

                            int row    = 1;
                            int column = 1;
                            if (!parameter_value(0U, 1, row) ||
                                !parameter_value(1U, 1, column))
                            {
                                malformed();
                                return;
                            }

                            if (row == 1 && column == 1) {
                                begin_primary_repaint_recovery_candidate();
                            }
                            set_cursor_address(row, column);
                            return;
        }
        case 'G':
    {
                            int column = 1;
                            if (!has_no_prefix) {
                                malformed();
                                return;
                            }
                            if (!single_parameter(1, column)) {
                                return;
                            }

                            set_cursor_position(m_cursor.row, std::max(column, 1) - 1);
                            return;
        }
        case 'J':
    {
                            int mode = 0;
                            if (!has_no_prefix) {
                                malformed();
                                return;
                            }
                            if (!single_parameter(0, mode)) {
                                return;
                            }
                            if (mode < 0 || mode > 3) {
                                unsupported();
                                return;
                            }

                            erase_in_display(mode);
                            return;
        }
        case 'K':
    {
                            int mode = 0;
                            if (!has_no_prefix) {
                                malformed();
                                return;
                            }
                            if (!single_parameter(0, mode)) {
                                return;
                            }
                            if (mode < 0 || mode > 2) {
                                unsupported();
                                return;
                            }

                            erase_in_line(mode);
                            return;
        }
        case 'X':
    {
                            int count = 1;
                            if (!has_no_prefix) {
                                malformed();
                                return;
                            }
                            if (!single_count(count)) {
                                return;
                            }

                            erase_characters(count);
                            return;
        }
        case '@':
        case 'P':
        case 'L':
        case 'M':
        case 'S':
        case 'T':
    {
                            // CSI ? Pi ; Pa ; Pv S is XTSMGRAPHICS, not a scroll. A
                            // geometry set carries two values, so four parameters at most.
                            if (final_byte == 'S' &&
                                sequence.private_marker == QByteArrayLiteral("?") &&
                                sequence.intermediates.isEmpty())
                            {
                                if (!parse_simple_parameters()) {
                                    return;
                                }

                                int item         = 0;
                                int action       = 0;
                                int first_value  = 0;
                                int second_value = 0;
                                if (parameter_count() > 4U               ||
                                    !parameter_value(0U, 0, item)        ||
                                    !parameter_value(1U, 0, action)      ||
                                    !parameter_value(2U, 0, first_value) ||
                                    !parameter_value(3U, 0, second_value))
                                {
                                    malformed();
                                    return;
                                }

                                generated_actions.push_back(graphics_attribute_reply(
                                    item,
                                    action,
                                    first_value,
                                    second_value));
                                return;
                            }

                            int count = 1;
                            if (!has_no_prefix) {
                                malformed();
                                return;
                            }
                            if (!parse_simple_parameters()) {
                                return;
                            }
                            if (parameter_count() > 1U) {
                                if (final_byte == 'T') {
                                    unsupported();
                                }
                                else {
                                    malformed();
                                }
                                return;
                            }
                            if (!parameter_value(0U, 1, count)) {
                                malformed();
                                return;
                            }
                            if (count < 1) {
                                count = 1;
                            }

                            if (final_byte == '@') { insert_cells(count); } else
                            if (final_byte == 'P') { delete_cells(count); } else
                            if (final_byte == 'L') { insert_lines(count); } else
                            if (final_byte == 'S') {
                                m_pending_wrap = false;
                                break_soft_wrap_before(m_scroll_bottom + 1);
                                scroll_up_region(
                                    m_scroll_top,
                                    m_scroll_bottom,
                                    m_active_buffer_id == Terminal_buffer_id::PRIMARY &&
                                        m_scroll_top == 0,
                                    count);
                            }
                            else
                            if (final_byte == 'T') {
                                m_pending_wrap = false;
                                scroll_down_region(m_scroll_top, m_scroll_bottom, count);
                            }
                            else {
                                delete_lines(count);
                            }
                            return;
        }
        case 'r':
    {
                            if (!has_no_prefix) {
                                malformed();
                                return;
                            }
                            if (!parse_simple_parameters()) {
                                return;
                            }
                            if (parameter_count() > 2U) {
                                malformed();
                                return;
                            }

                            int top    = 1;
                            int bottom = m_config.grid_size.rows;
                            if (!parameter_value(0U, 1, top) ||
                                !parameter_value(1U, m_config.grid_size.rows, bottom))
                            {
                                malformed();
                                return;
                            }

                            set_scroll_region(top, bottom);
                            return;
        }
        case 'g':
    {
                            int mode = 0;
                            if (!has_no_prefix) {
                                malformed();
                                return;
                            }
                            if (!single_parameter(0, mode)) {
                                return;
                            }
                            if (mode == 0) {
                                clear_current_tab_stop();
                                return;
                            }
                            if (mode == 3) {
                                clear_all_tab_stops();
                                return;
                            }

                            unsupported();
                            return;
        }
        case 's':
            if (has_no_prefix && sequence.payload.isEmpty()) {
                save_cursor();
                return;
            }
            unsupported();
            return;
        case 'u':
            if (has_no_prefix && sequence.payload.isEmpty()) {
                restore_cursor();
                return;
            }
            unsupported();
            return;
        case 'c':
    {
                            int mode = 0;
                            if (!parse_simple_parameters()) {
                                return;
                            }
                            if (parameter_count() > 1U || !parameter_value(0U, 0, mode)) {
                                malformed();
                                return;
                            }

                            if (sequence.intermediates.isEmpty() &&
                                sequence.private_marker.isEmpty() &&
                                mode == 0)
                            {
                                // Class 61 with attribute 4, sixel graphics (ctlseqs), claimed
                                // only while a cell pixel size lets images be placed.
                                generated_actions.push_back(make_da1_reply_action(
                                    m_config.cell_pixel_size.has_value()
                                        ? QByteArrayLiteral("\x1b[?61;4c")
                                        : QByteArrayLiteral("\x1b[?61c")));
                                return;
                            }
                            if (sequence.intermediates.isEmpty() &&
                                sequence.private_marker == QByteArrayLiteral(">") &&
                                mode                    == 0)
                            {
                                generated_actions.push_back(make_da2_reply_action(QByteArrayLiteral("\x1b[>0;0;0c")));
                                return;
                            }

                            unsupported();
                            return;
        }
        case 'n':
    {
                            int mode = 0;
                            if (!has_no_prefix) {
                                malformed();
                                return;
                            }
                            if (!single_parameter(0, mode)) {
                                return;
                            }
                            if (mode == 6) {
                                const int report_row = m_origin_mode
                                    ? m_cursor.row - m_scroll_top + 1
                                    : m_cursor.row + 1;
                                generated_actions.push_back(make_dsr_cursor_position_reply_action(
                                    report_row,
                                    m_cursor.column + 1));
                                return;
                            }

                            unsupported();
                            return;
        }
        case 't':
    {
                            if (!has_no_prefix) {
                                unsupported();
                                return;
                            }
                            if (!parse_simple_parameters()) {
                                return;
                            }

                            terminal_grid_size_t requested_grid_size;
                            switch (terminal_text_area_resize_request_status(
                                groups,
                                requested_grid_size))
                            {
                                case Terminal_text_area_resize_request_status::MALFORMED:
                                    malformed();
                                    return;
                                case Terminal_text_area_resize_request_status::UNSUPPORTED:
                                    unsupported();
                                    return;
                                case Terminal_text_area_resize_request_status::SUPPORTED:
                                    // The host owns its geometry right now, so the
                                    // text area cannot move. Ignoring it here keeps
                                    // the grid on the item; applying it and letting
                                    // the host revert would resize the pty twice per
                                    // request and never converge against a client
                                    // that re-asserts its size.
                                    if (m_config.text_area_resize_policy ==
                                        Terminal_text_area_resize_policy::DISABLED)
                                    {
                                        unsupported();
                                        return;
                                    }
                                    apply_grid_resize(requested_grid_size, false);
                                    generated_actions.push_back(
                                        make_text_area_resize_notification_action(
                                            requested_grid_size.rows,
                                            requested_grid_size.columns));
                                    return;
                                case Terminal_text_area_resize_request_status::NOT_REQUESTED:
                                    break;
                            }

                            int mode = 0;
                            if (!parameter_value(0U, 0, mode)) {
                                malformed();
                                return;
                            }
                            // Other multi-parameter lowercase t forms are xterm window-operation
                            // subcommands; reject them as unsupported, not as malformed CSI syntax.
                            if (parameter_count() != 1U) {
                                unsupported();
                                return;
                            }
                            if (mode == 18) {
                                generated_actions.push_back(make_text_area_size_reply_action(
                                    m_config.grid_size.rows,
                                    m_config.grid_size.columns));
                                return;
                            }
                            // The pixel reports need a known cell pixel size; without
                            // one they stay unsupported like the other window operations.
                            if (mode == 14 && m_config.cell_pixel_size.has_value()) {
                                generated_actions.push_back(make_text_area_pixel_size_reply_action(
                                    m_config.grid_size.rows    * m_config.cell_pixel_size->height,
                                    m_config.grid_size.columns * m_config.cell_pixel_size->width));
                                return;
                            }
                            if (mode == 16 && m_config.cell_pixel_size.has_value()) {
                                generated_actions.push_back(make_cell_pixel_size_reply_action(
                                    m_config.cell_pixel_size->height,
                                    m_config.cell_pixel_size->width));
                                return;
                            }

                            unsupported();
                            return;
        }
        case 'h':
        case 'l':
    {
                            if (sequence.private_marker != QByteArrayLiteral("?") ||
                                !sequence.intermediates.isEmpty())
                            {
                                unsupported();
                                return;
                            }
                            if (!parse_simple_parameters()) {
                                return;
                            }

                            for (std::size_t i = 0; i < parameter_count(); ++i) {
                                int mode = 0;
                                if (!parameter_value(i, 0, mode)) {
                                    malformed();
                                    return;
                                }
                                apply_dec_private_mode(
                                    mode,
                                    final_byte == 'h',
                                    generated_actions,
                                    sequence,
                                    publication);
                            }

                            if (parameter_count() > 0U) {
                                return;
                            }

                            unsupported();
                            return;
        }
        case 'p':
    {
                            int mode = 0;
                            if (sequence.private_marker != QByteArrayLiteral("?") ||
                                sequence.intermediates  != QByteArrayLiteral("$"))
                            {
                                unsupported();
                                return;
                            }
                            if (!single_parameter(0, mode)) {
                                return;
                            }
                            const int status = dec_private_mode_status(mode);
                            if (status != 0) {
                                generated_actions.push_back(make_decrqm_reply_action(
                                    mode,
                                    status));
                                return;
                            }

                            if (mode == 2027) {
                                generated_actions.push_back(make_private_mode_diagnostic(mode, sequence));
                                return;
                            }

                            unsupported();
                            return;
        }
        default:
            unsupported();
            return;
    }
}

// XTSMGRAPHICS (xterm ctlseqs) reads or sets item 1, the color register count,
// or item 2, the sixel geometry; ReGIS, item 3, is an error in Pi like any other
// item. Nothing here is settable: every image has 256 private registers and the
// geometry follows the text area and the decoded-size cap. So a reset reports the
// value in effect as a read does, the maximum is that value, and a set succeeds
// only when it asks for it. Without a cell pixel size no image can be placed, and
// the items fail as xterm's do when it is not configured for graphics.
Parser_action Terminal_screen_model::graphics_attribute_reply(
    int item,
    int action,
    int first_value,
    int second_value) const
{
    using Status = Terminal_graphics_attribute_status;

    constexpr int color_register_item = 1;
    constexpr int sixel_geometry_item = 2;
    constexpr int set_action          = 3;

    if (item != color_register_item && item != sixel_geometry_item) {
        return make_graphics_attribute_reply_action(item, Status::ITEM_ERROR);
    }
    // 1 reads, 2 resets, 3 sets and 4 reads the maximum.
    if (action < 1 || action > 4) {
        return make_graphics_attribute_reply_action(item, Status::ACTION_ERROR);
    }
    if (!m_config.cell_pixel_size.has_value()) {
        return make_graphics_attribute_reply_action(item, Status::FAILURE);
    }

    if (item == color_register_item) {
        if (action == set_action && first_value != k_sixel_color_register_count) {
            return make_graphics_attribute_reply_action(item, Status::FAILURE);
        }
        return make_graphics_attribute_reply_action(
            item,
            Status::SUCCESS,
            {k_sixel_color_register_count});
    }

    const QSize geometry = sixel_graphics_geometry(
        m_config.grid_size,
        *m_config.cell_pixel_size,
        sixel_raster_limit_bytes());
    if (action == set_action &&
        (first_value != geometry.width() || second_value != geometry.height()))
    {
        return make_graphics_attribute_reply_action(item, Status::FAILURE);
    }
    return make_graphics_attribute_reply_action(
        item,
        Status::SUCCESS,
        {geometry.width(), geometry.height()});
}

void Terminal_screen_model::apply_sgr_sequence(const Terminal_sgr_sequence& sequence)
{
    Terminal_text_style updated_style = m_current_style;
    for (const Terminal_sgr_operation& operation : sequence.operations) {
        apply_sgr_operation(updated_style, operation);
    }

    set_current_style(updated_style);
}

void Terminal_screen_model::apply_sgr_operation(
    Terminal_text_style&          style,
    const Terminal_sgr_operation& operation)
{
    switch (operation.kind) {
        case Terminal_sgr_operation_kind::RESET_ALL:
            style = make_default_terminal_text_style();
            break;
        case Terminal_sgr_operation_kind::SET_ATTRIBUTES:
            style.attributes |= operation.attributes;
            break;
        case Terminal_sgr_operation_kind::CLEAR_ATTRIBUTES:
            style.attributes &= static_cast<std::uint16_t>(~operation.attributes);
            break;
        case Terminal_sgr_operation_kind::SET_FOREGROUND:
            style.foreground = operation.color;
            break;
        case Terminal_sgr_operation_kind::SET_BACKGROUND:
            style.background = operation.color;
            break;
        default:
            throw std::invalid_argument("invalid terminal SGR operation kind");
    }
}

void Terminal_screen_model::set_current_style(const Terminal_text_style& style)
{
    const terminal_text_style_lookup_key_t key = terminal_text_style_lookup_key(style);
    const auto found = m_style_ids_by_value.find(key);
    if (found != m_style_ids_by_value.end()) {
        m_current_style    = style;
        m_current_style_id = found->second;
        return;
    }

    if (m_styles.size() >= m_next_style_compaction_count) {
        compact_styles(style);
        return;
    }

    if (m_styles.size() >= m_style_count_cap) {
        throw std::overflow_error("terminal style table capacity exhausted");
    }

    const Terminal_style_id style_id = static_cast<Terminal_style_id>(m_styles.size());
    const auto insertion = m_style_ids_by_value.emplace(key, style_id);
    Q_ASSERT(insertion.second);
    try {
        m_styles.push_back(style);
    }
    catch (...) {
        m_style_ids_by_value.erase(insertion.first);
        throw;
    }
    m_style_table_stats.current_style_count = static_cast<std::uint64_t>(m_styles.size());
    m_style_table_stats.peak_style_count = std::max(
        m_style_table_stats.peak_style_count,
        m_style_table_stats.current_style_count);
    m_current_style    = style;
    m_current_style_id = style_id;
}

void Terminal_screen_model::compact_styles(const Terminal_text_style& pending_style)
{
    const std::size_t old_style_count = m_styles.size();
    std::vector<std::uint8_t> referenced(old_style_count, 0U);

    const auto collect_style_id = [
            this,
            &referenced
        ](
            Terminal_style_id style_id)
        {
            const std::size_t style_index = static_cast<std::size_t>(style_id);
            if (style_index >= m_styles.size()) {
                throw std::runtime_error("terminal style compaction found invalid live style id");
            }
            referenced[style_index] = 1U;
        };

    const auto collect_rows = [
            &collect_style_id
        ](
            const std::vector<Terminal_screen_row>& rows)
        {
            for (const Terminal_screen_row& row : rows) {
                for (const Cell& cell : row.cells) {
                    collect_style_id(cell.style_id);
                }
            }
        };

    collect_style_id(k_default_terminal_style_id);
    collect_style_id(m_saved_cursor.style_id);
    collect_style_id(m_primary_backing.active_grid_state().saved_cursor.style_id);
    collect_style_id(m_alternate_grid.active_grid_state().saved_cursor.style_id);
    collect_rows(m_primary_backing.active_grid_state().rows);
    collect_rows(m_alternate_grid.active_grid_state().rows);
    if (m_primary_repaint_recovery_candidate.active) {
        collect_rows(m_primary_repaint_recovery_candidate.rows);
    }

    constexpr Terminal_style_id k_invalid_style_id =
        std::numeric_limits<Terminal_style_id>::max();
    std::vector<Terminal_style_id> remap(old_style_count, k_invalid_style_id);
    std::vector<Terminal_text_style> compacted_styles;
    compacted_styles.reserve(std::min(old_style_count + 1U, m_style_count_cap));
    std::map<terminal_text_style_lookup_key_t, Terminal_style_id>
        compacted_ids_by_value;

    for (std::size_t old_index = 0; old_index < old_style_count; ++old_index) {
        if (referenced[old_index] == 0U) {
            continue;
        }

        const Terminal_text_style& live_style = m_styles[old_index];
        if (old_index == 0U && live_style != make_default_terminal_text_style()) {
            throw std::runtime_error("terminal style compaction lost default style zero");
        }

        const terminal_text_style_lookup_key_t live_key =
            terminal_text_style_lookup_key(live_style);
        const Terminal_style_id compacted_id =
            static_cast<Terminal_style_id>(compacted_styles.size());
        compacted_styles.push_back(live_style);
        const auto insertion = compacted_ids_by_value.emplace(live_key, compacted_id);
        if (!insertion.second) {
            throw std::runtime_error(
                "terminal style compaction found duplicate interned styles");
        }
        remap[old_index] = compacted_id;
    }

    const std::size_t retained_old_style_count = compacted_styles.size();
    const terminal_text_style_lookup_key_t pending_key =
        terminal_text_style_lookup_key(pending_style);
    if (compacted_styles.size() >= m_style_count_cap) {
        throw std::overflow_error("terminal style table capacity exhausted");
    }

    const Terminal_style_id pending_id =
        static_cast<Terminal_style_id>(compacted_styles.size());
    compacted_styles.push_back(pending_style);
    compacted_ids_by_value.emplace(pending_key, pending_id);

    const auto rewritten_style_id = [
            &remap
        ](
            Terminal_style_id old_id)
        {
            return remap[static_cast<std::size_t>(old_id)];
        };

    const auto rewrite_rows = [
            &rewritten_style_id
        ](
            std::vector<Terminal_screen_row>& rows)
        {
            for (Terminal_screen_row& row : rows) {
                for (Cell& cell : row.cells) {
                    cell.style_id = rewritten_style_id(cell.style_id);
                }
            }
        };

    m_saved_cursor.style_id = rewritten_style_id(m_saved_cursor.style_id);
    m_primary_backing.active_grid_state().saved_cursor.style_id = rewritten_style_id(
        m_primary_backing.active_grid_state().saved_cursor.style_id);
    m_alternate_grid.active_grid_state().saved_cursor.style_id = rewritten_style_id(
        m_alternate_grid.active_grid_state().saved_cursor.style_id);
    rewrite_rows(m_primary_backing.active_grid_state().rows);
    rewrite_rows(m_alternate_grid.active_grid_state().rows);
    if (m_primary_repaint_recovery_candidate.active) {
        rewrite_rows(m_primary_repaint_recovery_candidate.rows);
    }

    m_styles             = std::move(compacted_styles);
    m_style_ids_by_value = std::move(compacted_ids_by_value);
    m_current_style      = pending_style;
    m_current_style_id   = pending_id;
    validate_live_style_ids();

    const std::size_t styles_before_cap = m_style_count_cap - m_styles.size();
    m_next_style_compaction_count =
        m_style_compaction_threshold >= styles_before_cap
            ? m_style_count_cap
            : m_styles.size() + m_style_compaction_threshold;
    m_style_table_stats.current_style_count = static_cast<std::uint64_t>(m_styles.size());
    m_style_table_stats.peak_style_count = std::max(
        m_style_table_stats.peak_style_count,
        m_style_table_stats.current_style_count);
    ++m_style_table_stats.compaction_count;
    m_style_table_stats.reclaimed_styles +=
        static_cast<std::uint64_t>(old_style_count - retained_old_style_count);
}

void Terminal_screen_model::validate_live_style_ids() const
{
    const auto validate_style_id = [
            this
        ](
            Terminal_style_id style_id)
        {
            if (static_cast<std::size_t>(style_id) >= m_styles.size()) {
                throw std::runtime_error(
                    "terminal style compaction produced invalid live style id");
            }
        };

    const auto validate_rows = [
            &validate_style_id
        ](
            const std::vector<Terminal_screen_row>& rows)
        {
            for (const Terminal_screen_row& row : rows) {
                for (const Cell& cell : row.cells) {
                    validate_style_id(cell.style_id);
                }
            }
        };

    validate_style_id(m_current_style_id);
    validate_style_id(m_saved_cursor.style_id);
    validate_style_id(m_primary_backing.active_grid_state().saved_cursor.style_id);
    validate_style_id(m_alternate_grid.active_grid_state().saved_cursor.style_id);
    validate_rows(m_primary_backing.active_grid_state().rows);
    validate_rows(m_alternate_grid.active_grid_state().rows);
    if (m_primary_repaint_recovery_candidate.active) {
        validate_rows(m_primary_repaint_recovery_candidate.rows);
    }

    if (m_style_ids_by_value.size() != m_styles.size()) {
        throw std::runtime_error("terminal style compaction produced incomplete lookup index");
    }
    for (std::size_t style_index = 0; style_index < m_styles.size(); ++style_index) {
        const auto found = m_style_ids_by_value.find(
            terminal_text_style_lookup_key(m_styles[style_index]));
        if (found == m_style_ids_by_value.end() ||
            found->second != static_cast<Terminal_style_id>(style_index))
        {
            throw std::runtime_error("terminal style compaction produced invalid lookup index");
        }
    }
}

Parser_action Terminal_screen_model::make_color_query_reply(
    const Terminal_color_query& query) const
{
    switch (query.kind) {
        case Terminal_color_query_kind::DEFAULT_FOREGROUND:
            return make_osc_query_reply_action(
                QByteArrayLiteral("\x1b]10;") +
                    color_reply_payload(m_color_state.default_foreground_rgba) +
                    QByteArrayLiteral("\x1b\\"),
                query.source_sequence);
        case Terminal_color_query_kind::DEFAULT_BACKGROUND:
            return make_osc_query_reply_action(
                QByteArrayLiteral("\x1b]11;") +
                    color_reply_payload(m_color_state.default_background_rgba) +
                    QByteArrayLiteral("\x1b\\"),
                query.source_sequence);
        case Terminal_color_query_kind::CURSOR:
            return make_osc_query_reply_action(
                QByteArrayLiteral("\x1b]12;") +
                    color_reply_payload(m_color_state.cursor_rgba) +
                    QByteArrayLiteral("\x1b\\"),
                query.source_sequence);
        case Terminal_color_query_kind::PALETTE_INDEX:
    {
                            const std::size_t palette_index = static_cast<std::size_t>(query.palette_index);
                            return make_osc_query_reply_action(
                                QByteArrayLiteral("\x1b]4;") + QByteArray::number(query.palette_index) + ';' +
                                    color_reply_payload(m_color_state.palette_rgba[palette_index]) +
                                    QByteArrayLiteral("\x1b\\"),
                                query.source_sequence);
        }
    }

    return make_osc_query_reply_action({}, query.source_sequence);
}

Parser_action Terminal_screen_model::make_unsupported_control_diagnostic(
    const Parser_control_sequence& sequence) const
{
    return
        make_unsupported_sequence_diagnostic(
            source_name_for_family(sequence.family),
            sequence.family,
            static_cast<std::size_t>(sequence.raw_bytes.size()),
            k_control_sequence_pending_limit_bytes,
            Parser_recovery_strategy::DISCARD_SEQUENCE);
}

Parser_action Terminal_screen_model::make_private_mode_diagnostic(
    int                            mode,
    const Parser_control_sequence& sequence) const
{
    return
        make_unsupported_sequence_diagnostic(
            QStringLiteral("DEC private mode ?") + QString::number(mode),
            sequence.family,
            static_cast<std::size_t>(sequence.raw_bytes.size()),
            k_control_sequence_pending_limit_bytes,
            Parser_recovery_strategy::DISCARD_SEQUENCE);
}

Terminal_selection_result Terminal_screen_model::selected_text(
    const Terminal_selection_range& selection) const
{
    const terminal_grid_position_t start = normalized_selection_start(selection);
    const terminal_grid_position_t end   = normalized_selection_end(selection);
    const std::size_t row_count = normalized_selection_row_count(selection);
    if (row_count == 0U) {
        return {Terminal_selection_result_code::INVALID_RANGE, {}};
    }

    std::vector<int> logical_rows;
    logical_rows.reserve(row_count);
    for (int logical_row = start.row; logical_row <= end.row; ++logical_row) {
        logical_rows.push_back(logical_row);
    }

    return selected_text_from_logical_rows(
        m_active_buffer_id,
        selection,
        std::span<const int>(logical_rows.data(), logical_rows.size()));
}

Terminal_selection_result Terminal_screen_model::selected_text(
    Terminal_buffer_id                                buffer_id,
    const Terminal_selection_range&                   selection,
    std::span<const terminal_selection_line_lease_t>  descriptors) const
{
    std::vector<int> logical_rows;
    if (!selection_line_lease_logical_rows(
            buffer_id,
            selection,
            descriptors,
            logical_rows))
    {
        return {Terminal_selection_result_code::INVALID_RANGE, {}};
    }

    return selected_text_from_logical_rows(
        buffer_id,
        selection,
        std::span<const int>(logical_rows.data(), logical_rows.size()));
}

std::vector<terminal_selection_line_lease_t> Terminal_screen_model::selection_line_leases(
    Terminal_buffer_id               buffer_id,
    const Terminal_selection_range&  range) const
{
    std::vector<terminal_selection_line_lease_t> descriptors;
    const terminal_grid_position_t start = normalized_selection_start(range);
    const terminal_grid_position_t end   = normalized_selection_end(range);
    const std::size_t row_count = normalized_selection_row_count(range);
    if (row_count == 0U) {
        return descriptors;
    }

    descriptors.reserve(row_count);
    for (int logical_row = start.row; logical_row <= end.row; ++logical_row) {
        const std::optional<terminal_history_handle_t> handle =
            retained_history_handle_at_logical_row(buffer_id, logical_row);
        if (!handle.has_value()) {
            return {};
        }

        descriptors.push_back({
            logical_row - start.row,
            *handle,
        });
    }
    return descriptors;
}

Terminal_selection_result Terminal_screen_model::selected_text_from_logical_rows(
    Terminal_buffer_id               buffer_id,
    const Terminal_selection_range&  selection,
    std::span<const int>             logical_rows) const
{
    const terminal_grid_position_t start = normalized_selection_start(selection);
    const terminal_grid_position_t end   = normalized_selection_end(selection);
    const std::size_t row_count = normalized_selection_row_count(selection);
    if (row_count == 0U || logical_rows.size() != row_count) {
        return {Terminal_selection_result_code::INVALID_RANGE, {}};
    }

    QStringList selected_rows;
    for (std::size_t index = 0U; index < logical_rows.size(); ++index) {
        const int logical_row = logical_rows[index];
        const std::optional<std::vector<Cell>> row_cells =
            logical_row_cells(buffer_id, logical_row);
        if (!row_cells.has_value()) {
            return {Terminal_selection_result_code::INVALID_RANGE, {}};
        }

        const int first_column = index == 0U ? start.column : 0;
        const int end_column =
            index + 1U == logical_rows.size() ? end.column : m_config.grid_size.columns;
        if (first_column < 0 || first_column > m_config.grid_size.columns ||
            end_column   < 0 || end_column   > m_config.grid_size.columns ||
            end_column   < first_column)
        {
            return {Terminal_selection_result_code::INVALID_RANGE, {}};
        }

        selected_rows.push_back(
            row_text_from_cells(*row_cells, first_column, end_column));
    }

    return {Terminal_selection_result_code::OK, selected_rows.join(QLatin1Char('\n'))};
}

QString Terminal_screen_model::visible_text() const
{
    QString text;
    for (int row = 0; row < m_config.grid_size.rows; ++row) {
        if (row > 0) {
            text += QChar(u'\n');
        }
        text += row_text(row);
    }
    return text;
}

QString Terminal_screen_model::row_text(int row) const
{
    if (row < 0 || row >= m_config.grid_size.rows) {
        return {};
    }

    QString text;
    for (const Cell& cell : active_grid_rows()[row].cells) {
        text += cell.text;
    }

    while (!text.isEmpty() && text.back() == QChar(u' ')) {
        text.chop(1);
    }

    return text;
}

terminal_grid_position_t Terminal_screen_model::cursor_position() const
{
    return m_cursor;
}

terminal_grid_size_t Terminal_screen_model::grid_size() const
{
    return m_config.grid_size;
}

Terminal_buffer_id Terminal_screen_model::active_buffer_id() const
{
    return m_active_buffer_id;
}

const Terminal_mode_state& Terminal_screen_model::mode_state() const
{
    return m_modes;
}

Terminal_input_mode_state Terminal_screen_model::input_mode_state() const
{
    Terminal_input_mouse_tracking_mode mouse_tracking =
        Terminal_input_mouse_tracking_mode::NONE;
    switch (m_modes.mouse_tracking) {
        case Terminal_mouse_tracking_mode::NONE:
            mouse_tracking = Terminal_input_mouse_tracking_mode::NONE;
            break;
        case Terminal_mouse_tracking_mode::BUTTON:
            mouse_tracking = Terminal_input_mouse_tracking_mode::NORMAL;
            break;
        case Terminal_mouse_tracking_mode::DRAG:
            mouse_tracking = Terminal_input_mouse_tracking_mode::BUTTON_EVENT;
            break;
        case Terminal_mouse_tracking_mode::ANY:
            mouse_tracking = Terminal_input_mouse_tracking_mode::ANY_EVENT;
            break;
    }

    return {
        .application_cursor_keys = m_modes.application_cursor_keys,
        .application_keypad      = m_application_keypad,
        .mouse_tracking          = mouse_tracking,
        .sgr_mouse_encoding      = m_modes.sgr_mouse_encoding,
        .bracketed_paste         = m_modes.bracketed_paste,
    };
}

const QString& Terminal_screen_model::title() const
{
    return m_title;
}

const QString& Terminal_screen_model::icon_name() const
{
    return m_icon_name;
}

int Terminal_screen_model::scrollback_size() const
{
    return m_primary_backing.retained_history_size();
}

bool Terminal_screen_model::retained_line_descriptors_match(
    Terminal_buffer_id                                buffer_id,
    const Terminal_selection_range&                   range,
    std::span<const terminal_selection_line_lease_t>  descriptors) const
{
    std::vector<int> logical_rows;
    return selection_line_lease_logical_rows(
        buffer_id,
        range,
        descriptors,
        logical_rows);
}

Terminal_retained_line_lookup_result Terminal_screen_model::retained_line_lookup(
    Terminal_buffer_id          buffer_id,
    terminal_history_handle_t   history_handle) const
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_screen_model::retained_line_lookup");

    if (m_selection_reconciliation_counters.enabled) {
        ++m_selection_reconciliation_counters.retained_line_lookup_requests;
    }

    if (!terminal_history_handle_has_identity(history_handle)) {
        return {};
    }

    Terminal_retained_line_lookup_result result;
    if (history_handle.epoch != k_terminal_history_retained_identity_epoch) {
        result.resolution_status = Terminal_history_resolution_status::STALE_EPOCH;
        return result;
    }

    result.resolution_status = Terminal_history_resolution_status::STALE_ROW_SEQUENCE;
    const retained_lookup_index_t& index = retained_lookup_index(buffer_id);

    const auto remember_neighbor =
        [&](const retained_lookup_index_entry_t& entry, bool successor) {
            const std::optional<int> logical_row =
                retained_lookup_index_entry_logical_row(buffer_id, entry);
            if (!logical_row.has_value() ||
                retained_lookup_index_entry_status(buffer_id, entry, *logical_row) !=
                    Terminal_history_resolution_status::OK)
            {
                return;
            }

            if (successor) {
                result.nearest_successor             = true;
                result.nearest_successor_logical_row = *logical_row;
            }
            else {
                result.nearest_predecessor             = true;
                result.nearest_predecessor_logical_row = *logical_row;
            }
        };

    const auto history_match =
        index.history_by_row_sequence.find(history_handle.row_sequence);
    const auto active_match =
        index.active_grid_by_row_sequence.find(history_handle.row_sequence);
    const int history_match_count = history_match == index.history_by_row_sequence.end()
        ? 0
        : history_match->second.history_match_count;
    const int active_match_count = active_match == index.active_grid_by_row_sequence.end()
        ? 0
        : active_match->second.active_grid_match_count;
    const retained_lookup_index_entry_t* exact_entry =
        history_match != index.history_by_row_sequence.end()
            ? &history_match->second
            : active_match != index.active_grid_by_row_sequence.end()
                ? &active_match->second
                : nullptr;
    if (exact_entry != nullptr) {
        const retained_lookup_index_entry_t& entry = *exact_entry;
        const std::optional<int> logical_row =
            retained_lookup_index_entry_logical_row(buffer_id, entry);
        if (logical_row.has_value() &&
            retained_lookup_index_entry_status(buffer_id, entry, *logical_row) ==
                Terminal_history_resolution_status::OK)
        {
            result.retained_line_id_found = true;
            result.retained_line_id_match_count =
                history_match_count + active_match_count;
            result.resolution_status =
                retained_history_handle_match_status(entry.history_handle, history_handle);
            if (result.resolution_status == Terminal_history_resolution_status::OK) {
                result.exact_match       = true;
                result.exact_logical_row = *logical_row;
            }
            else
            if (result.resolution_status ==
                Terminal_history_resolution_status::CONTENT_GENERATION_MISMATCH)
            {
                result.retained_line_content_generation_mismatch = true;
            }
        }
    }

    const retained_lookup_index_entry_t* successor_entry = nullptr;
    std::uint64_t successor_id = std::numeric_limits<std::uint64_t>::max();
    const auto remember_successor_candidate =
        [&](const auto& entries) {
            const auto found = entries.upper_bound(history_handle.row_sequence);
            if (found != entries.end() && found->first < successor_id) {
                successor_id = found->first;
                successor_entry = &found->second;
            }
        };
    remember_successor_candidate(index.history_by_row_sequence);
    remember_successor_candidate(index.active_grid_by_row_sequence);
    if (successor_entry != nullptr) {
        remember_neighbor(*successor_entry, true);
    }

    const retained_lookup_index_entry_t* predecessor_entry = nullptr;
    std::uint64_t predecessor_id = 0U;
    bool has_predecessor = false;
    const auto remember_predecessor_candidate =
        [&](const auto& entries) {
            auto found = entries.lower_bound(history_handle.row_sequence);
            if (found == entries.begin()) {
                return;
            }
            --found;
            if (!has_predecessor || found->first > predecessor_id) {
                predecessor_id = found->first;
                predecessor_entry = &found->second;
                has_predecessor = true;
            }
        };
    remember_predecessor_candidate(index.history_by_row_sequence);
    remember_predecessor_candidate(index.active_grid_by_row_sequence);
    if (predecessor_entry != nullptr) {
        remember_neighbor(*predecessor_entry, false);
    }
    return result;
}

void Terminal_screen_model::clear_selection_cell_watch(bool drag_anchor)
{
    auto& watches = m_selection_cell_watches[drag_anchor ? 1U : 0U];
    for (const auto& [id, watch] : watches) {
        watch->m_intact = false;
    }
    watches.clear();
}

void Terminal_screen_model::watch_selection_cells(
    terminal_selection_visual_lease_t& lease, bool drag_anchor)
{
    clear_selection_cell_watch(drag_anchor);
    auto& watches = m_selection_cell_watches[drag_anchor ? 1U : 0U];
    const auto start = normalized_selection_start(lease.selected_range);
    const auto end   = normalized_selection_end(lease.selected_range);
    for (auto& line : lease.selected_lines) {
        line.cell_continuity.reset();
        const auto lookup = retained_line_lookup(lease.buffer_id, line.history_handle);
        if (!lookup.exact_match || lookup.retained_line_id_match_count != 1) {
            continue;
        }
        const int active_row = lookup.exact_logical_row -
            (lease.buffer_id == Terminal_buffer_id::PRIMARY ? scrollback_size() : 0);
        const auto& rows = lease.buffer_id == Terminal_buffer_id::PRIMARY
            ? primary_active_grid_rows() : alternate_active_grid_rows();
        // History rows cannot mutate. Only active rows need a gesture-owned watch.
        if (active_row < 0 || active_row >= (int)rows.size()) {
            continue;
        }
        const auto& row = rows[(std::size_t)active_row];
        const int logical_row = start.row + line.row_offset;
        int first = logical_row == start.row ? start.column : 0;
        int limit = logical_row == end.row ? end.column : lease.grid_size.columns;
        if (drag_anchor && first == limit) {
            first = std::clamp(first, 0, (int)row.cells.size() - 1);
            limit = first + 1;
        }
        if (first < 0 || limit > (int)row.cells.size() || first >= limit) {
            continue;
        }
        first = cell_base_column_in_row(row, first);
        const int last_base = cell_base_column_in_row(row, limit - 1);
        limit = std::max(limit, last_base + std::max(1, row.cells[(std::size_t)last_base].display_width));
        auto watch = std::make_shared<Selection_cell_continuity>();
        watch->m_row_id             = line.history_handle.row_sequence;
        watch->m_initial_generation = line.history_handle.content_generation;
        watch->m_first_column       = first;
        watch->m_end_column         = limit;
        line.cell_continuity = watch;
        watches.emplace(watch->m_row_id, std::move(watch));
    }
}

void Terminal_screen_model::record_selection_cell_mutation(
    const Terminal_screen_row& row, int first, int end)
{
    for (auto& watches : m_selection_cell_watches) {
        const auto found = watches.find(row.retained_line_provenance.retained_line_id);
        if (found == watches.end()) {
            continue;
        }
        auto& watch = *found->second;
        if (watch.m_intact && first < watch.m_end_column && end > watch.m_first_column) {
            watch.m_intact = false;
            watch.m_invalidated_generation = row.retained_line_provenance.content_generation + 1U;
        }
    }
}

void Terminal_screen_model::transfer_selection_cell_watches(
    std::uint64_t old_id, std::uint64_t new_id, std::uint64_t preserved_generation)
{
    for (auto& watches : m_selection_cell_watches) {
        auto entry = watches.extract(old_id);
        if (!entry.empty()) {
            entry.key() = new_id;
            entry.mapped()->m_row_id = new_id;
            // An exact repaint successor preserves the predecessor generation.
            // Discard physical rebuild mutations, but never an earlier mutation
            // that was already part of the predecessor's content.
            if (entry.mapped()->m_invalidated_generation > preserved_generation) {
                entry.mapped()->m_intact = true;
                entry.mapped()->m_invalidated_generation = 0U;
            }
            watches.insert(std::move(entry));
        }
    }
}

Terminal_retained_line_lookup_result Terminal_screen_model::selection_line_lookup(
    Terminal_buffer_id buffer_id,
    terminal_history_handle_t& handle,
    const std::shared_ptr<Selection_cell_continuity>& continuity) const
{
    auto lookup = retained_line_lookup(buffer_id, handle);
    if (!continuity) {
        return lookup;
    }
    if (!continuity->m_intact) {
        lookup.exact_match = false;
        lookup.retained_line_content_generation_mismatch = true;
        lookup.resolution_status = Terminal_history_resolution_status::CONTENT_GENERATION_MISMATCH;
        return lookup;
    }
    if (!lookup.retained_line_content_generation_mismatch ||
        lookup.retained_line_id_match_count != 1 ||
        continuity->m_row_id != handle.row_sequence ||
        continuity->m_initial_generation > handle.content_generation)
    {
        return lookup;
    }
    const auto& index = retained_lookup_index(buffer_id);
    auto found = index.history_by_row_sequence.find(handle.row_sequence);
    const retained_lookup_index_entry_t* entry = found != index.history_by_row_sequence.end()
        ? &found->second : nullptr;
    if (entry == nullptr) {
        const auto active = index.active_grid_by_row_sequence.find(handle.row_sequence);
        if (active != index.active_grid_by_row_sequence.end()) {
            entry = &active->second;
        }
    }
    if (entry != nullptr && entry->history_handle.content_generation >= handle.content_generation) {
        handle = entry->history_handle;
        lookup = retained_line_lookup(buffer_id, handle);
    }
    return lookup;
}

Terminal_selection_attachment_resolution
Terminal_screen_model::resolve_selection_attachment(
    const terminal_selection_visual_lease_t&  prior_lease,
    const terminal_selection_source_identity_t& target_source,
    const Terminal_screen_model_result&       publication) const
{
    Terminal_selection_attachment_resolution resolution;
    if (m_selection_reconciliation_counters.enabled) {
        ++m_selection_reconciliation_counters.attachment_resolver_requests;
    }

    const terminal_grid_position_t old_start =
        normalized_selection_start(prior_lease.selected_range);
    const std::size_t selected_row_count =
        normalized_selection_row_count(prior_lease.selected_range);
    if (prior_lease.selected_range.mode == Terminal_selection_mode::NONE ||
        selected_row_count == 0U ||
        prior_lease.selected_lines.size() != selected_row_count ||
        prior_lease.anchor != prior_lease.selected_range.start ||
        prior_lease.extent != prior_lease.selected_range.end)
    {
        return resolution;
    }

    if (prior_lease.buffer_id != target_source.buffer_id ||
        target_source.buffer_id != m_active_buffer_id)
    {
        resolution.status = Terminal_selection_attachment_resolution_status::BUFFER_MISMATCH;
        return resolution;
    }
    if (prior_lease.session_epoch != target_source.session_epoch) {
        resolution.status = Terminal_selection_attachment_resolution_status::EPOCH_MISMATCH;
        return resolution;
    }
    if (prior_lease.anchor_domain != target_source.anchor_domain) {
        resolution.status = Terminal_selection_attachment_resolution_status::SOURCE_MISMATCH;
        return resolution;
    }
    if (prior_lease.grid_reflow_basis != target_source.grid_reflow_basis) {
        resolution.status =
            Terminal_selection_attachment_resolution_status::GRID_REFLOW_MISMATCH;
        return resolution;
    }
    if (!is_terminal_screen_model_grid_size_supported(prior_lease.grid_size) ||
        prior_lease.grid_size.columns != target_source.grid_size.columns ||
        !grid_sizes_match(target_source.grid_size, m_config.grid_size))
    {
        resolution.status = Terminal_selection_attachment_resolution_status::GRID_SIZE_MISMATCH;
        return resolution;
    }

    const terminal_selection_continuity_capability_t* continuity = nullptr;
    if (publication.selection_continuity.has_value()) {
        if (publication.selection_continuity->version !=
            k_terminal_selection_continuity_capability_version)
        {
            resolution.status =
                Terminal_selection_attachment_resolution_status::CAPABILITY_UNAVAILABLE;
            return resolution;
        }
        continuity = &*publication.selection_continuity;
    }

    resolution.old_logical_rows.reserve(selected_row_count);
    resolution.final_logical_rows.reserve(selected_row_count);
    resolution.row_deltas.reserve(selected_row_count);
    std::vector<terminal_selection_line_lease_t> final_lines;
    final_lines.reserve(selected_row_count);
    std::set<std::uint64_t> final_retained_line_ids;

    for (std::size_t index = 0U; index < selected_row_count; ++index) {
        if (m_selection_reconciliation_counters.enabled) {
            ++m_selection_reconciliation_counters.attachment_resolver_line_resolutions;
        }
        const terminal_selection_line_lease_t& old_line =
            prior_lease.selected_lines[index];
        if (old_line.row_offset != static_cast<int>(index) ||
            !terminal_history_handle_has_identity(old_line.history_handle))
        {
            resolution.status = Terminal_selection_attachment_resolution_status::INVALID_LEASE;
            return resolution;
        }

        const int old_logical_row = old_start.row + old_line.row_offset;
        terminal_history_handle_t final_handle = old_line.history_handle;
        const terminal_selection_line_successor_t* used_successor = nullptr;
        int         expected_successor_old_logical_row = old_logical_row;
        std::size_t successor_hops                     = 0U;
        Terminal_retained_line_lookup_result lookup = selection_line_lookup(
            prior_lease.buffer_id,
            final_handle,
            old_line.cell_continuity);
        // One ingest can accept more than one repaint recovery, and each one
        // replaces the handles the one before it published. A selected line can
        // therefore stand two or more exact replacements away from the handle
        // the lease holds while every link of that chain sits in this same
        // publication. Stopping after one link detaches a selection the
        // publication can still prove intact, so follow the chain instead. Each
        // link is taken on the terms a single link was already taken on, so an
        // ambiguous relation, a changed generation, a missing handle and a cycle
        // all still fail closed.
        while (!lookup.exact_match || lookup.retained_line_id_match_count != 1) {
            if (lookup.retained_line_id_match_count > 1) {
                resolution.status = Terminal_selection_attachment_resolution_status::
                    DUPLICATE_RESOLUTION;
                return resolution;
            }
            if (lookup.retained_line_content_generation_mismatch) {
                resolution.status = Terminal_selection_attachment_resolution_status::
                    CONTENT_GENERATION_MISMATCH;
                return resolution;
            }

            const terminal_selection_line_successor_t* successor = nullptr;
            if (continuity != nullptr) {
                if (m_selection_reconciliation_counters.enabled) {
                    ++m_selection_reconciliation_counters.successor_relation_lookups;
                }
                const auto found =
                    continuity->successors_by_old_retained_line_id.find(
                        final_handle.row_sequence);
                if (found !=
                    continuity->successors_by_old_retained_line_id.end())
                {
                    if (found->second.size() != 1U) {
                        resolution.status =
                            Terminal_selection_attachment_resolution_status::
                                DUPLICATE_RESOLUTION;
                        return resolution;
                    }
                    successor = &found->second.front();
                }
            }
            if (successor == nullptr) {
                resolution.status = Terminal_selection_attachment_resolution_status::MISSING_LINE;
                return resolution;
            }
            if (successor->old_handle != final_handle) {
                resolution.status = Terminal_selection_attachment_resolution_status::
                    CONTENT_GENERATION_MISMATCH;
                return resolution;
            }
            if (successor->final_handle.content_generation !=
                final_handle.content_generation)
            {
                resolution.status = Terminal_selection_attachment_resolution_status::
                    CONTENT_GENERATION_MISMATCH;
                return resolution;
            }

            // Each relation is keyed by the retained line id it replaces and a
            // usable key carries exactly one relation, so a walk longer than the
            // number of keys has already reused one. That is a cycle rather than
            // a longer history, and it needs no visited set to detect.
            ++successor_hops;
            if (successor_hops >
                continuity->successors_by_old_retained_line_id.size())
            {
                resolution.status = Terminal_selection_attachment_resolution_status::
                    DUPLICATE_RESOLUTION;
                return resolution;
            }

            // Only the last link carries rows normalized against the published
            // history, because finalize_selection_continuity_rows() can only
            // resolve a final handle that is still retained and an intermediate
            // link's final handle by definition is not. So an intermediate link
            // proves only that it continues from where the previous one landed;
            // the retained row itself is checked once, after the walk.
            //
            // The delta is redundant evidence: every producer writes it as
            // final_logical_row - old_logical_row, and the finalizer rewrites it
            // the same way. A link whose three row fields disagree is malformed
            // whichever one is right, so reject it here rather than pick the
            // field this consumer happens to read.
            if (successor->old_logical_row != expected_successor_old_logical_row ||
                successor->row_delta !=
                    successor->final_logical_row - successor->old_logical_row)
            {
                resolution.status = Terminal_selection_attachment_resolution_status::
                    INCONSISTENT_ROW_DELTA;
                return resolution;
            }

            expected_successor_old_logical_row = successor->final_logical_row;
            used_successor                     = successor;
            final_handle                       = successor->final_handle;
            lookup = selection_line_lookup(prior_lease.buffer_id, final_handle, old_line.cell_continuity);
        }

        if (used_successor != nullptr &&
            (used_successor->final_logical_row != lookup.exact_logical_row ||
             used_successor->row_delta !=
                 lookup.exact_logical_row - used_successor->old_logical_row))
        {
            resolution.status = Terminal_selection_attachment_resolution_status::
                INCONSISTENT_ROW_DELTA;
            return resolution;
        }

        if (!final_retained_line_ids.insert(final_handle.row_sequence).second) {
            resolution.status = Terminal_selection_attachment_resolution_status::
                DUPLICATE_RESOLUTION;
            return resolution;
        }

        resolution.old_logical_rows.push_back(old_logical_row);
        resolution.final_logical_rows.push_back(lookup.exact_logical_row);
        resolution.row_deltas.push_back(lookup.exact_logical_row - old_logical_row);
        final_lines.push_back({old_line.row_offset, final_handle, old_line.cell_continuity});
    }

    const int uniform_delta = resolution.row_deltas.front();
    for (std::size_t index = 1U; index < selected_row_count; ++index) {
        if (resolution.final_logical_rows[index] <=
            resolution.final_logical_rows[index - 1U])
        {
            resolution.status = Terminal_selection_attachment_resolution_status::
                REORDERED_RESOLUTION;
            return resolution;
        }
        if (resolution.final_logical_rows[index] !=
            resolution.final_logical_rows[index - 1U] + 1)
        {
            resolution.status = Terminal_selection_attachment_resolution_status::
                NONCONTIGUOUS_RESOLUTION;
            return resolution;
        }
        if (resolution.row_deltas[index] != uniform_delta) {
            resolution.status = Terminal_selection_attachment_resolution_status::
                INCONSISTENT_ROW_DELTA;
            return resolution;
        }
    }

    const auto endpoint_is_representable = [&](terminal_grid_position_t endpoint) {
        const int translated_row = endpoint.row + uniform_delta;
        return translated_row >= 0 &&
            endpoint.column >= 0 && endpoint.column <= target_source.grid_size.columns;
    };
    if (!endpoint_is_representable(prior_lease.selected_range.start) ||
        !endpoint_is_representable(prior_lease.selected_range.end)   ||
        !endpoint_is_representable(prior_lease.anchor)               ||
        !endpoint_is_representable(prior_lease.extent))
    {
        resolution.status = Terminal_selection_attachment_resolution_status::
            ENDPOINT_UNREPRESENTABLE;
        return resolution;
    }

    terminal_selection_visual_lease_t translated = prior_lease;
    translated.source_content_basis  = target_source.source_content_basis;
    translated.anchor_domain         = target_source.anchor_domain;
    translated.session_epoch         = target_source.session_epoch;
    translated.buffer_id             = target_source.buffer_id;
    translated.grid_reflow_basis     = target_source.grid_reflow_basis;
    translated.row_origin_generation = target_source.row_origin_generation;
    translated.grid_size             = target_source.grid_size;
    translated.viewport_mapping      = target_source.viewport_mapping;
    translated.selected_range.start.row += uniform_delta;
    translated.selected_range.end.row   += uniform_delta;
    translated.anchor.row                += uniform_delta;
    translated.extent.row                += uniform_delta;
    translated.selected_lines = std::move(final_lines);

    resolution.status = Terminal_selection_attachment_resolution_status::TRANSLATED;
    resolution.translated_lease = std::move(translated);
    return resolution;
}

std::optional<terminal_history_handle_t>
Terminal_screen_model::retained_history_handle_at_logical_row(
    Terminal_buffer_id buffer_id,
    int                logical_row) const
{
    return retained_lookup_cache_live_handle(buffer_id, logical_row);
}

void Terminal_screen_model::discard_retained_lookup_cache_for_testing() const
{
    // The retained index is owner-maintained and has no lazy cache to discard.
}

bool Terminal_screen_model::retained_history_storage_allocated_for_testing() const
{
    return m_primary_backing.retained_history.ring != nullptr;
}

void Terminal_screen_model::set_next_hyperlink_id_for_testing(Terminal_hyperlink_id id)
{
    m_next_hyperlink_id = id;
}

Terminal_hyperlink_id Terminal_screen_model::current_hyperlink_id_for_testing() const
{
    return m_current_hyperlink_id;
}

Terminal_hyperlink_id Terminal_screen_model::next_hyperlink_id_for_testing() const
{
    return m_next_hyperlink_id;
}

terminal_hyperlink_identity_by_id_t
Terminal_screen_model::active_hyperlink_identity_keys_by_id_for_testing() const
{
    return active_hyperlink_identity_keys_by_id();
}

void Terminal_screen_model::fail_next_hyperlink_compaction_allocation_for_testing(
    Terminal_hyperlink_compaction_allocation_phase phase)
{
    m_fail_next_hyperlink_compaction_allocation_phase_for_testing = phase;
}

void Terminal_screen_model::set_style_table_limits_for_testing(
    std::size_t compaction_threshold,
    std::size_t count_cap)
{
    if (compaction_threshold == 0U                         ||
        compaction_threshold >  count_cap                  ||
        count_cap            >  k_terminal_style_count_cap ||
        count_cap            <  m_styles.size())
    {
        throw std::invalid_argument("invalid terminal style table limits");
    }

    m_style_compaction_threshold    = compaction_threshold;
    m_style_count_cap               = count_cap;
    m_next_style_compaction_count   = compaction_threshold;
}

terminal_screen_model_style_table_stats_t Terminal_screen_model::style_table_stats() const
{
    return m_style_table_stats;
}

terminal_retained_history_diagnostics_t
Terminal_screen_model::retained_history_diagnostics() const
{
    const Retained_history_storage& retained = m_primary_backing.retained_history;

    terminal_retained_history_diagnostics_t diagnostics;
    diagnostics.byte_budget                          = retained.capacity_bytes;
    diagnostics.retained_rows                        = retained.index.size();
    diagnostics.retained_record_bytes                = retained.ring == nullptr
        ? 0U
        : retained.ring->head_byte_sequence() -
            retained.ring->oldest_live_byte_sequence();
    diagnostics.average_retained_row_bytes           = diagnostics.retained_rows == 0U
        ? 0.0
        : static_cast<double>(diagnostics.retained_record_bytes) /
            static_cast<double>(diagnostics.retained_rows);
    Q_ASSERT(retained.prefix_plain_ascii_rows <= diagnostics.retained_rows);
    diagnostics.payload_kind_generic_compact_rows =
        diagnostics.retained_rows - retained.prefix_plain_ascii_rows;
    diagnostics.payload_kind_prefix_plain_ascii_rows = retained.prefix_plain_ascii_rows;
    diagnostics.current_style_count                  = m_style_table_stats.current_style_count;
    diagnostics.peak_style_count                     = m_style_table_stats.peak_style_count;
    diagnostics.style_compaction_count               = m_style_table_stats.compaction_count;
    diagnostics.reclaimed_styles                     = m_style_table_stats.reclaimed_styles;
    diagnostics.hyperlink_compaction_count           = m_hyperlink_table_stats.compaction_count;
    diagnostics.reclaimed_hyperlink_ids =
        m_hyperlink_table_stats.reclaimed_hyperlink_ids;
    diagnostics.prefix_plain_ascii_estimate =
        make_terminal_history_prefix_plain_ascii_retention_estimate(
            diagnostics.byte_budget,
            m_config.grid_size.columns);
    return diagnostics;
}

void Terminal_screen_model::set_selection_reconciliation_counters_enabled(
    bool enabled) const
{
    m_selection_reconciliation_counters.enabled = enabled;
}

void Terminal_screen_model::reset_selection_reconciliation_counters() const
{
    const bool enabled = m_selection_reconciliation_counters.enabled;
    m_selection_reconciliation_counters = {};
    m_selection_reconciliation_counters.enabled = enabled;
}

terminal_selection_reconciliation_counters_t
Terminal_screen_model::selection_reconciliation_counters() const
{
    return m_selection_reconciliation_counters;
}

void Terminal_screen_model::refresh_active_grid_retained_lookup_index(
    Terminal_buffer_id buffer_id)
{
    retained_lookup_index_t& index = mutable_retained_lookup_index(buffer_id);
    if (m_selection_reconciliation_counters.enabled) {
        m_selection_reconciliation_counters.retained_lookup_active_grid_entries_visited +=
            index.active_grid_by_row_sequence.size();
    }
    index.active_grid_by_row_sequence.clear();

    const std::vector<Terminal_screen_row>& rows = buffer_id == Terminal_buffer_id::PRIMARY
        ? primary_active_grid_rows()
        : alternate_active_grid_rows();
    if (m_selection_reconciliation_counters.enabled) {
        m_selection_reconciliation_counters.retained_lookup_active_grid_entries_visited +=
            rows.size();
    }
    for (int active_row = 0; active_row < static_cast<int>(rows.size()); ++active_row) {
        const terminal_history_handle_t handle = retained_history_handle_from_provenance(
            rows[static_cast<std::size_t>(active_row)].retained_line_provenance);
        if (!terminal_history_handle_has_identity(handle)) {
            continue;
        }

        auto insertion = index.active_grid_by_row_sequence.emplace(
            handle.row_sequence,
            retained_lookup_index_entry_t{
                Retained_lookup_location_kind::ACTIVE_GRID,
                0U,
                active_row,
                handle,
                handle,
                0,
                1,
                1,
            });
        if (!insertion.second) {
            retained_lookup_index_entry_t& entry = insertion.first->second;
            ++entry.active_grid_match_count;
            ++entry.row_sequence_match_count;
        }
    }
}

void Terminal_screen_model::refresh_active_grid_retained_lookup_indexes()
{
    refresh_active_grid_retained_lookup_index(Terminal_buffer_id::PRIMARY);
    refresh_active_grid_retained_lookup_index(Terminal_buffer_id::ALTERNATE);
}

void Terminal_screen_model::insert_primary_history_retained_lookup(
    terminal_history_handle_t handle,
    std::uint64_t             history_ordinal)
{
    auto insertion = m_primary_retained_lookup_index.history_by_row_sequence.emplace(
        handle.row_sequence,
        retained_lookup_index_entry_t{
            Retained_lookup_location_kind::PRIMARY_HISTORY,
            history_ordinal,
            0,
            handle,
            {},
            1,
            0,
            1,
        });
    if (!insertion.second) {
        retained_lookup_index_entry_t& entry = insertion.first->second;
        entry.location_kind = Retained_lookup_location_kind::PRIMARY_HISTORY;
        entry.history_ordinal = history_ordinal;
        entry.history_handle = handle;
        ++entry.history_match_count;
        ++entry.row_sequence_match_count;
    }
}

void Terminal_screen_model::erase_retained_lookup_entry(
    Terminal_buffer_id buffer_id,
    std::uint64_t      retained_line_id)
{
    retained_lookup_index_t& index = mutable_retained_lookup_index(buffer_id);
    const auto found = index.history_by_row_sequence.find(retained_line_id);
    if (found == index.history_by_row_sequence.end()) {
        return;
    }
    retained_lookup_index_entry_t& entry = found->second;
    if (entry.history_match_count > 0) {
        --entry.history_match_count;
        --entry.row_sequence_match_count;
    }
    if (entry.row_sequence_match_count == 0) {
        index.history_by_row_sequence.erase(found);
    }
}

const Terminal_screen_model::retained_lookup_index_t&
Terminal_screen_model::retained_lookup_index(Terminal_buffer_id buffer_id) const
{
    return buffer_id == Terminal_buffer_id::PRIMARY
        ? m_primary_retained_lookup_index
        : m_alternate_retained_lookup_index;
}

Terminal_screen_model::retained_lookup_index_t&
Terminal_screen_model::mutable_retained_lookup_index(Terminal_buffer_id buffer_id)
{
    return buffer_id == Terminal_buffer_id::PRIMARY
        ? m_primary_retained_lookup_index
        : m_alternate_retained_lookup_index;
}

std::optional<terminal_history_handle_t>
Terminal_screen_model::retained_lookup_cache_live_handle(
    Terminal_buffer_id buffer_id,
    int                logical_row) const
{
    if (logical_row < 0) {
        return std::nullopt;
    }

    if (buffer_id == Terminal_buffer_id::PRIMARY) {
        if (logical_row < scrollback_size()) {
            return m_primary_backing.retained_history_handle_at_index(
                static_cast<std::size_t>(logical_row));
        }

        const std::optional<active_grid_row_t> active_row =
            active_grid_row_from_primary_backing(primary_backing_row_t{logical_row});
        if (!active_row.has_value()) {
            return std::nullopt;
        }

        const std::vector<Terminal_screen_row>& rows = primary_active_grid_rows();
        if (active_row->value < 0 ||
            active_row->value >= static_cast<int>(rows.size()))
        {
            return std::nullopt;
        }

        return retained_history_handle_from_provenance(
            rows[static_cast<std::size_t>(active_row->value)].retained_line_provenance);
    }

    const Terminal_screen_row* row = alternate_active_row(active_grid_row_t{logical_row});
    if (row == nullptr) {
        return std::nullopt;
    }

    return retained_history_handle_from_provenance(row->retained_line_provenance);
}

std::optional<int> Terminal_screen_model::retained_lookup_index_entry_logical_row(
    Terminal_buffer_id                   buffer_id,
    const retained_lookup_index_entry_t& entry) const
{
    if (entry.location_kind == Retained_lookup_location_kind::ACTIVE_GRID) {
        const int row_count = buffer_id == Terminal_buffer_id::PRIMARY
            ? static_cast<int>(primary_active_grid_rows().size())
            : static_cast<int>(alternate_active_grid_rows().size());
        if (entry.active_grid_row < 0 || entry.active_grid_row >= row_count) {
            return std::nullopt;
        }
        return buffer_id == Terminal_buffer_id::PRIMARY
            ? scrollback_size() + entry.active_grid_row
            : entry.active_grid_row;
    }

    if (buffer_id != Terminal_buffer_id::PRIMARY ||
        m_primary_backing.retained_history.index.empty())
    {
        return std::nullopt;
    }

    const std::uint64_t first_ordinal =
        m_primary_backing.retained_history.index.front().ordinal;
    if (entry.history_ordinal < first_ordinal) {
        return std::nullopt;
    }
    const std::uint64_t offset = entry.history_ordinal - first_ordinal;
    if (offset >= m_primary_backing.retained_history.index.size() ||
        offset > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    {
        return std::nullopt;
    }
    return static_cast<int>(offset);
}

Terminal_history_resolution_status
Terminal_screen_model::retained_lookup_index_entry_status(
    Terminal_buffer_id                   buffer_id,
    const retained_lookup_index_entry_t& entry,
    int                                  logical_row) const
{
    if (m_selection_reconciliation_counters.enabled) {
        ++m_selection_reconciliation_counters.retained_lookup_entry_revalidations;
    }
    const std::optional<terminal_history_handle_t> live_handle =
        retained_lookup_cache_live_handle(buffer_id, logical_row);
    if (!live_handle.has_value()) {
        return Terminal_history_resolution_status::STALE_ROW_SEQUENCE;
    }

    if (!terminal_history_handle_has_identity(*live_handle)) {
        return Terminal_history_resolution_status::STALE_ROW_SEQUENCE;
    }

    return retained_history_handle_match_status(*live_handle, entry.history_handle);
}

Terminal_retained_line_provenance Terminal_screen_model::retained_line_provenance_for_testing(
    Terminal_buffer_id buffer_id,
    int                logical_row) const
{
    if (logical_row < 0) {
        return {};
    }

    if (buffer_id == Terminal_buffer_id::PRIMARY) {
        const std::optional<Terminal_screen_row> row =
            primary_backing_row(primary_backing_row_t{logical_row});
        if (row.has_value()) {
            return row->retained_line_provenance;
        }
        return {};
    }

    const Terminal_screen_row* row = alternate_active_row(active_grid_row_t{logical_row});
    if (row != nullptr) {
        return row->retained_line_provenance;
    }
    return {};
}

std::vector<terminal_retained_line_content_origin_span_t>
Terminal_screen_model::retained_line_content_origin_spans_for_testing(
    Terminal_buffer_id buffer_id,
    int                logical_row) const
{
    if (logical_row < 0) {
        return {};
    }

    if (buffer_id == Terminal_buffer_id::PRIMARY) {
        const std::optional<Terminal_screen_row> row =
            primary_backing_row(primary_backing_row_t{logical_row});
        if (row.has_value()) {
            return row->content_origin_spans;
        }
        return {};
    }

    const Terminal_screen_row* row = alternate_active_row(active_grid_row_t{logical_row});
    if (row != nullptr) {
        return row->content_origin_spans;
    }
    return {};
}

void Terminal_screen_model::set_active_grid_retained_line_provenance_for_testing(
    Terminal_buffer_id                buffer_id,
    int                               active_grid_row,
    Terminal_retained_line_provenance provenance)
{
    std::vector<Terminal_screen_row>& rows = buffer_id == Terminal_buffer_id::PRIMARY
        ? m_primary_backing.active_grid_state().rows
        : m_alternate_grid.active_grid_state().rows;
    if (active_grid_row < 0 || active_grid_row >= static_cast<int>(rows.size())) {
        return;
    }
    Terminal_screen_row& row = rows[static_cast<std::size_t>(active_grid_row)];
    row.retained_line_provenance = provenance;
    row.content_origin_spans.clear();
    refresh_active_grid_retained_lookup_index(buffer_id);
}

std::optional<terminal_retained_row_record_metadata_t>
Terminal_screen_model::retained_row_record_metadata_for_testing(
    Terminal_buffer_id buffer_id,
    int                logical_row) const
{
    if (buffer_id != Terminal_buffer_id::PRIMARY ||
        logical_row < 0                         ||
        logical_row >= scrollback_size())
    {
        return std::nullopt;
    }

    const std::optional<retained_row_record_t> retained_record =
        m_primary_backing.materialize_retained_history_record(
            static_cast<std::size_t>(logical_row),
            Terminal_history_row_record_image_decode::SKIP_PIXELS);
    return retained_record.has_value()
        ? std::optional<terminal_retained_row_record_metadata_t>(retained_record->metadata)
        : std::nullopt;
}

std::optional<std::vector<terminal_retained_history_cell_state_for_testing_t>>
Terminal_screen_model::retained_history_row_cells_for_testing(
    Terminal_buffer_id buffer_id,
    int                logical_row) const
{
    if (buffer_id != Terminal_buffer_id::PRIMARY ||
        logical_row < 0                         ||
        logical_row >= scrollback_size())
    {
        return std::nullopt;
    }

    const std::optional<retained_row_record_t> retained_record =
        m_primary_backing.materialize_retained_history_record(
            static_cast<std::size_t>(logical_row),
            Terminal_history_row_record_image_decode::SKIP_PIXELS);
    if (!retained_record.has_value()) {
        return std::nullopt;
    }

    std::vector<terminal_retained_history_cell_state_for_testing_t> cells;
    cells.reserve(retained_record->row.cells.size());
    for (const Cell& cell : retained_record->row.cells) {
        terminal_retained_history_cell_state_for_testing_t state;
        state.text = cell.text;
        state.text_category = cell.text_category;
        state.display_width = cell.display_width;
        state.natural_display_width = cell.natural_display_width;
        state.wide_continuation = cell.wide_continuation;
        state.occupied = cell.occupied;
        state.style_id = cell.style_id;
        state.hyperlink_id = cell.hyperlink_id;
        cells.push_back(std::move(state));
    }
    return cells;
}

std::shared_ptr<const Terminal_image_slice> Terminal_screen_model::image_slice_for_testing(
    Terminal_buffer_id buffer_id,
    int                logical_row) const
{
    if (logical_row < 0) {
        return nullptr;
    }

    if (buffer_id == Terminal_buffer_id::ALTERNATE) {
        const Terminal_screen_row* row = alternate_active_row(active_grid_row_t{logical_row});
        return row != nullptr ? row->image_slice : nullptr;
    }

    if (logical_row < scrollback_size()) {
        const std::optional<retained_row_record_t> retained_record =
            m_primary_backing.materialize_retained_history_record(
                static_cast<std::size_t>(logical_row),
                Terminal_history_row_record_image_decode::DECODE_PIXELS);
        return retained_record.has_value() ? retained_record->row.image_slice : nullptr;
    }

    const std::optional<active_grid_row_t> active_row =
        active_grid_row_from_primary_backing(primary_backing_row_t{logical_row});
    return active_row.has_value()
        ? primary_active_grid_rows()[static_cast<std::size_t>(active_row->value)].image_slice
        : nullptr;
}

bool Terminal_screen_model::retained_line_descriptor_logical_row(
    Terminal_buffer_id              buffer_id,
    terminal_selection_line_lease_t descriptor,
    int&                            logical_row) const
{
    const Terminal_retained_line_lookup_result lookup =
        retained_line_lookup(buffer_id, descriptor.history_handle);
    if (!lookup.exact_match ||
        lookup.retained_line_id_match_count != 1)
    {
        return false;
    }

    logical_row = lookup.exact_logical_row;
    return true;
}

bool Terminal_screen_model::selection_line_lease_logical_rows(
    Terminal_buffer_id                                buffer_id,
    const Terminal_selection_range&                   range,
    std::span<const terminal_selection_line_lease_t>  descriptors,
    std::vector<int>&                                 logical_rows) const
{
    if (descriptors.size() != normalized_selection_row_count(range)) {
        return false;
    }

    if (descriptors.empty()) {
        return false;
    }

    const terminal_grid_position_t start = normalized_selection_start(range);
    logical_rows.clear();
    logical_rows.reserve(descriptors.size());
    for (std::size_t index = 0U; index < descriptors.size(); ++index) {
        const terminal_selection_line_lease_t descriptor = descriptors[index];
        if (descriptor.row_offset != static_cast<int>(index)) {
            return false;
        }

        int logical_row = 0;
        if (!retained_line_descriptor_logical_row(buffer_id, descriptor, logical_row)) {
            return false;
        }

        if (logical_row != start.row + descriptor.row_offset) {
            return false;
        }

        logical_rows.push_back(logical_row);
    }
    return true;
}

bool Terminal_screen_model::render_selection_request_logical_rows(
    const Terminal_render_selection_request& request,
    Terminal_buffer_id                       buffer_id,
    std::vector<int>&                        logical_rows) const
{
    return selection_line_lease_logical_rows(
        buffer_id,
        request.range,
        std::span<const terminal_selection_line_lease_t>(
            request.expected_lines.data(),
            request.expected_lines.size()),
        logical_rows);
}

void Terminal_screen_model::set_dirty_row_stats_enabled(bool enabled)
{
    m_dirty_row_stats            = {};
    m_dirty_row_timeline         = {};
    m_dirty_row_stats_start_time = std::chrono::steady_clock::now();
#if VNM_TERMINAL_PROFILING_ENABLED
    m_dirty_row_stats.enabled = enabled;
#else
    Q_UNUSED(enabled);
#endif
}

Terminal_screen_model_dirty_row_stats Terminal_screen_model::dirty_row_stats() const
{
    return m_dirty_row_stats;
}

Terminal_screen_model_dirty_row_timeline Terminal_screen_model::dirty_row_timeline() const
{
    return m_dirty_row_timeline;
}

void Terminal_screen_model::set_profile_stats_enabled(bool enabled)
{
#if VNM_TERMINAL_PROFILING_ENABLED
    m_profile_stats = {};
    m_profile_stats.enabled = enabled;
#else
    Q_UNUSED(enabled);
#endif
}

Terminal_screen_model_profile_stats Terminal_screen_model::profile_stats() const
{
    return m_profile_stats;
}

Terminal_screen_model::screen_buffer_state_t Terminal_screen_model::make_empty_buffer_state()
{
    screen_buffer_state_t state;
    resize_rows(state.rows, m_config.grid_size);
    state.scroll_top    = 0;
    state.scroll_bottom = m_config.grid_size.rows - 1;
    return state;
}

Terminal_screen_model::Retained_history_storage::Retained_history_storage() = default;

Terminal_screen_model::Retained_history_storage::~Retained_history_storage() = default;
Terminal_screen_model::Retained_history_storage&
Terminal_screen_model::Retained_history_storage::operator=(
    Retained_history_storage&&) = default;

void Terminal_screen_model::Retained_history_storage::ensure_allocated()
{
    if (ring != nullptr) {
        return;
    }

    ring = make_retained_history_ring(capacity_bytes);
}

void Terminal_screen_model::Retained_history_storage::reset()
{
    if (ring != nullptr) {
        ring->clear();
    }
    index.clear();
    prefix_plain_ascii_rows = 0U;
    next_ordinal = 0U;
}

terminal_history_ring_resize_result_t
Terminal_screen_model::Retained_history_storage::resize_capacity(
    std::size_t capacity_bytes)
{
    if (ring == nullptr) {
        this->capacity_bytes = capacity_bytes;
        return {};
    }

    const terminal_history_ring_resize_result_t result = ring->resize_capacity(
        capacity_bytes);
    if (result.status == Terminal_history_ring_status::OK) {
        this->capacity_bytes = ring->capacity_bytes();
    }
    return result;
}

void Terminal_screen_model::Retained_history_storage::
    track_record_in_reserved_index_slot(
        terminal_history_handle_t                history_handle,
        Terminal_history_row_record_payload_kind payload_kind,
        bool                                     has_image_section) noexcept
{
    Q_ASSERT(!index.empty());

    index.back().history_handle    = history_handle;
    index.back().payload_kind      = payload_kind;
    index.back().ordinal           = next_ordinal++;
    index.back().has_image_section = has_image_section;

    switch (payload_kind) {
        case Terminal_history_row_record_payload_kind::GENERIC_COMPACT:
            break;
        case Terminal_history_row_record_payload_kind::PREFIX_PLAIN_ASCII:
            ++prefix_plain_ascii_rows;
            break;
        default:
            std::terminate();
    }
}

void Terminal_screen_model::Retained_history_storage::discard_index_prefix(
    std::size_t record_count) const noexcept
{
    Q_ASSERT(record_count <= index.size());

    for (std::size_t entry_index = 0U; entry_index < record_count; ++entry_index) {
        const retained_history_index_entry_t& entry = index.front();

        switch (entry.payload_kind) {
            case Terminal_history_row_record_payload_kind::GENERIC_COMPACT:
                break;
            case Terminal_history_row_record_payload_kind::PREFIX_PLAIN_ASCII:
                --prefix_plain_ascii_rows;
                break;
            default:
                std::terminate();
        }

        index.pop_front();
    }
}

Terminal_screen_model::Primary_backing_buffer::Primary_backing_buffer() = default;
Terminal_screen_model::Primary_backing_buffer::~Primary_backing_buffer() = default;
Terminal_screen_model::Primary_backing_buffer&
Terminal_screen_model::Primary_backing_buffer::operator=(
    Primary_backing_buffer&&) = default;

Terminal_screen_model::screen_buffer_state_t&
Terminal_screen_model::Primary_backing_buffer::active_grid_state()
{
    return active_grid;
}

const Terminal_screen_model::screen_buffer_state_t&
Terminal_screen_model::Primary_backing_buffer::active_grid_state() const
{
    return active_grid;
}

bool Terminal_screen_model::Primary_backing_buffer::retained_history_empty() const
{
    return retained_history.index.empty();
}

int Terminal_screen_model::Primary_backing_buffer::retained_history_size() const
{
    if (retained_history.index.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        throw std::overflow_error("terminal retained history row count exceeds int range");
    }

    return static_cast<int>(retained_history.index.size());
}

std::optional<Terminal_screen_model::retained_row_record_t>
Terminal_screen_model::Primary_backing_buffer::materialize_retained_history_record(
    std::size_t                              index,
    Terminal_history_row_record_image_decode image_decode) const
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_screen_model::Primary_backing_buffer::materialize_retained_history_record");

    if (index >= retained_history.index.size()) {
        return std::nullopt;
    }

    const terminal_history_handle_t handle =
        retained_history.index[index].history_handle;
    const Terminal_history_ring_read_scope read =
        retained_history.ring->read_record_at_live_index(index, handle.byte_sequence);
    const Terminal_history_row_record_decode_result decoded =
        decode_terminal_history_row_record(read, image_decode, handle);
    if (decoded.status != Terminal_history_row_record_codec_status::OK) {
        throw_retained_history_storage_failure();
    }

    return Terminal_screen_model::retained_row_record_from_history_row_record(
        decoded.record);
}

std::optional<terminal_history_handle_t>
Terminal_screen_model::Primary_backing_buffer::retained_history_handle_at_index(
    std::size_t index) const
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_screen_model::Primary_backing_buffer::retained_history_handle_at_index");

    if (index >= retained_history.index.size()) {
        return std::nullopt;
    }

    return retained_history.index[index].history_handle;
}

Terminal_screen_model::retained_history_append_result_t
Terminal_screen_model::Primary_backing_buffer::append_retained_history_record(
    retained_row_record_t row)
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_screen_model::Primary_backing_buffer::append_retained_history_record");

    Terminal_history_row_record history_record =
        Terminal_screen_model::history_row_record_from_retained_record(row);

    terminal_history_row_record_identity_t identity;
    identity.epoch        = k_terminal_history_retained_identity_epoch;
    identity.row_sequence = history_record.provenance.retained_line_id;

    const auto record_is_oversized = [](const Terminal_history_row_record_append_result& append) {
        return
            append.status      == Terminal_history_row_record_codec_status::RING_RESERVE_FAILED &&
            append.ring_status == Terminal_history_ring_status::OVERSIZE_RECORD;
    };

    retained_history.ensure_allocated();
    retained_history.index.emplace_back();
    Terminal_history_row_record_append_result append;
    bool image_slice_dropped = false;
    try {
        append = encode_terminal_history_row_record_to_ring(
            *retained_history.ring,
            history_record,
            identity);
        // A row whose image leaves its record over the ring's record limit
        // keeps its text: the image is what makes it too large.
        if (record_is_oversized(append) && history_record.image_slice != nullptr) {
            history_record.image_slice.reset();
            image_slice_dropped = true;
            append = encode_terminal_history_row_record_to_ring(
                *retained_history.ring,
                history_record,
                identity);
        }
    }
    catch (...) {
        retained_history.index.pop_back();
        throw;
    }
    if (append.status != Terminal_history_row_record_codec_status::OK) {
        retained_history.index.pop_back();
        if (record_is_oversized(append)) {
            retained_history_append_result_t result;
            result.record_discarded = true;
            return result;
        }
        throw_retained_history_storage_failure();
    }

    retained_history.track_record_in_reserved_index_slot(
        append.history_handle,
        append.payload_kind,
        history_record.image_slice != nullptr);
    retained_history_append_result_t result;
    result.appended_handle     = append.history_handle;
    result.appended_ordinal    = retained_history.index.back().ordinal;
    result.image_slice_dropped = image_slice_dropped;
    if (append.commit.tail_advanced) {
        result.evicted_handles = prune_retained_history_rows_outside_live_window();
    }
    return result;
}

std::vector<terminal_history_handle_t>
Terminal_screen_model::Primary_backing_buffer::discard_oldest_retained_history_records(
    int row_count)
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_screen_model::Primary_backing_buffer::discard_oldest_retained_history_records");

    if (row_count <= 0 || retained_history.index.empty()) {
        return {};
    }

    const std::size_t rows_to_discard = std::min(
        static_cast<std::size_t>(row_count),
        retained_history.index.size());
    const terminal_history_ring_discard_result_t discard =
        retained_history.ring->discard_oldest_records(rows_to_discard);
    if (discard.status != Terminal_history_ring_status::OK) {
        throw_retained_history_storage_failure();
    }

    std::vector<terminal_history_handle_t> discarded_handles;
    discarded_handles.reserve(discard.discarded_records);
    for (std::size_t index = 0U; index < discard.discarded_records; ++index) {
        discarded_handles.push_back(retained_history.index[index].history_handle);
    }
    retained_history.discard_index_prefix(discard.discarded_records);

    if (discard.discarded_records >
        static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        throw std::overflow_error("terminal retained history discard exceeds int range");
    }

    return discarded_handles;
}

Terminal_screen_model::Retained_history_capacity_resize_result
Terminal_screen_model::Primary_backing_buffer::resize_retained_history_capacity(
    std::size_t capacity_bytes)
{
    // The ring keeps the newest records that fit and stops at the first one
    // over the new record limit, so a single image row over it would take all
    // older history with it. When an image is what makes a record too large,
    // the rows are re-encoded without that image instead.
    if (retained_history.ring != nullptr) {
        const std::size_t new_max_record_bytes = terminal_history_ring_max_record_bytes(
            terminal_history_ring_aligned_capacity(capacity_bytes));
        for (const auto& entry : retained_history.index) {
            if (entry.has_image_section &&
                entry.history_handle.record_bytes > new_max_record_bytes)
            {
                return rebuild_retained_history_without_oversized_images(capacity_bytes);
            }
        }
    }

    const terminal_history_ring_resize_result_t resize_result =
        retained_history.resize_capacity(capacity_bytes);
    if (resize_result.status != Terminal_history_ring_status::OK) {
        throw_retained_history_storage_failure();
    }

    if (retained_history.ring == nullptr ||
        resize_result.discarded_records == 0U)
    {
        return {};
    }

    return {prune_retained_history_rows_outside_live_window(), false};
}

Terminal_screen_model::Retained_history_capacity_resize_result
Terminal_screen_model::Primary_backing_buffer::rebuild_retained_history_without_oversized_images(
    std::size_t capacity_bytes)
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_screen_model::Primary_backing_buffer::rebuild_retained_history_without_oversized_images");

    struct Kept_record
    {
        std::size_t index      = 0U;
        bool        drop_image = false;
    };

    // Decodes one retained record. Only one is decoded at a time: a decoded
    // row is as wide as its source, whatever its compact record holds.
    const auto decode_record = [this](
        std::size_t                              index,
        Terminal_history_row_record_image_decode image_decode)
    {
        const terminal_history_handle_t handle = retained_history.index[index].history_handle;
        const Terminal_history_ring_read_scope read =
            retained_history.ring->read_record_at_live_index(index, handle.byte_sequence);
        Terminal_history_row_record_decode_result decoded =
            decode_terminal_history_row_record(read, image_decode, handle);
        if (decoded.status != Terminal_history_row_record_codec_status::OK) {
            throw_retained_history_storage_failure();
        }
        return std::move(decoded.record);
    };

    // Choose the kept rows as the ring would, newest first, stopping at the
    // first record still over the record limit or at the capacity, but with
    // each oversized image dropped from its row first. Record sizes come from
    // the index; only a row that loses its image is decoded, to size it.
    const std::size_t new_capacity = terminal_history_ring_aligned_capacity(capacity_bytes);
    const std::size_t new_max_record_bytes = terminal_history_ring_max_record_bytes(new_capacity);
    std::vector<Kept_record> kept;
    std::size_t kept_bytes = 0U;
    for (std::size_t index = retained_history.index.size(); index-- > 0U;) {
        const auto& entry = retained_history.index[index];
        std::size_t record_bytes = entry.history_handle.record_bytes;
        const bool drop_image =
            entry.has_image_section && record_bytes > new_max_record_bytes;
        if (drop_image) {
            const Terminal_history_row_record record =
                decode_record(index, Terminal_history_row_record_image_decode::DECODE_PIXELS);
            record_bytes -= terminal_history_row_record_image_section_bytes(*record.image_slice);
        }
        if (record_bytes > new_max_record_bytes ||
            kept_bytes   > new_capacity - record_bytes)
        {
            break;
        }

        kept.push_back({index, drop_image});
        kept_bytes += record_bytes;
    }

    // Re-encode oldest first into a ring of the new capacity, decoding each
    // row just before it is encoded. The kept records fit the ring together,
    // so no append evicts another; each keeps its ordinal and row sequence.
    // Until the swap below nothing the model reads has changed, so a failure
    // here leaves history as it was.
    std::unique_ptr<Terminal_history_ring> ring = make_retained_history_ring(new_capacity);
    std::vector<terminal_history_handle_t> kept_handles(kept.size());
    for (std::size_t kept_index = kept.size(); kept_index-- > 0U;) {
        const Kept_record& kept_record = kept[kept_index];
        const auto& entry = retained_history.index[kept_record.index];
        const Terminal_history_row_record record = decode_record(
            kept_record.index,
            entry.has_image_section && !kept_record.drop_image
                ? Terminal_history_row_record_image_decode::DECODE_PIXELS
                : Terminal_history_row_record_image_decode::SKIP_PIXELS);
        const Terminal_history_row_record_append_result append =
            encode_terminal_history_row_record_to_ring(
                *ring,
                record,
                {
                    k_terminal_history_retained_identity_epoch,
                    entry.history_handle.row_sequence,
                });
        if (append.status != Terminal_history_row_record_codec_status::OK ||
            append.commit.tail_advanced)
        {
            throw_retained_history_storage_failure();
        }
        kept_handles[kept_index] = append.history_handle;
    }

    const std::size_t dropped_rows = retained_history.index.size() - kept.size();
    Retained_history_capacity_resize_result result;
    result.retained_handles_replaced = true;
    result.evicted_handles.reserve(dropped_rows);
    for (std::size_t index = 0U; index < dropped_rows; ++index) {
        result.evicted_handles.push_back(retained_history.index[index].history_handle);
    }

    // The swap and the index updates below cannot throw.
    retained_history.ring           = std::move(ring);
    retained_history.capacity_bytes = retained_history.ring->capacity_bytes();
    for (std::size_t kept_index = 0U; kept_index < kept.size(); ++kept_index) {
        auto& entry = retained_history.index[kept[kept_index].index];
        entry.history_handle    = kept_handles[kept_index];
        entry.has_image_section = entry.has_image_section && !kept[kept_index].drop_image;
    }
    retained_history.discard_index_prefix(dropped_rows);
    return result;
}

void Terminal_screen_model::Primary_backing_buffer::clear_retained_history()
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_screen_model::Primary_backing_buffer::clear_retained_history");

    retained_history.reset();
}

std::vector<terminal_history_handle_t>
Terminal_screen_model::Primary_backing_buffer::
    prune_retained_history_rows_outside_live_window() const
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_screen_model::Primary_backing_buffer::prune_retained_history_rows_outside_live_window");

    const std::uint64_t oldest_live =
        retained_history.ring->oldest_live_byte_sequence();

    auto first_live = retained_history.index.begin();
    while (first_live != retained_history.index.end() &&
        first_live->history_handle.byte_sequence < oldest_live)
    {
        ++first_live;
    }

    const std::size_t pruned_rows = static_cast<std::size_t>(
        first_live - retained_history.index.begin());
    std::vector<terminal_history_handle_t> pruned_handles;
    pruned_handles.reserve(pruned_rows);
    for (std::size_t index = 0U; index < pruned_rows; ++index) {
        pruned_handles.push_back(retained_history.index[index].history_handle);
    }
    retained_history.discard_index_prefix(pruned_rows);

    if (pruned_rows > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::overflow_error("terminal retained history prune exceeds int range");
    }

    return pruned_handles;
}

Terminal_screen_model::screen_buffer_state_t&
Terminal_screen_model::Alternate_active_grid::active_grid_state()
{
    return active_grid;
}

const Terminal_screen_model::screen_buffer_state_t&
Terminal_screen_model::Alternate_active_grid::active_grid_state() const
{
    return active_grid;
}

void Terminal_screen_model::restore_buffer_state(const screen_buffer_state_t& state)
{
    const bool previous_origin_mode = m_origin_mode;
    active_grid_rows()  = state.rows;
    m_saved_cursor      = state.saved_cursor;
    m_cursor            = state.cursor;
    m_scroll_top        = state.scroll_top;
    m_scroll_bottom     = state.scroll_bottom;
    m_origin_mode       = state.origin_mode;
    m_modes.origin_mode = m_origin_mode;
    m_pending_wrap      = state.pending_wrap;
    if (m_origin_mode != previous_origin_mode) {
        mark_mode_state_changed();
    }
}

void Terminal_screen_model::save_active_buffer_state()
{
    // The selected buffer already owns active_grid_rows(). Save only the
    // working cursor/mode metadata; copying its rows would deep-copy the grid.
    screen_buffer_state_t& state = active_buffer_state();
    state.saved_cursor = m_saved_cursor;
    state.cursor       = m_cursor;
    state.scroll_top   = m_scroll_top;
    state.scroll_bottom = m_scroll_bottom;
    state.origin_mode  = m_origin_mode;
    state.pending_wrap = m_pending_wrap;
}

Terminal_screen_model::screen_buffer_state_t& Terminal_screen_model::active_buffer_state()
{
    return m_active_buffer_id == Terminal_buffer_id::PRIMARY
        ? m_primary_backing.active_grid_state()
        : m_alternate_grid.active_grid_state();
}

const Terminal_screen_model::screen_buffer_state_t&
Terminal_screen_model::active_buffer_state() const
{
    return m_active_buffer_id == Terminal_buffer_id::PRIMARY
        ? m_primary_backing.active_grid_state()
        : m_alternate_grid.active_grid_state();
}

std::vector<Terminal_screen_model::Terminal_screen_row>&
Terminal_screen_model::active_grid_rows()
{
    return active_buffer_state().rows;
}

const std::vector<Terminal_screen_model::Terminal_screen_row>&
Terminal_screen_model::active_grid_rows() const
{
    return active_buffer_state().rows;
}

void Terminal_screen_model::reflow_primary_rows(screen_buffer_state_t& state, int columns)
{
    // The packaged ConPTY runtime adopted in September 2026 reflows its active
    // screen without sending a resize repaint. Padding or clipping our rows leaves
    // subsequent cursor-addressed output on unrelated text. Reflow the active
    // primary rows and their cursors together; retained history remains outside
    // the backend-owned screen and keeps its original source-width projection.
    const terminal_grid_position_t old_cursor = state.cursor;
    const saved_cursor_state_t old_saved      = state.saved_cursor;
    const bool old_pending_wrap              = state.pending_wrap;
    std::vector<Terminal_screen_row> rows;
    rows.reserve(state.rows.size());
    const auto same_origin = [](
        const Terminal_retained_line_provenance& left,
        const Terminal_retained_line_provenance& right)
    {
        return left.retained_line_id == right.retained_line_id &&
            left.content_generation == right.content_generation &&
            left.source == right.source &&
            left.content_stamp_ms == right.content_stamp_ms &&
            left.content_stamp_is_unambiguous == right.content_stamp_is_unambiguous;
    };
    const auto append_origin = [&](
        Terminal_screen_row& row,
        int                 column,
        const Terminal_retained_line_provenance& origin)
    {
        if (!row.content_origin_spans.empty()) {
            terminal_retained_line_content_origin_span_t& previous =
                row.content_origin_spans.back();
            if (previous.first_column + previous.cell_count == column &&
                same_origin(previous.origin, origin))
            {
                ++previous.cell_count;
                return;
            }
        }

        row.content_origin_spans.push_back({
            .first_column = column,
            .cell_count   = 1,
            .origin       = origin,
        });
    };
    const auto finish_row_provenance = [](Terminal_screen_row& row)
    {
        if (row.content_origin_spans.empty()) {
            return;
        }

        const Terminal_retained_line_provenance& first_origin =
            row.content_origin_spans.front().origin;
        bool same_stamp = first_origin.content_stamp_is_unambiguous;
        bool same_generation = true;
        bool same_source = true;
        for (const terminal_retained_line_content_origin_span_t& span :
            row.content_origin_spans)
        {
            same_stamp = same_stamp && span.origin.content_stamp_is_unambiguous &&
                span.origin.content_stamp_ms == first_origin.content_stamp_ms;
            same_generation = same_generation &&
                span.origin.content_generation == first_origin.content_generation;
            same_source = same_source && span.origin.source == first_origin.source;
        }

        row.retained_line_provenance.content_stamp_ms =
            same_stamp ? first_origin.content_stamp_ms : 0;
        row.retained_line_provenance.content_stamp_is_unambiguous = same_stamp;
        row.retained_line_provenance.content_generation =
            same_generation ? first_origin.content_generation : 0U;
        row.retained_line_provenance.source = same_source
            ? first_origin.source
            : Terminal_retained_line_provenance_source::TERMINAL_STORAGE;
    };

    for (std::size_t first = 0; first < state.rows.size();) {
        std::size_t last = first;
        while (last + 1U < state.rows.size() && state.rows[last].soft_wrap_columns > 0) {
            ++last;
        }
        std::vector<Cell> cells;
        std::vector<Terminal_retained_line_provenance> cell_origins;
        std::optional<std::size_t> cursor_offset;
        std::optional<std::size_t> saved_offset;
        bool contains_latent_wide_cell = false;
        for (std::size_t index = first; index <= last; ++index) {
            const Terminal_screen_row& source = state.rows[index];
            int count = (int)source.cells.size();
            while (count > 0 && !source.cells[(std::size_t)count - 1U].occupied) {
                --count;
            }
            // Preserve written cells beyond an earlier wide-glyph wrap gap.
            count = std::max(count, source.soft_wrap_columns);
            // An early wide-glyph wrap can leave an unused right margin.
            // A cursor in that margin is a boundary in the logical line, not
            // another content cell to insert ahead of the next row. Keep the
            // occupied extent above, including actual writes into the margin.
            const bool joined_row = index < last;
            if (!joined_row && (int)index == old_cursor.row) {
                count = std::max(count, old_cursor.column + 1);
            }
            if (!joined_row && old_saved.valid &&
                (int)index == old_saved.position.row)
            {
                count = std::max(count, old_saved.position.column + 1);
            }

            std::size_t origin_span_index = 0U;
            const auto origin_at_column = [&](int column)
            {
                while (origin_span_index < source.content_origin_spans.size() &&
                    source.content_origin_spans[origin_span_index].first_column +
                        source.content_origin_spans[origin_span_index].cell_count <= column)
                {
                    ++origin_span_index;
                }
                if (origin_span_index < source.content_origin_spans.size()) {
                    const terminal_retained_line_content_origin_span_t& span =
                        source.content_origin_spans[origin_span_index];
                    if (column >= span.first_column &&
                        column < span.first_column + span.cell_count)
                    {
                        return span.origin;
                    }
                }
                return source.retained_line_provenance;
            };
            const int cursor_boundary = std::min(count,
                old_cursor.column + (old_pending_wrap ? 1 : 0));
            const int saved_boundary = std::min(count,
                old_saved.position.column + (old_saved.pending_wrap ? 1 : 0));
            const auto record_source_position = [&](int boundary, std::size_t offset)
            {
                if ((int)index == old_cursor.row && cursor_boundary == boundary) {
                    cursor_offset = offset;
                }
                if (old_saved.valid && (int)index == old_saved.position.row &&
                    saved_boundary == boundary)
                {
                    saved_offset = offset;
                }
            };

            // The flattened stream contains one logical slot for every cell
            // in each glyph's natural width. If a narrow source row stores
            // only the base cell, synthesize its continuation here so later
            // offsets advance in this logical stream, not across physical
            // source cells or destination columns.
            int source_column = 0;
            while (source_column < count) {
                const Cell& source_cell = source.cells[static_cast<std::size_t>(source_column)];
                const int stored_width = source_cell.wide_continuation
                    ? 1
                    : std::max(1, source_cell.display_width);
                const int physical_width = std::min(stored_width, count - source_column);
                const int natural_width = source_cell.wide_continuation
                    ? 1
                    : std::max(stored_width, source_cell.natural_display_width);
                const std::size_t logical_start = cells.size();
                const Terminal_retained_line_provenance origin =
                    origin_at_column(source_column);
                record_source_position(source_column, logical_start);

                Cell base_cell = source_cell;
                base_cell.display_width = natural_width;
                base_cell.natural_display_width = natural_width;
                base_cell.wide_continuation = false;
                cells.push_back(std::move(base_cell));
                cell_origins.push_back(origin);

                for (int part = 1; part < natural_width; ++part) {
                    Cell continuation;
                    if (part < physical_width &&
                        source_column + part < static_cast<int>(source.cells.size()) &&
                        source.cells[static_cast<std::size_t>(source_column + part)]
                            .wide_continuation)
                    {
                        continuation =
                            source.cells[static_cast<std::size_t>(source_column + part)];
                    }
                    continuation.text = {};
                    continuation.text_category =
                        Terminal_render_cell_text_category::EMPTY;
                    continuation.display_width = 0;
                    continuation.natural_display_width = 0;
                    continuation.wide_continuation = true;
                    continuation.occupied = true;
                    continuation.style_id = source_cell.style_id;
                    continuation.hyperlink_id = source_cell.hyperlink_id;
                    cells.push_back(std::move(continuation));
                    cell_origins.push_back(origin);
                }

                for (int part = 1; part < physical_width; ++part) {
                    record_source_position(
                        source_column + part,
                        logical_start + static_cast<std::size_t>(part));
                }
                record_source_position(
                    source_column + physical_width,
                    logical_start + static_cast<std::size_t>(natural_width));
                contains_latent_wide_cell = contains_latent_wide_cell ||
                    natural_width > physical_width;
                source_column += physical_width;
            }
            record_source_position(count, cells.size());
        }

        // Preserve the identity of a row whose written content does not move.
        if (first == last && !contains_latent_wide_cell &&
            cells.size() <= (std::size_t)columns)
        {
            Terminal_screen_row row = std::move(state.rows[first]);
            row.cells.resize((std::size_t)columns);
            row.soft_wrap_columns = 0;
            for (terminal_retained_line_content_origin_span_t& span :
                row.content_origin_spans)
            {
                span.cell_count = std::min(
                    span.cell_count,
                    std::max(0, columns - span.first_column));
            }
            row.content_origin_spans.erase(
                std::remove_if(
                    row.content_origin_spans.begin(),
                    row.content_origin_spans.end(),
                    [](const terminal_retained_line_content_origin_span_t& span)
                    {
                        return span.cell_count <= 0;
                    }),
                row.content_origin_spans.end());
            const int target_row = (int)rows.size();
            if (cursor_offset.has_value()) {
                state.cursor = {target_row, std::min((int)*cursor_offset, columns - 1)};
                state.pending_wrap = *cursor_offset == (std::size_t)columns;
            }
            if (saved_offset.has_value()) {
                state.saved_cursor.position = {target_row, std::min((int)*saved_offset, columns - 1)};
                state.saved_cursor.pending_wrap = *saved_offset == (std::size_t)columns;
            }
            rows.push_back(std::move(row));
            first = last + 1U;
            continue;
        }

        const auto make_row = [&](const Terminal_retained_line_provenance& origin) {
            Terminal_screen_row row;
            row.cells.resize((std::size_t)columns);
            row.retained_line_provenance = origin;
            rebase_retained_line_id_preserving_content(row, origin.source);
            return row;
        };
        const Terminal_retained_line_provenance first_origin = cell_origins.empty()
            ? state.rows[first].retained_line_provenance
            : cell_origins.front();
        Terminal_screen_row row = make_row(first_origin);
        // An image stays on the first row of its logical line (S2), which is
        // the only row of the line that can show one (D2). Its columns take
        // no part in the reflow, so widening back loses nothing of it.
        Q_ASSERT(std::none_of(
            state.rows.begin() + static_cast<std::ptrdiff_t>(first) + 1,
            state.rows.begin() + static_cast<std::ptrdiff_t>(last) + 1,
            [](const Terminal_screen_row& source_row) { return source_row.image_slice != nullptr; }));
        row.image_slice = std::move(state.rows[first].image_slice);
        int column = 0;
        bool cursor_pending_end_mapped = false;
        bool saved_pending_end_mapped = false;
        const auto map_position = [&](std::size_t offset, int target_column, bool pending_wrap) {
            if (cursor_offset == offset &&
                !(old_pending_wrap && cursor_pending_end_mapped))
            {
                state.cursor       = {(int)rows.size(), target_column};
                state.pending_wrap = pending_wrap;
            }
            if (saved_offset == offset &&
                !(old_saved.pending_wrap && saved_pending_end_mapped))
            {
                state.saved_cursor.position     = {(int)rows.size(), target_column};
                state.saved_cursor.pending_wrap = pending_wrap;
            }
        };
        for (std::size_t offset = 0; offset < cells.size();) {
            const int source_width = std::max(1, cells[offset].natural_display_width);
            const int width        = std::min(source_width, columns);
            if (column + width > columns) {
                row.soft_wrap_columns = column;
                finish_row_provenance(row);
                rows.push_back(std::move(row));
                row = make_row(cell_origins[offset]);
                column = 0;
            }
            for (int part = 0; part < source_width; ++part) {
                map_position(offset + part, column + std::min(part, width - 1), false);
                if (part < width) {
                    Cell output_cell = cells[offset + part];
                    output_cell.display_width = part == 0 ? width : 0;
                    output_cell.natural_display_width = part == 0 ? source_width : 0;
                    output_cell.wide_continuation = part > 0;
                    if (part > 0) {
                        output_cell.text = {};
                        output_cell.text_category =
                            Terminal_render_cell_text_category::EMPTY;
                    }
                    row.cells[static_cast<std::size_t>(column + part)] =
                        std::move(output_cell);
                    append_origin(row, column + part, cell_origins[offset + part]);
                }
            }
            const std::size_t end_offset = offset + static_cast<std::size_t>(source_width);
            const int         end_column = column + width;
            if (old_pending_wrap && cursor_offset == end_offset) {
                state.cursor = {
                    (int)rows.size(),
                    std::min(end_column, columns - 1),
                };
                state.pending_wrap = end_column == columns;
                cursor_pending_end_mapped = true;
            }
            if (old_saved.pending_wrap && saved_offset == end_offset) {
                state.saved_cursor.position = {
                    (int)rows.size(),
                    std::min(end_column, columns - 1),
                };
                state.saved_cursor.pending_wrap = end_column == columns;
                saved_pending_end_mapped = true;
            }
            column += width;
            offset = end_offset;
        }
        map_position(cells.size(), std::min(column, columns - 1), column == columns);
        finish_row_provenance(row);
        rows.push_back(std::move(row));
        first = last + 1U;
    }
    state.rows = std::move(rows);
}

void Terminal_screen_model::resize_buffer_state(
    screen_buffer_state_t& state,
    terminal_grid_size_t   grid_size,
    bool                   primary)
{
    const bool width_changed = state.rows.front().cells.size() != (std::size_t)grid_size.columns;
    if (primary && width_changed) {
        reflow_primary_rows(state, grid_size.columns);
    }
    else
    if (width_changed) {
        state.pending_wrap = false;
        state.saved_cursor.pending_wrap = false;
    }

    // Retained history is archival. Resize only the live screen and keep its
    // cursor visible; growing the screen does not pull archived rows back in.
    const int prefix_rows = std::max(0, state.cursor.row - grid_size.rows + 1);
    for (int row = 0; row < prefix_rows; ++row) {
        if (primary) {
            append_scrollback_row(state.rows[(std::size_t)row]);
        }
    }
    if (prefix_rows > 0) {
        state.rows.erase(state.rows.begin(), state.rows.begin() + prefix_rows);
        state.cursor.row -= prefix_rows;
        state.saved_cursor.position.row -= prefix_rows;
    }
    resize_rows(state.rows, grid_size);
    state.cursor.row    = std::clamp(state.cursor.row,    0, grid_size.rows - 1);
    state.cursor.column = std::clamp(state.cursor.column, 0, grid_size.columns - 1);
    if (state.saved_cursor.valid) {
        const bool saved_row_clamped = state.saved_cursor.position.row < 0 ||
            state.saved_cursor.position.row >= grid_size.rows;
        state.saved_cursor.position.row =
            std::clamp(state.saved_cursor.position.row, 0, grid_size.rows - 1);
        state.saved_cursor.position.column =
            std::clamp(state.saved_cursor.position.column, 0, grid_size.columns - 1);
        if (saved_row_clamped) {
            state.saved_cursor.pending_wrap = false;
        }
        state.saved_cursor.origin_mode = false;
    }
    state.scroll_top    = 0;
    state.scroll_bottom = grid_size.rows - 1;
    state.origin_mode   = false;
}

void Terminal_screen_model::resize_rows(
    std::vector<Terminal_screen_row>&    rows,
    terminal_grid_size_t               grid_size)
{
    const std::size_t old_row_count = rows.size();
    rows.resize(static_cast<std::size_t>(grid_size.rows));
    for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
        Terminal_screen_row& row = rows[row_index];
        const bool existing_retained_row =
            row_index < old_row_count &&
            row.retained_line_provenance.retained_line_id != 0U;
        // Existing rows are already valid. A height-only resize changes no
        // surviving cell and must not refresh that row's content generation.
        if (existing_retained_row &&
            row.cells.size() == static_cast<std::size_t>(grid_size.columns))
        {
            continue;
        }
        std::vector<Cell> before_cells;
        if (existing_retained_row) {
            before_cells = row.cells;
        }

        row.cells.resize(static_cast<std::size_t>(grid_size.columns));
        repair_wide_spans_in_row(row.cells, grid_size.columns);
        if (!existing_retained_row) {
            replace_retained_line_id(row);
            continue;
        }

        advance_row_content_generation_if_changed(row, before_cells);
    }
}

std::uint64_t Terminal_screen_model::next_retained_line_id()
{
    if (m_next_retained_line_id == 0U) {
        throw std::overflow_error("terminal retained line id space exhausted");
    }

    const std::uint64_t id = m_next_retained_line_id;
    ++m_next_retained_line_id;
    return id;
}

void Terminal_screen_model::replace_retained_line_id(
    Terminal_screen_row&                    row,
    Terminal_retained_line_provenance_source source)
{
    row.content_origin_spans.clear();
    row.retained_line_provenance = {
        .retained_line_id   = next_retained_line_id(),
        .content_generation = 0U,
        .source             = source,
    };
}

// Identity-only rebase: the row keeps its cells, so its content metadata
// (generation and wall-clock stamp) must survive. Renaming a line for
// provenance disambiguation does not make its content never-written; only
// fresh blank fill (replace_retained_line_id) resets the stamp.
void Terminal_screen_model::rebase_retained_line_id_preserving_content(
    Terminal_screen_row&                    row,
    Terminal_retained_line_provenance_source source)
{
    row.retained_line_provenance = {
        .retained_line_id   = next_retained_line_id(),
        .content_generation = row.retained_line_provenance.content_generation,
        .source             = source,
        .content_stamp_ms   = row.retained_line_provenance.content_stamp_ms,
        .content_stamp_is_unambiguous =
            row.retained_line_provenance.content_stamp_is_unambiguous,
    };
}

void Terminal_screen_model::replace_visible_retained_line_ids()
{
    for (Terminal_screen_row& row : active_grid_rows()) {
        replace_retained_line_id(row);
    }
}

void Terminal_screen_model::rebase_visible_retained_line_ids_preserving_content()
{
    for (Terminal_screen_row& row : active_grid_rows()) {
        rebase_retained_line_id_preserving_content(row);
    }
}

void Terminal_screen_model::replace_row_with_erased_retained_line(Terminal_screen_row& row)
{
    fill_row_with_erased_cells(row.cells);
    row.soft_wrap_columns = 0;
    row.image_slice.reset();
    replace_retained_line_id(row);
}

bool Terminal_screen_model::cells_have_same_selection_content(
    const Cell&    left,
    const Cell&    right) const
{
    return left.text           == right.text              &&
        left.display_width     == right.display_width     &&
        left.natural_display_width == right.natural_display_width &&
        left.wide_continuation == right.wide_continuation &&
        left.occupied          == right.occupied;
}

bool Terminal_screen_model::rows_have_same_selection_content(
    const std::vector<Cell>&   left,
    const std::vector<Cell>&   right) const
{
    const std::size_t common_size = std::min(left.size(), right.size());
    for (std::size_t index = 0; index < common_size; ++index) {
        if (!cells_have_same_selection_content(left[index], right[index])) {
            return false;
        }
    }

    // A grid resize pads or trims a row around the same written content.
    // Compare the width overhang against the never-written default cell, so a
    // resize that only adds or drops blank fill is not a content change: it
    // must not advance the row generation or refresh the content stamp.
    const std::vector<Cell>& longer = left.size() >= right.size() ? left : right;
    const Cell never_written_cell;
    for (std::size_t index = common_size; index < longer.size(); ++index) {
        if (!cells_have_same_selection_content(longer[index], never_written_cell)) {
            return false;
        }
    }

    return true;
}

bool Terminal_screen_model::printable_ascii_cell_changes_selection_content(
    const Terminal_screen_row& row,
    int                        column,
    QChar                      text) const
{
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled) {
        ++m_profile_stats.printable_ascii_local_cells_inspected;
    }
#endif

    const Cell& cell = row.cells[static_cast<std::size_t>(column)];
    return cell.text           != printable_ascii_cell_text(text) ||
        cell.display_width     != 1                               ||
        cell.wide_continuation                                      ||
        !cell.occupied;
}

bool Terminal_screen_model::printable_ascii_span_changes_selection_content(
    const Terminal_screen_row& row,
    int                        first_column,
    QStringView                text) const
{
    for (qsizetype offset = 0; offset < text.size(); ++offset) {
        if (printable_ascii_cell_changes_selection_content(
                row,
                first_column + static_cast<int>(offset),
                text[offset]))
        {
            return true;
        }
    }

    return false;
}

bool Terminal_screen_model::single_width_bmp_cell_changes_selection_content(
    const Terminal_screen_row& row,
    int                        column,
    QChar                      text) const
{
    const Cell& cell = row.cells[static_cast<std::size_t>(column)];
    return cell.text.size()   != 1 ||
        cell.text[0]          != text ||
        cell.display_width    != 1 ||
        cell.wide_continuation      ||
        !cell.occupied;
}

bool Terminal_screen_model::single_width_bmp_span_changes_selection_content(
    const Terminal_screen_row& row,
    int                        first_column,
    QStringView                text) const
{
    for (qsizetype offset = 0; offset < text.size(); ++offset) {
        if (single_width_bmp_cell_changes_selection_content(
                row,
                first_column + static_cast<int>(offset),
                text[offset]))
        {
            return true;
        }
    }

    return false;
}

bool Terminal_screen_model::scalar_span_changes_selection_content(
    const Terminal_screen_row& row,
    terminal_grid_position_t   position,
    QStringView                text,
    int                        display_width,
    int                        natural_display_width)
{
    int first_column = position.column;
    int last_column  = position.column + display_width - 1;

    auto include_cleared_span = [&](terminal_grid_position_t cleared_position) {
        const terminal_grid_position_t base_position = cell_base_position(cleared_position);
        const Cell&                    base_cell =
            row.cells[static_cast<std::size_t>(base_position.column)];
        const int clear_width = std::max(1, base_cell.display_width);
        first_column          = std::min(first_column, base_position.column);
        last_column           = std::max(last_column, base_position.column + clear_width - 1);
    };

    for (int width_offset = 0; width_offset < display_width; ++width_offset) {
        include_cleared_span({position.row, position.column + width_offset});
    }

    first_column = std::clamp(first_column, 0, m_config.grid_size.columns - 1);
    last_column  = std::clamp(last_column,  0, m_config.grid_size.columns - 1);
    const int new_span_end_column = position.column + display_width;
    bool changed = false;
    for (int column = first_column; column <= last_column; ++column) {
#if VNM_TERMINAL_PROFILING_ENABLED
        if (m_profile_stats.enabled) {
            ++m_profile_stats.scalar_span_local_cells_inspected;
        }
#endif
        Cell intended_cell;
        if (column == position.column) {
            intended_cell.text              = text.toString();
            intended_cell.display_width     = display_width;
            intended_cell.natural_display_width = natural_display_width;
            intended_cell.wide_continuation = false;
            intended_cell.occupied          = true;
        }
        else
        if (column > position.column && column < new_span_end_column) {
            intended_cell.text              = {};
            intended_cell.display_width     = 0;
            intended_cell.natural_display_width = 0;
            intended_cell.wide_continuation = true;
            intended_cell.occupied          = true;
        }

        if (!cells_have_same_selection_content(
                row.cells[static_cast<std::size_t>(column)],
                intended_cell))
        {
            record_selection_cell_mutation(row, column, column + 1);
            changed = true;
        }
    }

    return changed;
}

bool Terminal_screen_model::scalar_span_clear_changes_selection_content(
    const Terminal_screen_row& row,
    terminal_grid_position_t   position)
{
    const terminal_grid_position_t base_position = cell_base_position(position);
    const Cell&                    base_cell =
        row.cells[static_cast<std::size_t>(base_position.column)];
    const int clear_width = std::max(1, base_cell.display_width);
    bool changed = false;

    for (int width_offset = 0; width_offset < clear_width; ++width_offset) {
#if VNM_TERMINAL_PROFILING_ENABLED
        if (m_profile_stats.enabled) {
            ++m_profile_stats.scalar_span_local_cells_inspected;
        }
#endif
        if (!cells_have_same_selection_content(
                row.cells[static_cast<std::size_t>(base_position.column + width_offset)],
                Cell{}))
        {
            record_selection_cell_mutation(row, base_position.column, base_position.column + clear_width);
            changed = true;
        }
    }

    return changed;
}

void Terminal_screen_model::advance_row_content_generation_if_changed(
    Terminal_screen_row&       row,
    const std::vector<Cell>&   before_cells)
{
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled) {
        ++m_profile_stats.row_content_generation_comparisons;
        m_profile_stats.row_content_generation_comparison_cells +=
            static_cast<std::uint64_t>(
                std::max(before_cells.size(), row.cells.size()));
    }
#endif
    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_screen_model::advance_row_content_generation_if_changed::compare");
        if (rows_have_same_selection_content(before_cells, row.cells)) {
            return;
        }
    }
    for (std::size_t column = 0; column < row.cells.size(); ++column) {
        if (column >= before_cells.size() ||
            !cells_have_same_selection_content(before_cells[column], row.cells[column]))
        {
            record_selection_cell_mutation(row, (int)column, (int)column + 1);
        }
    }
    advance_row_content_generation_with_change_flag(row, true);
}

void Terminal_screen_model::advance_row_content_generation_with_change_flag(
    Terminal_screen_row&       row,
    bool                       selection_content_changed)
{
    if (!selection_content_changed) {
        return;
    }

#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled) {
        ++m_profile_stats.row_content_generation_advances;
    }
#endif

    if (row.retained_line_provenance.content_generation ==
        std::numeric_limits<std::uint64_t>::max())
    {
        throw std::overflow_error("terminal retained line content generation space exhausted");
    }

    ++row.retained_line_provenance.content_generation;
    // Every content mutation funnels through this generation advance, so this
    // is the single place that records when the line last changed for the
    // row-timestamp tooltip.
    row.retained_line_provenance.content_stamp_ms = QDateTime::currentMSecsSinceEpoch();
    row.retained_line_provenance.content_stamp_is_unambiguous = true;
    row.content_origin_spans.clear();
}

std::vector<bool> Terminal_screen_model::default_tab_stops(int column_count) const
{
    std::vector<bool> tab_stops(static_cast<std::size_t>(column_count), false);
    for (int column = m_config.tab_width; column < column_count; column += m_config.tab_width) {
        tab_stops[static_cast<std::size_t>(column)] = true;
    }
    return tab_stops;
}

void Terminal_screen_model::reset_grid()
{
    cancel_primary_repaint_recovery_candidate();
    cancel_primary_repaint_recovery_resize_guard();
    m_primary_backing.active_grid_state()     = make_empty_buffer_state();
    m_alternate_grid.active_grid_state()      = make_empty_buffer_state();
    m_active_buffer_id              = Terminal_buffer_id::PRIMARY;
    m_active_alternate_mode         = 0;
    m_dec_1049_saved_primary_cursor = false;
    m_tab_stops                     = default_tab_stops(m_config.grid_size.columns);
    restore_buffer_state(m_primary_backing.active_grid_state());
}

bool Terminal_screen_model::apply_grid_resize(
    terminal_grid_size_t grid_size,
    bool                 guard_primary_repaint_recovery)
{
    if (!is_terminal_screen_model_grid_size_supported(grid_size)) {
        return false;
    }

    if (grid_size.rows    == m_config.grid_size.rows &&
        grid_size.columns == m_config.grid_size.columns)
    {
        return false;
    }

    const terminal_grid_size_t grid_size_before = m_config.grid_size;
    cancel_primary_repaint_recovery_candidate();
    save_active_buffer_state();
    // Reflowed prefix rows are sealed into history at their new source width.
    m_config.grid_size = grid_size;
    resize_buffer_state(m_primary_backing.active_grid_state(), grid_size, true);
    resize_buffer_state(m_alternate_grid.active_grid_state(),  grid_size, false);
    m_tab_stops = default_tab_stops(grid_size.columns);
    restore_buffer_state(active_buffer_state());
    mark_grid_reflow_changed();
    if (guard_primary_repaint_recovery) {
        arm_primary_repaint_recovery_resize_guard();
    }
    else {
        cancel_primary_repaint_recovery_resize_guard();
    }
    mark_all_dirty();
    mark_viewport_changed();
    if (grid_size.rows != grid_size_before.rows) {
        record_active_grid_delta(
            Terminal_backing_delta_kind::ACTIVE_GRID_RESIZED,
            grid_size_before,
            grid_size);
    }
    if (grid_size.columns != grid_size_before.columns) {
        record_active_grid_delta(
            Terminal_backing_delta_kind::COLUMN_REFLOWED,
            grid_size_before,
            grid_size);
    }

    if (m_resize_transition_sink != nullptr) {
        Q_ASSERT(!m_pending_resize_transition.has_value());
        m_pending_resize_transition = std::pair{grid_size_before, grid_size};
    }

    return true;
}

Terminal_screen_model_result Terminal_screen_model::resize(
    terminal_grid_size_t grid_size,
    const terminal_screen_model_resize_transition_sink_t* resize_transition_sink)
{
    Terminal_screen_model_result result;
    m_scrollback_evicted_rows = 0;
    clear_backing_deltas();
    clear_recovery_proposals();
    clear_selection_continuity();
    clear_dirty();

    ingest_publication_t resize_transition_publication;
    Resize_transition_scope resize_transition_scope(
        *this,
        resize_transition_sink,
        resize_transition_publication);

    const bool grid_resized = apply_grid_resize(grid_size, true);
    if (m_pending_resize_transition.has_value()) {
        emit_pending_resize_transition();
    }
    else {
        accumulate_pending_changes(resize_transition_publication);
    }
    const bool resize_grid_reflow_changed = m_grid_reflow_changed;
    result_change_overrides_t resize_overrides;
    resize_overrides.terminal_content_changed =
        resize_transition_publication.terminal_content_changed;
    resize_overrides.active_buffer_changed =
        resize_transition_publication.active_buffer_changed;
    resize_overrides.grid_reflow_changed =
        resize_transition_publication.grid_reflow_changed;
    resize_overrides.viewport_changed =
        resize_transition_publication.viewport_changed;
    resize_overrides.mode_state_changed =
        resize_transition_publication.mode_state_changed;
    resize_overrides.mouse_reporting_mode_changed =
        resize_transition_publication.mouse_reporting_mode_changed;
    resize_overrides.alternate_scroll_mode_changed =
        resize_transition_publication.alternate_scroll_mode_changed;
    if (!grid_resized) {
        return finalize_result(std::move(result), resize_overrides);
    }

    if (m_modes.synchronized_output) {
        collect_synchronized_changes();
        if (resize_grid_reflow_changed) {
            m_synchronized_grid_reflow_changed = false;
        }
    }
    retain_referenced_active_hyperlink_ids();

    return finalize_result(std::move(result), resize_overrides);
}

Terminal_screen_model_result Terminal_screen_model::set_scrollback_limit(int limit)
{
    Terminal_screen_model_result result;
    m_scrollback_evicted_rows = 0;
    clear_backing_deltas();
    clear_recovery_proposals();
    clear_selection_continuity();
    clear_dirty();

    const int bounded_limit = std::max(0, limit);
    if (m_config.scrollback_limit == bounded_limit) {
        record_primary_history_delta(
            Terminal_backing_delta_kind::BACKING_UNCHANGED,
            scrollback_size(),
            scrollback_size(),
            0,
            0,
            0);
        return finalize_result(std::move(result));
    }

    m_config.scrollback_limit = bounded_limit;
    while (scrollback_size() > m_config.scrollback_limit) {
        evict_oldest_scrollback_rows(
            scrollback_size() - m_config.scrollback_limit);
    }
    if (m_backing_deltas.empty()) {
        record_primary_history_delta(
            Terminal_backing_delta_kind::BACKING_UNCHANGED,
            scrollback_size(),
            scrollback_size(),
            0,
            0,
            0);
    }
    if (m_scrollback_evicted_rows > 0) {
        mark_terminal_content_changed();
    }

    if (m_scrollback_evicted_rows >  0 &&
        m_active_buffer_id        == Terminal_buffer_id::PRIMARY)
    {
        mark_viewport_changed();
    }
    if (m_modes.synchronized_output) {
        collect_synchronized_changes();
    }
    retain_referenced_active_hyperlink_ids();

    return finalize_result(std::move(result));
}

Terminal_screen_model_result
Terminal_screen_model::set_retained_history_capacity_bytes(
    std::size_t capacity_bytes)
{
    if (capacity_bytes < k_terminal_min_retained_history_capacity_bytes ||
        capacity_bytes > k_terminal_max_retained_history_capacity_bytes)
    {
        throw std::invalid_argument("invalid retained history capacity");
    }

    const std::size_t aligned_capacity = terminal_history_ring_aligned_capacity(
        capacity_bytes);
    Terminal_screen_model_result result;
    m_scrollback_evicted_rows = 0;
    clear_backing_deltas();
    clear_recovery_proposals();
    clear_selection_continuity();
    clear_dirty();

    if (m_config.retained_history_capacity_bytes == aligned_capacity) {
        record_primary_history_delta(
            Terminal_backing_delta_kind::BACKING_UNCHANGED,
            scrollback_size(),
            scrollback_size(),
            0,
            0,
            0);
        return finalize_result(std::move(result));
    }

    // A decrease lowers the decoded-size cap, possibly below row images the
    // model holds, which then drop. The drop's diagnostic and dirty-row
    // storage are prepared before the resize, so that once the ring has
    // changed nothing can throw until the lookup index matches it again and
    // the images are dropped.
    const std::size_t new_cap_bytes = terminal_history_ring_max_record_bytes(aligned_capacity);
    const std::size_t dropped_image_bytes =
        aligned_capacity < m_config.retained_history_capacity_bytes
            ? drop_row_images_over_cap(new_cap_bytes, false)
            : 0U;
    if (dropped_image_bytes > 0U) {
        result.actions.push_back(make_payload_limit_diagnostic(
            QStringLiteral("DCS sixel"),
            dropped_image_bytes,
            new_cap_bytes,
            Parser_sequence_family::DCS));
        reserve_dirty_row_marks(static_cast<std::size_t>(m_config.grid_size.rows));
    }

    const int scrollback_rows_before = scrollback_size();
    const Retained_history_capacity_resize_result resize =
        m_primary_backing.resize_retained_history_capacity(aligned_capacity);
    m_config.retained_history_capacity_bytes = aligned_capacity;
    m_parser.set_sixel_raster_limit_bytes(sixel_raster_limit_bytes());
    for (const terminal_history_handle_t handle : resize.evicted_handles) {
        erase_retained_lookup_entry(
            Terminal_buffer_id::PRIMARY,
            handle.row_sequence);
    }
    if (resize.retained_handles_replaced) {
        for (const auto& entry : m_primary_backing.retained_history.index) {
            const auto found = m_primary_retained_lookup_index.history_by_row_sequence.find(
                entry.history_handle.row_sequence);
            if (found != m_primary_retained_lookup_index.history_by_row_sequence.end() &&
                found->second.history_ordinal == entry.ordinal)
            {
                found->second.history_handle = entry.history_handle;
            }
        }
    }
    if (dropped_image_bytes > 0U) {
        drop_row_images_over_cap(new_cap_bytes, true);
    }

    const int evicted_rows = static_cast<int>(resize.evicted_handles.size());
    m_scrollback_evicted_rows = evicted_rows;
    if (evicted_rows > 0) {
        record_primary_history_delta(
            Terminal_backing_delta_kind::PRIMARY_HISTORY_EVICTED,
            scrollback_rows_before,
            scrollback_size(),
            0,
            evicted_rows,
            0);
    }
    // A rebuild changes every kept row's handle, and the images it dropped
    // change what those rows show, whether or not it evicted any row.
    if (resize.retained_handles_replaced) {
        record_primary_history_delta(
            Terminal_backing_delta_kind::PRIMARY_HISTORY_REWRITTEN,
            scrollback_rows_before,
            scrollback_size(),
            0,
            0,
            0);
    }
    if (evicted_rows > 0 || resize.retained_handles_replaced) {
        mark_terminal_content_changed();
        if (m_active_buffer_id == Terminal_buffer_id::PRIMARY) {
            mark_viewport_changed();
        }
    }
    else {
        record_primary_history_delta(
            Terminal_backing_delta_kind::BACKING_UNCHANGED,
            scrollback_rows_before,
            scrollback_size(),
            0,
            0,
            0);
    }
    if (m_modes.synchronized_output) {
        collect_synchronized_changes();
    }
    retain_referenced_active_hyperlink_ids();

    return finalize_result(std::move(result));
}

Terminal_screen_model_result Terminal_screen_model::set_color_state(Terminal_color_state state)
{
    Terminal_screen_model_result result;
    clear_backing_deltas();
    clear_recovery_proposals();
    clear_selection_continuity();
    clear_dirty();

    m_color_state = std::move(state);
    mark_all_dirty();
    mark_viewport_changed();
    if (m_modes.synchronized_output) {
        collect_synchronized_changes();
    }

    return finalize_result(std::move(result));
}

void Terminal_screen_model::set_text_area_resize_policy(
    Terminal_text_area_resize_policy policy)
{
    m_config.text_area_resize_policy = policy;
}

void Terminal_screen_model::set_cell_pixel_size(terminal_cell_pixel_size_t size)
{
    if (!is_valid_cell_pixel_size(size)) {
        throw std::invalid_argument("invalid cell pixel size");
    }

    m_config.cell_pixel_size = size;
}

void Terminal_screen_model::set_primary_repaint_recovery_enabled(bool enabled)
{
    if (m_config.recover_scrollback_from_primary_repaints == enabled) {
        return;
    }

    m_config.recover_scrollback_from_primary_repaints = enabled;
    if (!enabled) {
        cancel_primary_repaint_recovery_resize_guard();
        cancel_primary_repaint_recovery_candidate(
            Terminal_recovery_attempt_reason::RECOVERY_DISABLED);
    }
}

void Terminal_screen_model::reset_scroll_region()
{
    m_scroll_top        = 0;
    m_scroll_bottom     = m_config.grid_size.rows - 1;
    m_origin_mode       = false;
    m_modes.origin_mode = false;
}

void Terminal_screen_model::reset_tab_stops()
{
    m_tab_stops = default_tab_stops(m_config.grid_size.columns);
}

void Terminal_screen_model::put_scalar(QString text)
{
    const Terminal_utf8_width_result width = measure_utf8_width(text.toUtf8());
    if (width.cells <= 0) {
        append_zero_width_scalar(std::move(text));
        return;
    }

    put_spacing_scalar(std::move(text), width.cells);
}

void Terminal_screen_model::put_text(QString text)
{
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled) {
        ++m_profile_stats.print_text_calls;
    }
#endif

    for (qsizetype i = 0; i < text.size();) {
        const qsizetype ascii_begin = i;
        while (i < text.size() && is_printable_ascii(text[i])) {
            ++i;
        }
        if (i > ascii_begin) {
            put_printable_ascii_text(QStringView(text).sliced(ascii_begin, i - ascii_begin));
            continue;
        }

        const qsizetype single_width_bmp_begin = i;
        while (i < text.size() && is_single_width_non_ascii_bmp(text[i])) {
            ++i;
        }
        if (i > single_width_bmp_begin) {
            put_single_width_bmp_text(
                QStringView(text).sliced(single_width_bmp_begin, i - single_width_bmp_begin));
            continue;
        }

        const QChar current = text[i];
        if (current.isHighSurrogate() &&
            i + 1 < text.size() &&
            text[i + 1].isLowSurrogate())
        {
            put_scalar(QString(current) + text[i + 1]);
            i += 2;
            continue;
        }

        put_scalar(QString(current));
        ++i;
    }
}

void Terminal_screen_model::put_printable_ascii_text(QStringView text)
{
#if VNM_TERMINAL_PROFILING_ENABLED
    struct profile_depth_guard_t
    {
        int& depth;

        ~profile_depth_guard_t()
        {
            --depth;
        }
    };

    ++m_text_span_profile_depth;
    const profile_depth_guard_t profile_depth_guard{m_text_span_profile_depth};
#endif
    qsizetype offset = 0;
    while (offset < text.size()) {
        if (m_modes.autowrap && m_pending_wrap) {
#if VNM_TERMINAL_PROFILING_ENABLED
            if (m_profile_stats.enabled) {
                ++m_profile_stats.line_wraps_from_text_writes;
            }
#endif
            wrap_line();
            m_pending_wrap = false;
        }

        const int available_columns = m_config.grid_size.columns - m_cursor.column;
        if (available_columns <= 0) {
            return;
        }

        const qsizetype remaining = text.size() - offset;
        if (!m_modes.autowrap && remaining > available_columns) {
            mark_cursor_dirty();
            Terminal_screen_row& screen_row =
                active_grid_rows()[static_cast<std::size_t>(m_cursor.row)];
            // The touched cells run from the cursor to the margin: clear their
            // image before any cell changes.
            screen_row.image_slice = image_slice_without_cells(
                screen_row,
                m_cursor.column,
                m_config.grid_size.columns);
            bool selection_content_changed = false;
            if (available_columns > 1) {
                selection_content_changed =
                    printable_ascii_span_changes_selection_content(
                        screen_row,
                        m_cursor.column,
                        text.sliced(offset, available_columns - 1));
                write_printable_ascii_span_content(
                    screen_row,
                    m_cursor.column,
                    text.sliced(offset, available_columns - 1));
            }
            else {
                mark_terminal_content_changed();
            }
            selection_content_changed =
                selection_content_changed ||
                printable_ascii_cell_changes_selection_content(
                    screen_row,
                    m_config.grid_size.columns - 1,
                    text[text.size() - 1]);
            write_printable_ascii_cell_content(
                screen_row,
                m_config.grid_size.columns - 1,
                text[text.size() - 1]);
            advance_row_content_generation_with_change_flag(
                screen_row,
                selection_content_changed);
            m_cursor.column = m_config.grid_size.columns - 1;
            m_pending_wrap = false;
            mark_cursor_dirty();
            return;
        }

        const int span_length = static_cast<int>(std::min<qsizetype>(
            remaining,
            available_columns));
        mark_cursor_dirty();
        write_printable_ascii_span(
            m_cursor.row,
            m_cursor.column,
            text.sliced(offset, span_length));
        if (span_length >= available_columns) {
            m_cursor.column = m_config.grid_size.columns - 1;
            m_pending_wrap = m_modes.autowrap;
        }
        else {
            m_cursor.column += span_length;
            m_pending_wrap = false;
        }
        mark_cursor_dirty();
        offset += span_length;
    }
}

void Terminal_screen_model::put_single_width_bmp_text(QStringView text)
{
#if VNM_TERMINAL_PROFILING_ENABLED
    struct profile_depth_guard_t
    {
        int& depth;

        ~profile_depth_guard_t()
        {
            --depth;
        }
    };

    ++m_text_span_profile_depth;
    const profile_depth_guard_t profile_depth_guard{m_text_span_profile_depth};
#endif
    qsizetype offset = 0;
    while (offset < text.size()) {
        if (m_modes.autowrap && m_pending_wrap) {
#if VNM_TERMINAL_PROFILING_ENABLED
            if (m_profile_stats.enabled) {
                ++m_profile_stats.line_wraps_from_text_writes;
            }
#endif
            wrap_line();
            m_pending_wrap = false;
        }

        const int available_columns = m_config.grid_size.columns - m_cursor.column;
        if (available_columns <= 0) {
            return;
        }

        const qsizetype remaining = text.size() - offset;
        if (!m_modes.autowrap && remaining > available_columns) {
            mark_cursor_dirty();
            Terminal_screen_row& screen_row =
                active_grid_rows()[static_cast<std::size_t>(m_cursor.row)];
            // The touched cells run from the cursor to the margin: clear their
            // image before any cell changes.
            screen_row.image_slice = image_slice_without_cells(
                screen_row,
                m_cursor.column,
                m_config.grid_size.columns);
            bool selection_content_changed = false;
            if (available_columns > 1) {
                selection_content_changed =
                    single_width_bmp_span_changes_selection_content(
                        screen_row,
                        m_cursor.column,
                        text.sliced(offset, available_columns - 1));
                write_single_width_bmp_span_content(
                    screen_row,
                    m_cursor.column,
                    text.sliced(offset, available_columns - 1));
            }
            else {
                mark_terminal_content_changed();
            }
            selection_content_changed =
                selection_content_changed ||
                single_width_bmp_cell_changes_selection_content(
                    screen_row,
                    m_config.grid_size.columns - 1,
                    text[text.size() - 1]);
            const QString margin_text(text[text.size() - 1]);
            write_single_width_bmp_cell_content(
                screen_row,
                m_config.grid_size.columns - 1,
                margin_text);
            advance_row_content_generation_with_change_flag(
                screen_row,
                selection_content_changed);
            m_cursor.column = m_config.grid_size.columns - 1;
            m_pending_wrap = false;
            mark_cursor_dirty();
            return;
        }

        const int span_length = static_cast<int>(std::min<qsizetype>(
            remaining,
            available_columns));
        mark_cursor_dirty();
        write_single_width_bmp_span(
            m_cursor.row,
            m_cursor.column,
            text.sliced(offset, span_length));
        if (span_length >= available_columns) {
            m_cursor.column = m_config.grid_size.columns - 1;
            m_pending_wrap = m_modes.autowrap;
        }
        else {
            m_cursor.column += span_length;
            m_pending_wrap = false;
        }
        mark_cursor_dirty();
        offset += span_length;
    }
}

void Terminal_screen_model::write_printable_ascii_span(
    int                        row,
    int                        first_column,
    QStringView                text)
{
    Terminal_screen_row& screen_row = active_grid_rows()[static_cast<std::size_t>(row)];
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled) {
        ++m_profile_stats.printable_ascii_span_calls;
        const std::uint64_t span_characters = static_cast<std::uint64_t>(text.size());
        m_profile_stats.printable_ascii_span_characters += span_characters;
        m_profile_stats.max_printable_ascii_span_characters =
            std::max(
                m_profile_stats.max_printable_ascii_span_characters,
                span_characters);
    }
#endif

    const bool selection_content_changed =
        printable_ascii_span_changes_selection_content(screen_row, first_column, text);
    write_printable_ascii_span_content(screen_row, first_column, text);
    advance_row_content_generation_with_change_flag(screen_row, selection_content_changed);
    mark_dirty(row);
}

void Terminal_screen_model::write_printable_ascii_span_content(
    Terminal_screen_row&       row,
    int                        first_column,
    QStringView                text)
{
    if (text.isEmpty()) {
        return;
    }

    mark_terminal_content_changed();
    row.image_slice = image_slice_without_cells(
        row,
        first_column,
        first_column + static_cast<int>(text.size()));
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled) {
        m_profile_stats.printable_ascii_cells_written +=
            static_cast<std::uint64_t>(text.size());
    }
#endif
    for (qsizetype offset = 0; offset < text.size(); ++offset) {
        write_printable_ascii_cell_content(
            row,
            first_column + static_cast<int>(offset),
            text[offset]);
    }
}

void Terminal_screen_model::write_printable_ascii_cell_content(
    Terminal_screen_row&       row,
    int                        column,
    QChar                      text)
{
    Cell& cell = row.cells[static_cast<std::size_t>(column)];
    if ((!m_selection_cell_watches[0].empty() || !m_selection_cell_watches[1].empty()) &&
        printable_ascii_cell_changes_selection_content(row, column, text))
    {
        const int base = cell_base_column_in_row(row, column);
        record_selection_cell_mutation(row, base,
            std::max(column + 1, base + std::max(1, row.cells[(std::size_t)base].display_width)));
    }
    if (cell.wide_continuation ||
        cell.display_width != 1)
    {
        clear_cell_at_content(row, column);
    }

    Cell& target_cell = row.cells[static_cast<std::size_t>(column)];
    target_cell.text              = printable_ascii_cell_text(text);
    target_cell.text_category     = Terminal_render_cell_text_category::PRINTABLE_ASCII;
    target_cell.display_width     = 1;
    target_cell.natural_display_width = 1;
    target_cell.wide_continuation = false;
    target_cell.occupied          = true;
    target_cell.style_id          = m_current_style_id;
    target_cell.hyperlink_id      = m_current_hyperlink_id;
}

void Terminal_screen_model::write_single_width_bmp_span(
    int                        row,
    int                        first_column,
    QStringView                text)
{
    Terminal_screen_row& screen_row = active_grid_rows()[static_cast<std::size_t>(row)];

    const bool selection_content_changed =
        single_width_bmp_span_changes_selection_content(screen_row, first_column, text);
    write_single_width_bmp_span_content(screen_row, first_column, text);
    advance_row_content_generation_with_change_flag(screen_row, selection_content_changed);
    mark_dirty(row);
}

void Terminal_screen_model::write_single_width_bmp_span_content(
    Terminal_screen_row&       row,
    int                        first_column,
    QStringView                text)
{
    if (text.isEmpty()) {
        return;
    }

    mark_terminal_content_changed();
    row.image_slice = image_slice_without_cells(
        row,
        first_column,
        first_column + static_cast<int>(text.size()));
    const QString first_text(text[0]);
    for (qsizetype offset = 0; offset < text.size(); ++offset) {
        if (text[offset] == text[0]) {
            write_single_width_bmp_cell_content(
                row,
                first_column + static_cast<int>(offset),
                first_text);
        }
        else {
            const QString cell_text(text[offset]);
            write_single_width_bmp_cell_content(
                row,
                first_column + static_cast<int>(offset),
                cell_text);
        }
    }
}

void Terminal_screen_model::write_single_width_bmp_cell_content(
    Terminal_screen_row&       row,
    int                        column,
    const QString&             text)
{
    Cell& cell = row.cells[static_cast<std::size_t>(column)];
    if ((!m_selection_cell_watches[0].empty() || !m_selection_cell_watches[1].empty()) &&
        single_width_bmp_cell_changes_selection_content(row, column, text[0]))
    {
        const int base = cell_base_column_in_row(row, column);
        record_selection_cell_mutation(row, base,
            std::max(column + 1, base + std::max(1, row.cells[(std::size_t)base].display_width)));
    }
    if (cell.wide_continuation ||
        cell.display_width != 1)
    {
        clear_cell_at_content(row, column);
    }

    Cell& target_cell = row.cells[static_cast<std::size_t>(column)];
    target_cell.text              = text;
    target_cell.text_category     = Terminal_render_cell_text_category::NON_ASCII;
    target_cell.display_width     = 1;
    target_cell.natural_display_width = 1;
    target_cell.wide_continuation = false;
    target_cell.occupied          = true;
    target_cell.style_id          = m_current_style_id;
    target_cell.hyperlink_id      = m_current_hyperlink_id;
}

void Terminal_screen_model::put_spacing_scalar(QString text, int display_width)
{
    const int natural_display_width = display_width;
    if (display_width > m_config.grid_size.columns) {
        display_width = 1;
    }

    if (m_modes.autowrap && m_pending_wrap) {
        wrap_line();
        m_pending_wrap = false;
    }

    if (display_width > m_config.grid_size.columns - m_cursor.column) {
        if (m_modes.autowrap) {
            wrap_line();
        }
        else {
            return;
        }
    }

    place_cell_text(
        m_cursor,
        std::move(text),
        display_width,
        natural_display_width);
    set_cursor_after_cell(m_cursor, display_width);
}

void Terminal_screen_model::append_zero_width_scalar(QString text)
{
    terminal_grid_position_t target = m_cursor;
    if (!m_pending_wrap) {
        const bool current_margin_cell_is_target =
            !m_modes.autowrap                                 &&
            m_cursor.column == m_config.grid_size.columns - 1 &&
            active_grid_rows()[m_cursor.row].cells[m_cursor.column].occupied;

        if (!current_margin_cell_is_target && m_cursor.column == 0) {
            return;
        }

        if (!current_margin_cell_is_target) {
            target.column = m_cursor.column - 1;
        }
    }

    target = cell_base_position(target);
    Cell& cell = active_grid_rows()[target.row].cells[target.column];
    if (!cell.occupied) {
        return;
    }

    QString combined_text = cell.text + text;
    int natural_display_width = measure_utf8_width(combined_text.toUtf8()).cells;
    if (natural_display_width <= 0) { natural_display_width = 1; }
    int display_width = natural_display_width;
    if (display_width > m_config.grid_size.columns) { display_width = 1; }
    if (display_width > m_config.grid_size.columns - target.column) {
        if (!m_modes.autowrap) {
            display_width = 1;
        }
        else {
            const Terminal_style_id     style_id     = cell.style_id;
            const Terminal_hyperlink_id hyperlink_id = cell.hyperlink_id;
            Terminal_screen_row& screen_row =
                active_grid_rows()[static_cast<std::size_t>(target.row)];
            const bool selection_content_changed =
                scalar_span_clear_changes_selection_content(screen_row, target);
            clear_cell_at(target);
            advance_row_content_generation_with_change_flag(
                screen_row,
                selection_content_changed);
            mark_dirty(target.row);
            m_cursor       = target;
            m_pending_wrap = false;
            wrap_line();
            install_cell_span(
                m_cursor,
                std::move(combined_text),
                display_width,
                natural_display_width,
                style_id,
                hyperlink_id);
            set_cursor_after_cell(m_cursor, display_width);
            return;
        }
    }

    install_cell_span(
        target,
        std::move(combined_text),
        display_width,
        natural_display_width,
        cell.style_id,
        cell.hyperlink_id);
    set_cursor_after_cell(target, display_width);
}

void Terminal_screen_model::install_cell_span(
    terminal_grid_position_t   position,
    QString                    text,
    int                        display_width,
    int                        natural_display_width,
    Terminal_style_id          style_id,
    Terminal_hyperlink_id      hyperlink_id)
{
    mark_terminal_content_changed();
    Terminal_screen_row& screen_row =
        active_grid_rows()[static_cast<std::size_t>(position.row)];
    const bool selection_content_changed =
        scalar_span_changes_selection_content(
            screen_row,
            position,
            QStringView(text),
            display_width,
            natural_display_width);
    // Computed before any cell changes, so a failed allocation leaves the row
    // whole.
    std::shared_ptr<const Terminal_image_slice> image =
        image_slice_without_cells(screen_row, position.column, position.column + display_width);
    clear_cell_at(position);

    Cell& cell = screen_row.cells[position.column];
    cell.text              = std::move(text);
    cell.text_category     = Terminal_render_cell_text::category_for_text(
        QStringView(cell.text));
    cell.display_width     = display_width;
    cell.natural_display_width = std::max(display_width, natural_display_width);
    cell.wide_continuation = false;
    cell.occupied          = true;
    cell.style_id          = style_id;
    cell.hyperlink_id      = hyperlink_id;

    for (int width_offset = 1; width_offset < display_width; ++width_offset) {
        clear_cell_at({position.row, position.column + width_offset});
        Cell& continuation = active_grid_rows()[position.row].cells[position.column + width_offset];
        continuation.text              = {};
        continuation.text_category     = Terminal_render_cell_text_category::EMPTY;
        continuation.display_width     = 0;
        continuation.natural_display_width = 0;
        continuation.wide_continuation = true;
        continuation.occupied          = true;
        continuation.style_id          = cell.style_id;
        continuation.hyperlink_id      = cell.hyperlink_id;
    }

    screen_row.image_slice = std::move(image);
    advance_row_content_generation_with_change_flag(screen_row, selection_content_changed);
    mark_dirty(position.row);
}

void Terminal_screen_model::place_cell_text(
    terminal_grid_position_t   position,
    QString                    text,
    int                        display_width,
    int                        natural_display_width)
{
    install_cell_span(
        position,
        std::move(text),
        display_width,
        natural_display_width,
        m_current_style_id,
        m_current_hyperlink_id);
}

void Terminal_screen_model::clear_cell_span(terminal_grid_position_t position)
{
    mark_terminal_content_changed();
    Terminal_screen_row& screen_row =
        active_grid_rows()[static_cast<std::size_t>(position.row)];
    clear_cell_span_content(screen_row, position.column);
}

void Terminal_screen_model::clear_cell_span_content(
    Terminal_screen_row&       row,
    int                        column)
{
    Cell&     cell          = row.cells[static_cast<std::size_t>(column)];
    const int display_width = cell.display_width;
    cell = Cell{};

    for (int width_offset = 1; width_offset < display_width; ++width_offset) {
        row.cells[static_cast<std::size_t>(column + width_offset)] = Cell{};
    }
}

void Terminal_screen_model::clear_cell_at(terminal_grid_position_t position)
{
    const terminal_grid_position_t base_position = cell_base_position(position);
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled && m_text_span_profile_depth > 0) {
        const Cell& base_cell =
            active_grid_rows()[base_position.row].cells[base_position.column];
        if (base_position.row    != position.row ||
            base_position.column != position.column ||
            base_cell.display_width > 1)
        {
            ++m_profile_stats.wide_boundary_repairs_from_text_writes;
        }
    }
#endif
    clear_cell_span(base_position);
}

int Terminal_screen_model::cell_base_column_in_row(
    const Terminal_screen_row& row,
    int                        column) const
{
    if (!row.cells[static_cast<std::size_t>(column)].wide_continuation) {
        return column;
    }

    for (int candidate = column - 1; candidate >= 0; --candidate) {
        const Cell& cell = row.cells[static_cast<std::size_t>(candidate)];
        if (!cell.wide_continuation && column - candidate < cell.display_width) {
            return candidate;
        }
    }

    return column;
}

void Terminal_screen_model::clear_cell_at_content(
    Terminal_screen_row&       row,
    int                        column)
{
    const int base_column = cell_base_column_in_row(row, column);
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled && m_text_span_profile_depth > 0) {
        const Cell& base_cell = row.cells[static_cast<std::size_t>(base_column)];
        if (base_column != column || base_cell.display_width > 1) {
            ++m_profile_stats.wide_boundary_repairs_from_text_writes;
        }
    }
#endif
    clear_cell_span_content(row, base_column);
}

Terminal_screen_model::Cell Terminal_screen_model::erased_cell() const
{
    Cell cell;
    if (m_current_style_id == k_default_terminal_style_id) {
        return cell;
    }

    cell.occupied = true;
    cell.style_id = m_current_style_id;
    return cell;
}

void Terminal_screen_model::fill_row_with_erased_cells(std::vector<Cell>& row) const
{
    row.assign(static_cast<std::size_t>(m_config.grid_size.columns), erased_cell());
}

void Terminal_screen_model::erase_cell_at(terminal_grid_position_t position)
{
    const Cell                     replacement   = erased_cell();
    const terminal_grid_position_t base_position = cell_base_position(position);
    const int                      display_width =
        active_grid_rows()[base_position.row].cells[base_position.column].display_width;
    clear_cell_span(base_position);
    if (!replacement.occupied) {
        return;
    }

    const int bounded_width = std::clamp(
        display_width,
        1,
        m_config.grid_size.columns - base_position.column);
    for (int width_offset = 0; width_offset < bounded_width; ++width_offset) {
        active_grid_rows()[base_position.row].cells[base_position.column + width_offset] = replacement;
    }
}

void Terminal_screen_model::erase_row_range(int row, int first_column, int last_column)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_screen_model::erase_row_range");

    if (row < 0 || row >= m_config.grid_size.rows) {
        return;
    }

    first_column = std::clamp(first_column, 0, m_config.grid_size.columns - 1);
    last_column  = std::clamp(last_column,  0, m_config.grid_size.columns - 1);
    if (first_column > last_column) {
        return;
    }

    Terminal_screen_row& screen_row = active_grid_rows()[static_cast<std::size_t>(row)];
    // Before any cell changes, and before the unoccupied-tail trim below: an
    // image usually covers cells with no text, and the erase clears its pixels
    // there too.
    screen_row.image_slice = image_slice_without_cells(screen_row, first_column, last_column + 1);
    if (last_column == m_config.grid_size.columns - 1) {
        screen_row.soft_wrap_columns = 0;
    }
    const Cell replacement = erased_cell();
    bool       selection_content_changed = false;

#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled) {
        ++m_profile_stats.erase_row_range_calls;
    }
#endif

    // Redraw clients commonly issue CSI K for every line.  Taking a full row
    // copy here and comparing that copy after the erase turns a line-local
    // operation into two complete row scans plus an allocation.  Clear each
    // affected cell in place while retaining the one semantic check needed
    // for the retained-line content generation.  If the range touches a wide
    // continuation, include its base and complete span exactly as
    // erase_cell_at() does.
    mark_terminal_content_changed();
    if (!replacement.occupied) {
        // CSI K commonly clears a populated prefix followed by the blank tail
        // of a terminal row.  The occupied bit is the model's canonical
        // content marker, so a backwards trim skips that tail without
        // changing the semantics of a range that reaches a wide continuation.
        while (last_column >= first_column &&
               !screen_row.cells[static_cast<std::size_t>(last_column)].occupied)
        {
            --last_column;
        }
        if (last_column < first_column) {
            mark_dirty(row);
            return;
        }
    }

    for (int column = first_column; column <= last_column; ++column) {
        int base_column = column;
        int clear_end = column + 1;
        const Cell& column_cell = screen_row.cells[static_cast<std::size_t>(column)];
        if (column_cell.wide_continuation) {
            base_column = cell_base_column_in_row(screen_row, column);
            const int display_width = std::max(
                1,
                screen_row.cells[static_cast<std::size_t>(base_column)].display_width);
            clear_end = std::min(
                m_config.grid_size.columns,
                base_column + display_width);
        }
        else
        if (column_cell.display_width > 1) {
            clear_end = std::min(
                m_config.grid_size.columns,
                column + column_cell.display_width);
        }
#if VNM_TERMINAL_PROFILING_ENABLED
        if (m_profile_stats.enabled && clear_end > column + 1) {
            m_profile_stats.erase_row_wide_span_cells +=
                static_cast<std::uint64_t>(clear_end - std::max(column, base_column));
        }
#endif
        for (int clear_column = base_column; clear_column < clear_end; ++clear_column) {
            Cell& cell = screen_row.cells[static_cast<std::size_t>(clear_column)];
#if VNM_TERMINAL_PROFILING_ENABLED
            if (m_profile_stats.enabled) {
                ++m_profile_stats.erase_row_cells_visited;
            }
#endif

            // Assignment also releases the old QString and copies all of the
            // replacement metadata.  Repeated redraws often ask to clear a
            // cell that already has exactly the erase representation; keep
            // that case entirely scalar and leave selection-content state
            // unchanged.
            const bool already_erased = replacement.occupied
                ? cell.occupied          &&
                    cell.display_width     == replacement.display_width     &&
                    cell.natural_display_width == replacement.natural_display_width &&
                    cell.wide_continuation == replacement.wide_continuation &&
                    cell.style_id          == replacement.style_id          &&
                    cell.hyperlink_id      == replacement.hyperlink_id      &&
                    cell.text_category     == replacement.text_category     &&
                    cell.text               == replacement.text
                : !cell.occupied;
            if (already_erased) {
#if VNM_TERMINAL_PROFILING_ENABLED
                if (m_profile_stats.enabled) {
                    ++m_profile_stats.erase_row_cells_already_erased;
                }
#endif
                continue;
            }

            if (cell.display_width     != replacement.display_width     ||
                 cell.natural_display_width != replacement.natural_display_width ||
                 cell.wide_continuation != replacement.wide_continuation ||
                 cell.occupied          != replacement.occupied          ||
                 cell.text              != replacement.text)
            {
                record_selection_cell_mutation(screen_row, clear_column, clear_column + 1);
                selection_content_changed = true;
            }
            cell = replacement;
#if VNM_TERMINAL_PROFILING_ENABLED
            if (m_profile_stats.enabled) {
                ++m_profile_stats.erase_row_cells_replaced;
            }
#endif
        }
    }

    advance_row_content_generation_with_change_flag(
        screen_row,
        selection_content_changed);
    mark_dirty(row);
}

void Terminal_screen_model::clear_screen_before_cursor()
{
    for (int row = 0; row < m_cursor.row; ++row) {
        erase_row_range(row, 0, m_config.grid_size.columns - 1);
    }
    erase_row_range(m_cursor.row, 0, m_cursor.column);
}

void Terminal_screen_model::clear_screen_after_cursor()
{
    erase_row_range(m_cursor.row, m_cursor.column, m_config.grid_size.columns - 1);
    for (int row = m_cursor.row + 1; row < m_config.grid_size.rows; ++row) {
        erase_row_range(row, 0, m_config.grid_size.columns - 1);
    }
}

void Terminal_screen_model::erase_visible_screen()
{
    mark_terminal_content_changed();
    const bool primary_repaint_rebuild =
        m_primary_repaint_recovery_candidate.active;
    for (int row = 0; row < m_config.grid_size.rows; ++row) {
        Terminal_screen_row& screen_row = active_grid_rows()[static_cast<std::size_t>(row)];
        if (primary_repaint_rebuild) {
            replace_row_with_erased_retained_line(screen_row);
        }
        else {
            const std::vector<Cell> before_cells = screen_row.cells;
            fill_row_with_erased_cells(screen_row.cells);
            screen_row.soft_wrap_columns = 0;
            screen_row.image_slice.reset();
            advance_row_content_generation_if_changed(screen_row, before_cells);
        }
    }
    mark_all_dirty();
}

void Terminal_screen_model::erase_in_display(int mode)
{
    m_pending_wrap = false;

    switch (mode) {
        case 0:
            if (m_primary_repaint_recovery_candidate.active &&
                m_cursor.row    == 0 &&
                m_cursor.column == 0)
            {
                replace_visible_retained_line_ids();
                m_primary_repaint_recovery_candidate.visible_row_identity_ambiguous = false;
                cancel_primary_repaint_recovery_candidate();
            }
            clear_screen_after_cursor();
            break;
        case 1:  clear_screen_before_cursor(); break;
        case 2:  erase_visible_screen();       break;
        case 3:
        {
            cancel_primary_repaint_recovery_candidate();
            if (m_primary_backing.retained_history_empty()) {
                record_primary_history_delta(
                    Terminal_backing_delta_kind::BACKING_UNCHANGED,
                    scrollback_size(),
                    scrollback_size(),
                    0,
                    0,
                    0);
                break;
            }

            const int scrollback_rows_before = scrollback_size();
            m_scrollback_evicted_rows       += scrollback_rows_before;
            m_primary_backing.clear_retained_history();
            m_primary_retained_lookup_index.history_by_row_sequence.clear();
            record_primary_history_delta(
                Terminal_backing_delta_kind::PRIMARY_HISTORY_CLEARED,
                scrollback_rows_before,
                0,
                0,
                scrollback_rows_before,
                0);
            mark_terminal_content_changed();
            mark_viewport_changed();
            break;
        }
        default: break;
    }
}

void Terminal_screen_model::erase_in_line(int mode)
{
    // EL must not consume delayed autowrap: a following printable still wraps,
    // while CR or cursor movement can cancel the pending wrap first.
    if (m_primary_repaint_recovery_candidate.active &&
        mode            == 0 &&
        m_cursor.column == 0)
    {
        const Terminal_screen_row& candidate_row =
            m_primary_repaint_recovery_candidate.rows[static_cast<std::size_t>(m_cursor.row)];
        m_primary_repaint_recovery_candidate.line_start_clear_before_text =
            m_primary_repaint_recovery_candidate.line_start_clear_before_text ||
            row_has_visible_text(candidate_row);
        m_primary_repaint_recovery_candidate.explicit_non_home_repaint_address =
            m_primary_repaint_recovery_candidate.explicit_non_home_repaint_address ||
            (m_primary_repaint_recovery_candidate.pending_non_home_addressed_row ==
                m_cursor.row &&
                row_has_visible_text(candidate_row));
        m_primary_repaint_recovery_candidate.pending_non_home_addressed_row = -1;
    }

    switch (mode) {
        case 0:  erase_row_range(m_cursor.row, m_cursor.column, m_config.grid_size.columns - 1); break;
        case 1:  erase_row_range(m_cursor.row, 0, m_cursor.column);                              break;
        case 2:  erase_row_range(m_cursor.row, 0, m_config.grid_size.columns - 1);               break;
        default: break;
    }
}

void Terminal_screen_model::erase_characters(int count)
{
    m_pending_wrap = false;

    const int bounded_count =
        std::clamp(count, 1, m_config.grid_size.columns - m_cursor.column);
    erase_row_range(
        m_cursor.row,
        m_cursor.column,
        m_cursor.column + bounded_count - 1);
}

void Terminal_screen_model::clear_wide_continuation_boundary(
    std::vector<Cell>& row,
    int                column)
{
    if (column >= 0                          &&
        column <  m_config.grid_size.columns &&
        row[static_cast<std::size_t>(column)].wide_continuation)
    {
        erase_cell_at({m_cursor.row, column});
    }
}

void Terminal_screen_model::finalize_row_cell_mutation(
    Terminal_screen_row&     screen_row,
    const std::vector<Cell>& before_cells)
{
    repair_wide_spans_in_row(screen_row.cells, m_config.grid_size.columns);

    m_pending_wrap = false;
    advance_row_content_generation_if_changed(screen_row, before_cells);
    mark_dirty(m_cursor.row);
}

void Terminal_screen_model::insert_cells(int count)
{
    mark_terminal_content_changed();
    count = std::clamp(count, 1, m_config.grid_size.columns - m_cursor.column);
    Terminal_screen_row& screen_row = active_grid_rows()[static_cast<std::size_t>(m_cursor.row)];
    std::vector<Cell>& row = screen_row.cells;
    const std::vector<Cell> before_cells = row;
    // Computed before any cell changes, so a failed allocation leaves the row
    // whole.
    std::shared_ptr<const Terminal_image_slice> image =
        image_slice_shifted(screen_row, m_cursor.column, count);

    clear_wide_continuation_boundary(row, m_cursor.column);
    clear_wide_continuation_boundary(row, m_config.grid_size.columns - count);

    std::move_backward(
        row.begin() + m_cursor.column,
        row.begin() + m_config.grid_size.columns - count,
        row.end());

    const Cell replacement = erased_cell();
    for (int column = m_cursor.column; column < m_cursor.column + count; ++column) {
        row[static_cast<std::size_t>(column)] = replacement;
    }
    screen_row.image_slice = std::move(image);

    finalize_row_cell_mutation(screen_row, before_cells);
}

void Terminal_screen_model::delete_cells(int count)
{
    mark_terminal_content_changed();
    count = std::clamp(count, 1, m_config.grid_size.columns - m_cursor.column);
    Terminal_screen_row& screen_row = active_grid_rows()[static_cast<std::size_t>(m_cursor.row)];
    std::vector<Cell>& row = screen_row.cells;
    const std::vector<Cell> before_cells = row;
    // Computed before any cell changes, so a failed allocation leaves the row
    // whole.
    std::shared_ptr<const Terminal_image_slice> image =
        image_slice_shifted(screen_row, m_cursor.column, -count);

    for (int column = m_cursor.column; column < m_cursor.column + count; ++column) {
        erase_cell_at({m_cursor.row, column});
    }
    clear_wide_continuation_boundary(row, m_cursor.column + count);

    std::move(
        row.begin() + m_cursor.column + count,
        row.end(),
        row.begin() + m_cursor.column);

    const Cell replacement = erased_cell();
    for (int column = m_config.grid_size.columns - count;
        column < m_config.grid_size.columns;
        ++column)
    {
        row[static_cast<std::size_t>(column)] = replacement;
    }
    screen_row.image_slice = std::move(image);

    finalize_row_cell_mutation(screen_row, before_cells);
}

void Terminal_screen_model::break_soft_wrap_before(int row)
{
    if (row > 0) {
        active_grid_rows()[(std::size_t)row - 1U].soft_wrap_columns = 0;
    }
}

void Terminal_screen_model::insert_lines(int count)
{
    if (m_cursor.row < m_scroll_top || m_cursor.row > m_scroll_bottom) {
        return;
    }

    break_soft_wrap_before(m_cursor.row);
    mark_terminal_content_changed();
    count = std::clamp(count, 1, m_scroll_bottom - m_cursor.row + 1);
    std::move_backward(
        active_grid_rows().begin() + m_cursor.row,
        active_grid_rows().begin() + m_scroll_bottom - count + 1,
        active_grid_rows().begin() + m_scroll_bottom + 1);

    for (int row = m_cursor.row; row < m_cursor.row + count; ++row) {
        replace_row_with_erased_retained_line(active_grid_rows()[static_cast<std::size_t>(row)]);
    }

    break_soft_wrap_before(m_scroll_bottom + 1);
    m_pending_wrap = false;
    mark_dirty_rows(m_cursor.row, m_scroll_bottom);
}

void Terminal_screen_model::delete_lines(int count)
{
    if (m_cursor.row < m_scroll_top || m_cursor.row > m_scroll_bottom) {
        return;
    }

    break_soft_wrap_before(m_cursor.row);
    mark_terminal_content_changed();
    count = std::clamp(count, 1, m_scroll_bottom - m_cursor.row + 1);
    break_soft_wrap_before(m_scroll_bottom + 1);
    std::move(
        active_grid_rows().begin() + m_cursor.row + count,
        active_grid_rows().begin() + m_scroll_bottom + 1,
        active_grid_rows().begin() + m_cursor.row);

    for (int row = m_scroll_bottom - count + 1; row <= m_scroll_bottom; ++row) {
        replace_row_with_erased_retained_line(active_grid_rows()[static_cast<std::size_t>(row)]);
    }

    m_pending_wrap = false;
    mark_dirty_rows(m_cursor.row, m_scroll_bottom);
}

terminal_grid_position_t Terminal_screen_model::cell_base_position(
    terminal_grid_position_t position) const
{
    if (!active_grid_rows()[position.row].cells[position.column].wide_continuation) {
        return position;
    }

    for (int column = position.column - 1; column >= 0; --column) {
        const Cell& cell = active_grid_rows()[position.row].cells[column];
        if (!cell.wide_continuation && position.column - column < cell.display_width) {
            return {position.row, column};
        }
    }

    return position;
}

void Terminal_screen_model::set_cursor_after_cell(
    terminal_grid_position_t   position,
    int                        display_width)
{
    mark_cursor_dirty();

    if (display_width >= m_config.grid_size.columns - position.column) {
        m_cursor.row    = position.row;
        m_cursor.column = m_config.grid_size.columns - 1;
        m_pending_wrap  = m_modes.autowrap;
        mark_cursor_dirty();
        return;
    }

    m_cursor.row    = position.row;
    m_cursor.column = position.column + display_width;
    m_pending_wrap  = false;
    mark_cursor_dirty();
}

void Terminal_screen_model::set_cursor_position(int row, int column)
{
    mark_cursor_dirty();
    m_cursor.row    = std::clamp(row,    0, m_config.grid_size.rows - 1);
    m_cursor.column = std::clamp(column, 0, m_config.grid_size.columns - 1);
    m_pending_wrap  = false;
    mark_cursor_dirty();
}

void Terminal_screen_model::set_cursor_address(int row_parameter, int column_parameter)
{
    const int requested_row    = std::max(row_parameter, 1) - 1;
    const int requested_column = std::max(column_parameter, 1) - 1;

    int target_row = requested_row;
    if (m_origin_mode) {
        target_row = m_scroll_top + requested_row;
        target_row = std::clamp(target_row, m_scroll_top, m_scroll_bottom);
    }
    else {
        target_row = std::clamp(target_row, 0, m_config.grid_size.rows - 1);
    }

    if (m_primary_repaint_recovery_candidate.active) {
        m_primary_repaint_recovery_candidate.pending_non_home_addressed_row =
            (requested_row != 0 || requested_column != 0) ? target_row : -1;
    }

    set_cursor_position(target_row, requested_column);
}

void Terminal_screen_model::move_cursor_relative(int row_delta, int column_delta)
{
    int target_row = m_cursor.row + row_delta;
    if (row_delta    != 0            &&
        m_cursor.row >= m_scroll_top &&
        m_cursor.row <= m_scroll_bottom)
    {
        target_row = std::clamp(target_row, m_scroll_top, m_scroll_bottom);
    }

    set_cursor_position(target_row, m_cursor.column + column_delta);
}

void Terminal_screen_model::set_scroll_region(int top_parameter, int bottom_parameter)
{
    const int defaulted_top_parameter =
        top_parameter <= 0 ? 1 : top_parameter;
    const int defaulted_bottom_parameter =
        bottom_parameter <= 0 ? m_config.grid_size.rows : bottom_parameter;
    const int top = std::clamp(
        defaulted_top_parameter - 1,
        0,
        m_config.grid_size.rows - 1);
    const int bottom = std::clamp(
        defaulted_bottom_parameter - 1,
        0,
        m_config.grid_size.rows - 1);

    if (top >= bottom) {
        return;
    }

    if (m_scroll_top != top || m_scroll_bottom != bottom) {
        cancel_primary_repaint_recovery_candidate();
    }
    m_scroll_top = top;
    m_scroll_bottom = bottom;
    set_cursor_address(1, 1);
}

void Terminal_screen_model::set_origin_mode(bool enabled)
{
    if (m_origin_mode == enabled) {
        set_cursor_address(1, 1);
        return;
    }

    cancel_primary_repaint_recovery_candidate();
    m_origin_mode = enabled;
    m_modes.origin_mode = enabled;
    set_cursor_address(1, 1);
    mark_mode_state_changed();
}

void Terminal_screen_model::set_autowrap_mode(bool enabled)
{
    if (m_modes.autowrap == enabled) {
        return;
    }

    m_modes.autowrap = enabled;
    if (!enabled) {
        m_pending_wrap = false;
    }
    mark_mode_state_changed();
}

void Terminal_screen_model::set_application_keypad_mode(bool enabled)
{
    if (m_application_keypad == enabled) {
        return;
    }

    m_application_keypad = enabled;
}

void Terminal_screen_model::set_hyperlink(QByteArray identity_key)
{
    m_current_hyperlink_id = identity_key.isEmpty()
        ? k_no_terminal_hyperlink_id
        : active_hyperlink_id_for_identity(identity_key);
}

void Terminal_screen_model::set_synchronized_output_mode(
    bool                           enabled,
    ingest_publication_t*          publication)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_screen_model::set_synchronized_output_mode");

    if (m_modes.synchronized_output == enabled) {
        return;
    }

    if (enabled) {
        if (publication != nullptr) {
            publish_pending_changes(*publication);
        }
        m_modes.synchronized_output = true;
        mark_mode_state_changed();
        if (publication != nullptr) {
            publish_pending_changes(*publication);
        }
        return;
    }

    finish_primary_repaint_recovery_candidate(false);
    if (publication != nullptr) {
        collect_synchronized_changes();
    }
    m_modes.synchronized_output = false;
    mark_mode_state_changed();
    if (publication != nullptr) {
        collect_synchronized_changes();
        release_synchronized_changes(*publication);
    }
}

void Terminal_screen_model::apply_dec_private_mode(
    int                            mode,
    bool                           enabled,
    std::vector<Parser_action>&    generated_actions,
    const Parser_control_sequence& sequence,
    ingest_publication_t*          publication)
{
    for (const Simple_dec_private_mode& entry : k_simple_dec_private_modes) {
        if (entry.mode != mode)              { continue; }
        if (m_modes.*entry.field == enabled) { return;   }
        m_modes.*entry.field = enabled;
        switch (entry.notify) {
            case Dec_private_mode_notify::MODE_STATE:
                mark_mode_state_changed();
                return;
            case Dec_private_mode_notify::MOUSE_REPORTING:
                mark_mouse_reporting_mode_changed();
                return;
            case Dec_private_mode_notify::ALTERNATE_SCROLL:
                mark_alternate_scroll_mode_changed();
                return;
        }
        return;
    }

    for (int unsupported : k_permanently_reset_dec_private_modes) {
        if (unsupported == mode) {
            generated_actions.push_back(make_private_mode_diagnostic(mode, sequence));
            return;
        }
    }

    switch (mode) {
        case 6:
            set_origin_mode(enabled);
            return;
        case 7:
            set_autowrap_mode(enabled);
            return;
        case 2027:
            generated_actions.push_back(make_private_mode_diagnostic(mode, sequence));
            return;
        case 66:
            // This follows the xterm DECNKM polarity used by modern TUI software.
            set_application_keypad_mode(enabled);
            return;
        case 80:
            // DECSDM set places sixel images at the page home without
            // scrolling, the polarity of xterm and OpenConsole.
            m_sixel_display_mode = enabled;
            return;
        case 1070:
            // Sixel color registers are always private to each image.
            generated_actions.push_back(make_private_mode_diagnostic(mode, sequence));
            return;
        case 47:
            if (enabled) {
                enter_alternate_screen(false, mode);
            }
            else {
                (void)leave_alternate_screen(false);
            }
            return;
        case 1047:
            if (enabled) {
                enter_alternate_screen(true, mode);
            }
            else {
                (void)leave_alternate_screen(true);
            }
            return;
        case 1048:
            if (enabled) {
                save_cursor();
            }
            else {
                restore_cursor();
            }
            return;
        case 1049:
            if (enabled) {
                if (m_active_buffer_id == Terminal_buffer_id::PRIMARY) {
                    save_cursor();
                    m_dec_1049_saved_primary_cursor = true;
                }
                enter_alternate_screen(true, mode);
            }
            else {
                const bool restore_primary_cursor =
                    m_active_buffer_id == Terminal_buffer_id::ALTERNATE &&
                    m_active_alternate_mode == 1049                     &&
                    m_dec_1049_saved_primary_cursor;
                (void)leave_alternate_screen(true);
                if (restore_primary_cursor) {
                    restore_cursor();
                }
                m_dec_1049_saved_primary_cursor = false;
            }
            return;
        case 1000:
            apply_mouse_tracking_mode(Terminal_mouse_tracking_mode::BUTTON, enabled);
            return;
        case 1002:
            apply_mouse_tracking_mode(Terminal_mouse_tracking_mode::DRAG, enabled);
            return;
        case 1003:
            apply_mouse_tracking_mode(Terminal_mouse_tracking_mode::ANY, enabled);
            return;
        case 2026:
            set_synchronized_output_mode(enabled, publication);
            return;
        default:
            generated_actions.push_back(make_unsupported_control_diagnostic(sequence));
            return;
    }
}

void Terminal_screen_model::apply_mouse_tracking_mode(
    Terminal_mouse_tracking_mode   target,
    bool                           enabled)
{
    const Terminal_mouse_tracking_mode previous = m_modes.mouse_tracking;
    if (enabled) {
        m_modes.mouse_tracking = target;
    }
    else
    if (m_modes.mouse_tracking == target)   { m_modes.mouse_tracking = Terminal_mouse_tracking_mode::NONE; }
    if (m_modes.mouse_tracking != previous) { mark_mouse_reporting_mode_changed();                         }
}

int Terminal_screen_model::dec_private_mode_status(int mode) const
{
    for (const Simple_dec_private_mode& entry : k_simple_dec_private_modes) {
        if (entry.mode == mode) {
            return m_modes.*entry.field ? 1 : 2;
        }
    }

    for (int unsupported : k_permanently_reset_dec_private_modes) {
        if (unsupported == mode) {
            return 4;
        }
    }

    switch (mode) {
        case 6:
            return m_origin_mode ? 1 : 2;
        case 7:
            return m_modes.autowrap ? 1 : 2;
        case 66:
            return m_application_keypad ? 1 : 2;
        case 80:
            return m_sixel_display_mode ? 1 : 2;
        case 1070:
            return 3;
        case 47:
        case 1047:
        case 1049: return m_active_buffer_id == Terminal_buffer_id::ALTERNATE ? 1 : 2;
        case 1048: return 2;
        case 1000: return m_modes.mouse_tracking == Terminal_mouse_tracking_mode::BUTTON ? 1 : 2;
        case 1002: return m_modes.mouse_tracking == Terminal_mouse_tracking_mode::DRAG ? 1 : 2;
        case 1003: return m_modes.mouse_tracking == Terminal_mouse_tracking_mode::ANY ? 1 : 2;
        case 2026: return m_modes.synchronized_output ? 1 : 2;
        default:   return 0;
    }
}

void Terminal_screen_model::enter_alternate_screen(bool clear_alternate, int active_mode)
{
    cancel_primary_repaint_recovery_candidate();
    if (m_active_buffer_id == Terminal_buffer_id::PRIMARY) {
        const Terminal_buffer_id active_buffer_before = m_active_buffer_id;
        save_active_buffer_state();
        if (clear_alternate) {
            m_alternate_grid.active_grid_state() = make_empty_buffer_state();
        }
        m_active_buffer_id = Terminal_buffer_id::ALTERNATE;
        record_mode_transition_delta(active_buffer_before, m_active_buffer_id);
        mark_active_buffer_changed();
        restore_buffer_state(m_alternate_grid.active_grid_state());
    }
    else
    if (clear_alternate) {
        m_alternate_grid.active_grid_state() = make_empty_buffer_state();
        mark_terminal_content_changed();
        restore_buffer_state(m_alternate_grid.active_grid_state());
    }
    else
    if (m_active_alternate_mode == active_mode) {
        return;
    }

    m_active_alternate_mode = active_mode;
    mark_all_dirty();
    mark_viewport_changed();
}

bool Terminal_screen_model::leave_alternate_screen(bool clear_alternate)
{
    cancel_primary_repaint_recovery_candidate();
    if (m_active_buffer_id != Terminal_buffer_id::ALTERNATE) {
        m_active_alternate_mode = 0;
        return false;
    }

    save_active_buffer_state();
    if (clear_alternate) {
        m_alternate_grid.active_grid_state() = make_empty_buffer_state();
    }
    const Terminal_buffer_id active_buffer_before = m_active_buffer_id;
    m_active_buffer_id = Terminal_buffer_id::PRIMARY;
    record_mode_transition_delta(active_buffer_before, m_active_buffer_id);
    mark_active_buffer_changed();
    restore_buffer_state(m_primary_backing.active_grid_state());
    m_active_alternate_mode = 0;
    m_dec_1049_saved_primary_cursor = false;
    mark_all_dirty();
    mark_viewport_changed();
    return true;
}

void Terminal_screen_model::save_cursor()
{
    m_saved_cursor.position     = m_cursor;
    m_saved_cursor.style        = m_current_style;
    m_saved_cursor.style_id     = m_current_style_id;
    m_saved_cursor.pending_wrap = m_pending_wrap;
    m_saved_cursor.origin_mode  = m_origin_mode;
    m_saved_cursor.valid        = true;
}

void Terminal_screen_model::restore_cursor()
{
    if (!m_saved_cursor.valid) {
        return;
    }

    mark_cursor_dirty();
    const bool previous_origin_mode = m_origin_mode;
    m_cursor.row        = std::clamp(m_saved_cursor.position.row, 0, m_config.grid_size.rows - 1);
    m_cursor.column     = std::clamp(
        m_saved_cursor.position.column,
        0,
        m_config.grid_size.columns - 1);
    m_current_style     = m_saved_cursor.style;
    m_current_style_id  = m_saved_cursor.style_id;
    m_pending_wrap      = m_saved_cursor.pending_wrap;
    m_origin_mode       = m_saved_cursor.origin_mode;
    m_modes.origin_mode = m_origin_mode;
    if (m_origin_mode != previous_origin_mode) {
        mark_mode_state_changed();
    }
    mark_cursor_dirty();
}

void Terminal_screen_model::clear_current_tab_stop()
{
    if (m_cursor.column >= 0 && m_cursor.column < static_cast<int>(m_tab_stops.size())) {
        m_tab_stops[static_cast<std::size_t>(m_cursor.column)] = false;
    }
}

void Terminal_screen_model::clear_all_tab_stops()
{
    std::fill(m_tab_stops.begin(), m_tab_stops.end(), false);
}

std::optional<terminal_history_handle_t> Terminal_screen_model::append_scrollback_row(
    const Terminal_screen_row&               row,
    Terminal_retained_line_provenance_source source,
    const std::map<Terminal_hyperlink_id, QByteArray>* hyperlink_identity_keys,
    const terminal_hyperlink_identity_by_id_t* active_hyperlink_identity_keys_by_id)
{
    std::optional<terminal_history_handle_t> appended_handle;
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled && m_text_span_profile_depth > 0) {
        ++m_profile_stats.scrollback_appends_from_text_writes;
    }
#endif
    mark_terminal_content_changed();
    if (m_config.scrollback_limit > 0) {
        const int scrollback_rows_before = scrollback_size();
        retained_history_append_result_t append;
        {
            VNM_TERMINAL_PROFILE_SCOPE(
                "Terminal_screen_model::append_scrollback_row::retained_history_append");
            append = m_primary_backing.append_retained_history_record(
                seal_retained_row_record(
                    row,
                    source,
                    hyperlink_identity_keys,
                    active_hyperlink_identity_keys_by_id));
        }
        if (append.image_slice_dropped && interaction_trace_enabled()) {
            record_interaction_trace(
                "history",
                "image-slice-dropped",
                QStringLiteral("retained_line_id=%1 image_bytes=%2 max_record_bytes=%3")
                    .arg(row.retained_line_provenance.retained_line_id)
                    .arg(terminal_history_row_record_image_section_bytes(*row.image_slice))
                    .arg(m_primary_backing.retained_history.ring->max_record_bytes()));
        }
        if (append.record_discarded) {
            ++m_scrollback_evicted_rows;
            record_primary_history_delta(
                Terminal_backing_delta_kind::PRIMARY_HISTORY_DISCARDED,
                scrollback_rows_before,
                scrollback_size(),
                0,
                0,
                1);
        }
        else {
            appended_handle = append.appended_handle;
            if (append.appended_handle.has_value()) {
                insert_primary_history_retained_lookup(
                    *append.appended_handle,
                    append.appended_ordinal);
            }
            for (const terminal_history_handle_t evicted_handle : append.evicted_handles) {
                erase_retained_lookup_entry(
                    Terminal_buffer_id::PRIMARY,
                    evicted_handle.row_sequence);
            }
            const int evicted_rows = static_cast<int>(append.evicted_handles.size());
            m_scrollback_evicted_rows += evicted_rows;
            if (evicted_rows > 0 && interaction_trace_enabled()) {
                const terminal_retained_history_diagnostics_t diagnostics =
                    retained_history_diagnostics();
                record_interaction_trace(
                    "history",
                    "ring-eviction",
                    QStringLiteral(
                        "evicted_rows=%1 retained_rows=%2 record_bytes=%3 byte_capacity=%4 row_limit=%5")
                        .arg(evicted_rows)
                        .arg(diagnostics.retained_rows)
                        .arg(diagnostics.retained_record_bytes)
                        .arg(diagnostics.byte_budget)
                        .arg(m_config.scrollback_limit));
            }
            record_primary_history_delta(
                Terminal_backing_delta_kind::PRIMARY_HISTORY_APPENDED,
                scrollback_rows_before,
                scrollback_size(),
                1,
                evicted_rows,
                0);
        }
        while (scrollback_size() > m_config.scrollback_limit) {
            const int rows_to_evict = scrollback_size() - m_config.scrollback_limit;
            if (interaction_trace_enabled()) {
                const terminal_retained_history_diagnostics_t diagnostics =
                    retained_history_diagnostics();
                record_interaction_trace(
                    "history",
                    "row-limit-eviction",
                    QStringLiteral(
                        "evict_rows=%1 retained_rows=%2 record_bytes=%3 byte_capacity=%4 row_limit=%5")
                        .arg(rows_to_evict)
                        .arg(diagnostics.retained_rows)
                        .arg(diagnostics.retained_record_bytes)
                        .arg(diagnostics.byte_budget)
                        .arg(m_config.scrollback_limit));
            }
            evict_oldest_scrollback_rows(
                rows_to_evict);
        }
    }
    else {
        ++m_scrollback_evicted_rows;
        record_primary_history_delta(
            Terminal_backing_delta_kind::PRIMARY_HISTORY_DISCARDED,
            scrollback_size(),
            scrollback_size(),
            0,
            0,
            1);
    }
    mark_viewport_changed();
    return appended_handle;
}

void Terminal_screen_model::scroll_up_region(
    int    top,
    int    bottom,
    bool   append_scrollback,
    int    count)
{
    break_soft_wrap_before(top);
    mark_terminal_content_changed();
    count = std::clamp(count, 1, bottom - top + 1);
    std::optional<terminal_hyperlink_identity_by_id_t> active_identity_keys_by_id;
    if (append_scrollback) {
        bool scrolled_rows_have_hyperlinks = false;
        for (int row = top; row < top + count; ++row) {
            for (const Cell& cell : active_grid_rows()[static_cast<std::size_t>(row)].cells) {
                if (cell.hyperlink_id != k_no_terminal_hyperlink_id) {
                    scrolled_rows_have_hyperlinks = true;
                    break;
                }
            }
            if (scrolled_rows_have_hyperlinks) {
                break;
            }
        }

        if (scrolled_rows_have_hyperlinks) {
            active_identity_keys_by_id = active_hyperlink_identity_keys_by_id();
        }
    }

    const terminal_hyperlink_identity_by_id_t* active_identity_keys =
        active_identity_keys_by_id.has_value()
            ? &*active_identity_keys_by_id
            : nullptr;
    for (int step = 0; step < count; ++step) {
        if (append_scrollback) {
            append_scrollback_row(
                active_grid_rows()[static_cast<std::size_t>(top)],
                Terminal_retained_line_provenance_source::TERMINAL_STORAGE,
                nullptr,
                active_identity_keys);
        }

        for (int row = top + 1; row <= bottom; ++row) {
            active_grid_rows()[static_cast<std::size_t>(row - 1)] =
                std::move(active_grid_rows()[static_cast<std::size_t>(row)]);
        }
        replace_row_with_erased_retained_line(active_grid_rows()[static_cast<std::size_t>(bottom)]);
    }

    mark_dirty_rows(top, bottom);
}

void Terminal_screen_model::scroll_down_region(int top, int bottom, int count)
{
    break_soft_wrap_before(top);
    mark_terminal_content_changed();
    count = std::clamp(count, 1, bottom - top + 1);
    for (int step = 0; step < count; ++step) {
        for (int row = bottom - 1; row >= top; --row) {
            active_grid_rows()[static_cast<std::size_t>(row + 1)] =
                std::move(active_grid_rows()[static_cast<std::size_t>(row)]);
        }
        replace_row_with_erased_retained_line(active_grid_rows()[static_cast<std::size_t>(top)]);
    }

    break_soft_wrap_before(bottom + 1);
    mark_dirty_rows(top, bottom);
}

void Terminal_screen_model::reverse_index()
{
    mark_cursor_dirty();
    m_pending_wrap = false;

    if (m_cursor.row == m_scroll_top) {
        scroll_down_region(m_scroll_top, m_scroll_bottom);
        mark_cursor_dirty();
        return;
    }

    if (m_cursor.row > 0) {
        --m_cursor.row;
    }
    mark_cursor_dirty();
}

void Terminal_screen_model::arm_primary_repaint_recovery_resize_guard()
{
    if (!m_config.recover_scrollback_from_primary_repaints ||
        m_active_buffer_id != Terminal_buffer_id::PRIMARY)
    {
        cancel_primary_repaint_recovery_resize_guard();
        return;
    }

    m_primary_repaint_recovery_resize_guard_remaining =
        k_primary_repaint_recovery_resize_guard_action_budget;
}

void Terminal_screen_model::cancel_primary_repaint_recovery_resize_guard()
{
    m_primary_repaint_recovery_resize_guard_remaining = 0;
}

void Terminal_screen_model::advance_primary_repaint_recovery_resize_guard()
{
    if (m_primary_repaint_recovery_resize_guard_remaining <= 0) {
        return;
    }

    --m_primary_repaint_recovery_resize_guard_remaining;
}

void Terminal_screen_model::begin_primary_repaint_recovery_candidate()
{
    const bool resize_repaint_guard_active =
        m_primary_repaint_recovery_resize_guard_remaining > 0;

    if (!m_config.recover_scrollback_from_primary_repaints ||
        m_active_buffer_id != Terminal_buffer_id::PRIMARY  ||
        m_origin_mode                                      ||
        m_modes.cursor_visible                             ||
        resize_repaint_guard_active                        ||
        m_scroll_top       != 0                            ||
        m_scroll_bottom    != m_config.grid_size.rows - 1)
    {
        cancel_primary_repaint_recovery_candidate();
        return;
    }

    if (m_primary_repaint_recovery_candidate.active) {
        finish_primary_repaint_recovery_candidate(true);
    }

    m_primary_repaint_recovery_candidate.rows = active_grid_rows();
    m_primary_repaint_recovery_candidate.text_rows.clear();
    m_primary_repaint_recovery_candidate.text_rows.reserve(
        m_primary_repaint_recovery_candidate.rows.size());
    m_primary_repaint_recovery_candidate.text_row_columns =
        m_config.grid_size.columns;

    bool has_visible_row = false;
    for (const Terminal_screen_row& row :
         m_primary_repaint_recovery_candidate.rows)
    {
        QString text = row_text_from_cells(
            row.cells,
            0,
            m_primary_repaint_recovery_candidate.text_row_columns);
        has_visible_row = has_visible_row || !text.isEmpty();
        m_primary_repaint_recovery_candidate.text_rows.push_back(std::move(text));
    }
    if (!has_visible_row) {
        cancel_primary_repaint_recovery_candidate();
        return;
    }

    m_primary_repaint_recovery_candidate.hyperlink_identity_keys.clear();
    const terminal_hyperlink_identity_by_id_t active_identity_keys_by_id =
        active_hyperlink_identity_keys_by_id();
    for (const Terminal_screen_row& row : m_primary_repaint_recovery_candidate.rows) {
        for (const Cell& cell : row.cells) {
            if (cell.hyperlink_id == k_no_terminal_hyperlink_id) {
                continue;
            }

            const auto found = active_identity_keys_by_id.find(cell.hyperlink_id);
            if (found != active_identity_keys_by_id.end()) {
                m_primary_repaint_recovery_candidate.hyperlink_identity_keys[
                    cell.hyperlink_id] = found->second;
            }
        }
    }
    m_primary_repaint_recovery_candidate.scrollback_rows              = scrollback_size();
    m_primary_repaint_recovery_candidate.unmatched_finish_budget      = 1;
    m_primary_repaint_recovery_candidate.pending_non_home_addressed_row = -1;
    m_primary_repaint_recovery_candidate.line_start_clear_before_text = false;
    m_primary_repaint_recovery_candidate.explicit_non_home_repaint_address =
        false;
    m_primary_repaint_recovery_candidate.visible_row_identity_ambiguous =
        false;
    m_primary_repaint_recovery_candidate.active                       = true;
}

void Terminal_screen_model::finish_primary_repaint_recovery_candidate(
    bool discard_if_no_match)
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_screen_model::finish_primary_repaint_recovery_candidate");

    if (!m_primary_repaint_recovery_candidate.active) {
        return;
    }

    primary_repaint_recovery_candidate_t candidate =
        std::move(m_primary_repaint_recovery_candidate);
    m_primary_repaint_recovery_candidate = {};

    const terminal_repaint_recovery_shift_result_t shift =
        primary_repaint_recovery_shift(candidate);
    std::optional<primary_repaint_recovery_proposal_t> proposal =
        primary_repaint_recovery_proposal(candidate, shift);
    if (!proposal.has_value()) {
        record_recovery_attempt({
            Terminal_recovery_attempt_status::REJECTED,
            shift.rejection_kind ==
                    Terminal_repaint_recovery_rejection_kind::REPEATED_ROW_AMBIGUOUS
                ? Terminal_recovery_attempt_reason::REPEATED_ROW_AMBIGUOUS
                : Terminal_recovery_attempt_reason::NONMATCHING,
            static_cast<int>(candidate.rows.size()),
            0,
        });
        if (candidate.visible_row_identity_ambiguous) {
            terminal_selection_continuity_capability_t& continuity =
                selection_continuity();
            std::vector<Terminal_screen_row>& active_rows = active_grid_rows();
            const int proof_rows = std::min(
                static_cast<int>(candidate.rows.size()),
                static_cast<int>(active_rows.size()));
            for (int row = 0; row < proof_rows; ++row) {
                const terminal_history_handle_t old_handle =
                    retained_history_handle_from_provenance(
                        candidate.rows[static_cast<std::size_t>(row)].retained_line_provenance);
                Terminal_screen_row& target = active_rows[static_cast<std::size_t>(row)];
                rebase_retained_line_id_preserving_content(target);
                continuity.survivor_proofs.push_back({
                    old_handle,
                    {},
                    retained_history_handle_from_provenance(
                        target.retained_line_provenance),
                    Terminal_selection_survivor_proof_result::PROOF_UNAVAILABLE,
                    candidate.scrollback_rows + row,
                    candidate.scrollback_rows + row,
                });
            }
            mark_all_dirty();
            candidate.visible_row_identity_ambiguous = false;
        }
        if (!discard_if_no_match && candidate.unmatched_finish_budget > 0) {
            --candidate.unmatched_finish_budget;
            m_primary_repaint_recovery_candidate = std::move(candidate);
        }
        else {
            m_primary_repaint_recovery_episode_active = false;
        }
        return;
    }

    Terminal_recovery_attempt_reason accepted_reason =
        Terminal_recovery_attempt_reason::PARTIAL_SHIFT_MATCH;
    if (shift.match_kind == Terminal_repaint_recovery_match_kind::FULL) {
        accepted_reason = Terminal_recovery_attempt_reason::FULL_SHIFT_MATCH;
    }
    else if (shift.match_kind ==
        Terminal_repaint_recovery_match_kind::PARTIAL_ANCHORED)
    {
        accepted_reason =
            Terminal_recovery_attempt_reason::PARTIAL_ANCHORED_SHIFT_MATCH;
    }
    record_recovery_attempt({
        Terminal_recovery_attempt_status::ACCEPTED,
        accepted_reason,
        static_cast<int>(candidate.rows.size()),
        shift.shifted_rows,
    });
    accept_primary_repaint_recovery_proposal(*proposal);
    std::vector<Terminal_screen_row>& active_rows = active_grid_rows();
    terminal_selection_continuity_capability_t& continuity = selection_continuity();
    for (int row = 0; row < static_cast<int>(active_rows.size()); ++row) {
        Terminal_screen_row& target = active_rows[static_cast<std::size_t>(row)];
        int predecessor_row = shift.shifted_rows + row;
        bool matched_candidate = row < shift.matched_prefix_rows &&
            predecessor_row < static_cast<int>(candidate.rows.size());
        if (shift.match_kind ==
            Terminal_repaint_recovery_match_kind::PARTIAL_ANCHORED)
        {
            const int anchored_suffix_start =
                static_cast<int>(active_rows.size()) - shift.anchored_suffix_rows;
            if (row >= anchored_suffix_start) {
                predecessor_row = row;
                matched_candidate =
                    predecessor_row < static_cast<int>(candidate.rows.size());
            }
            else if (row >= shift.matched_prefix_rows) {
                predecessor_row = -1;
                matched_candidate = false;
            }
        }

        terminal_history_handle_t old_handle;
        Terminal_selection_survivor_proof_result proof_result =
            Terminal_selection_survivor_proof_result::UNMATCHED_TAIL;
        if (predecessor_row >= 0 &&
            predecessor_row < static_cast<int>(candidate.rows.size()))
        {
            const Terminal_screen_row& predecessor =
                candidate.rows[static_cast<std::size_t>(predecessor_row)];
            old_handle = retained_history_handle_from_provenance(
                predecessor.retained_line_provenance);
            if (matched_candidate) {
                if (!rows_have_same_selection_content(predecessor.cells, target.cells)) {
                    proof_result =
                        Terminal_selection_survivor_proof_result::REPRESENTATION_MISMATCH;
                }
                else {
                    proof_result = Terminal_selection_survivor_proof_result::EXACT;
                }
            }
        }

        rebase_retained_line_id_preserving_content(target);
        const std::uint64_t final_retained_line_id =
            target.retained_line_provenance.retained_line_id;
        if (proof_result == Terminal_selection_survivor_proof_result::EXACT) {
            const Terminal_retained_line_provenance& predecessor_provenance =
                candidate.rows[static_cast<std::size_t>(predecessor_row)].retained_line_provenance;
            target.retained_line_provenance = predecessor_provenance;
            target.retained_line_provenance.retained_line_id = final_retained_line_id;
            transfer_selection_cell_watches(
                old_handle.row_sequence, final_retained_line_id, predecessor_provenance.content_generation);
        }
        const terminal_history_handle_t final_handle =
            retained_history_handle_from_provenance(target.retained_line_provenance);

        if (terminal_history_handle_has_identity(old_handle)) {
            continuity.survivor_proofs.push_back({
                old_handle,
                old_handle,
                final_handle,
                proof_result,
                candidate.scrollback_rows + predecessor_row,
                scrollback_size() + row,
            });
        }
        if (proof_result == Terminal_selection_survivor_proof_result::EXACT) {
            const int old_logical_row   = candidate.scrollback_rows + predecessor_row;
            const int final_logical_row = scrollback_size() + row;
            // The delta the relation publishes is a logical-row delta, the same
            // one finalize_selection_continuity_rows() writes back and the same
            // one the session composes across publications. Recording the
            // visible-row shift here instead left the two disagreeing on every
            // relation the finalizer could not reach, which is exactly the
            // relation a later recovery in the same ingest supersedes.
            record_selection_successor({
                old_handle,
                final_handle,
                old_logical_row,
                final_logical_row,
                final_logical_row - old_logical_row,
            });
        }
    }
    mark_all_dirty();
    m_primary_repaint_recovery_episode_active = true;
}

void Terminal_screen_model::cancel_primary_repaint_recovery_candidate(
    Terminal_recovery_attempt_reason reason)
{
    const bool had_active_candidate = m_primary_repaint_recovery_candidate.active;
    const int candidate_visible_rows = static_cast<int>(
        m_primary_repaint_recovery_candidate.rows.size());
    if (m_primary_repaint_recovery_candidate.active &&
        m_primary_repaint_recovery_candidate.visible_row_identity_ambiguous)
    {
        terminal_selection_continuity_capability_t& continuity =
            selection_continuity();
        std::vector<Terminal_screen_row>& active_rows = active_grid_rows();
        const int proof_rows = std::min(
            candidate_visible_rows,
            static_cast<int>(active_rows.size()));
        for (int row = 0; row < proof_rows; ++row) {
            const terminal_history_handle_t old_handle =
                retained_history_handle_from_provenance(
                    m_primary_repaint_recovery_candidate.rows[
                        static_cast<std::size_t>(row)].retained_line_provenance);
            Terminal_screen_row& target = active_rows[static_cast<std::size_t>(row)];
            rebase_retained_line_id_preserving_content(target);
            continuity.survivor_proofs.push_back({
                old_handle,
                {},
                retained_history_handle_from_provenance(
                    target.retained_line_provenance),
                Terminal_selection_survivor_proof_result::PROOF_UNAVAILABLE,
                m_primary_repaint_recovery_candidate.scrollback_rows + row,
                scrollback_size() + row,
            });
        }
        mark_all_dirty();
    }
    m_primary_repaint_recovery_candidate = {};
    m_primary_repaint_recovery_episode_active = false;
    if (had_active_candidate) {
        record_recovery_attempt({
            Terminal_recovery_attempt_status::CANCELLED,
            reason,
            candidate_visible_rows,
            0,
        });
    }
}

void Terminal_screen_model::accept_primary_repaint_recovery_proposal(
    const primary_repaint_recovery_proposal_t& proposal)
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_screen_model::accept_primary_repaint_recovery_proposal");

    std::optional<terminal_hyperlink_identity_by_id_t> active_identity_keys_by_id;
    bool needs_active_identity_lookup = false;
    for (const Terminal_screen_row& row : proposal.rows) {
        for (const Cell& cell : row.cells) {
            if (cell.hyperlink_id != k_no_terminal_hyperlink_id &&
                proposal.hyperlink_identity_keys.find(cell.hyperlink_id) ==
                    proposal.hyperlink_identity_keys.end())
            {
                needs_active_identity_lookup = true;
                break;
            }
        }
        if (needs_active_identity_lookup) {
            break;
        }
    }
    if (needs_active_identity_lookup) {
        active_identity_keys_by_id = active_hyperlink_identity_keys_by_id();
    }

    const terminal_hyperlink_identity_by_id_t* active_identity_keys =
        active_identity_keys_by_id.has_value()
            ? &*active_identity_keys_by_id
            : nullptr;
    for (int row = 0; row < static_cast<int>(proposal.rows.size()); ++row) {
        const Terminal_screen_row& recovered_row =
            proposal.rows[static_cast<std::size_t>(row)];
        const terminal_history_handle_t old_handle =
            retained_history_handle_from_provenance(
                recovered_row.retained_line_provenance);
        const std::optional<terminal_history_handle_t> final_handle = append_scrollback_row(
            recovered_row,
            proposal.metadata.provenance_source,
            &proposal.hyperlink_identity_keys,
            active_identity_keys);
        terminal_selection_survivor_proof_t proof;
        proof.old_handle                   = old_handle;
        proof.candidate_predecessor_handle = old_handle;
        proof.old_logical_row = proposal.source_scrollback_rows + row;
        proof.final_logical_row = scrollback_size() - 1;
        if (final_handle.has_value()) {
            proof.final_handle = *final_handle;
            const Terminal_retained_line_lookup_result lookup = retained_line_lookup(
                Terminal_buffer_id::PRIMARY,
                *final_handle);
            const std::optional<std::vector<Cell>> final_cells = lookup.exact_match &&
                    lookup.retained_line_id_match_count == 1
                ? logical_row_cells(Terminal_buffer_id::PRIMARY, lookup.exact_logical_row)
                : std::nullopt;
            if (!final_cells.has_value()) {
                proof.result = Terminal_selection_survivor_proof_result::PROOF_UNAVAILABLE;
            }
            else
            if (final_handle->content_generation != old_handle.content_generation) {
                proof.result = Terminal_selection_survivor_proof_result::
                    CONTENT_GENERATION_MISMATCH;
            }
            else
            if (!rows_have_same_selection_content(recovered_row.cells, *final_cells)) {
                proof.result = Terminal_selection_survivor_proof_result::
                    REPRESENTATION_MISMATCH;
            }
            else {
                proof.result = Terminal_selection_survivor_proof_result::EXACT;
                proof.final_logical_row = lookup.exact_logical_row;
                transfer_selection_cell_watches(
                    old_handle.row_sequence,
                    final_handle->row_sequence,
                    old_handle.content_generation);
                record_selection_successor({
                    old_handle,
                    *final_handle,
                    proof.old_logical_row,
                    proof.final_logical_row,
                    proof.final_logical_row - proof.old_logical_row,
                });
            }
        }
        else {
            proof.result = Terminal_selection_survivor_proof_result::PROOF_UNAVAILABLE;
        }
        selection_continuity().survivor_proofs.push_back(std::move(proof));
    }
    m_recovery_proposals.push_back(proposal.metadata);
}

std::optional<Terminal_screen_model::primary_repaint_recovery_proposal_t>
Terminal_screen_model::primary_repaint_recovery_proposal(
    const primary_repaint_recovery_candidate_t& candidate,
    const terminal_repaint_recovery_shift_result_t& shift) const
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_screen_model::primary_repaint_recovery_proposal");

    if (shift.shifted_rows <= 0) {
        return std::nullopt;
    }

    primary_repaint_recovery_proposal_t proposal;
    proposal.rows.reserve(static_cast<std::size_t>(shift.shifted_rows));
    for (int row = 0; row < shift.shifted_rows; ++row) {
        proposal.rows.push_back(candidate.rows[static_cast<std::size_t>(row)]);
    }
    proposal.hyperlink_identity_keys = candidate.hyperlink_identity_keys;

    proposal.metadata.reason =
        Terminal_recovery_proposal_reason::PRIMARY_REPAINT_SHIFTED_VISIBLE_ROWS;
    proposal.metadata.status = Terminal_recovery_proposal_status::ACCEPTED;
    proposal.metadata.provenance_source =
        Terminal_retained_line_provenance_source::RECOVERED_PRIMARY_REPAINT;
    proposal.metadata.candidate_visible_rows =
        static_cast<int>(candidate.rows.size());
    proposal.metadata.recovered_row_count = shift.shifted_rows;
    proposal.metadata.matched_prefix_rows = shift.matched_prefix_rows;
    proposal.metadata.unmatched_tail_rows = shift.unmatched_tail_rows;
    proposal.metadata.anchored_suffix_rows = shift.anchored_suffix_rows;
    proposal.metadata.visible_row_identity_ambiguous =
        candidate.visible_row_identity_ambiguous;
    proposal.source_scrollback_rows = candidate.scrollback_rows;
    return proposal;
}

terminal_repaint_recovery_shift_result_t
Terminal_screen_model::primary_repaint_recovery_shift(
    const primary_repaint_recovery_candidate_t& candidate) const
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_screen_model::primary_repaint_recovery_shift");

    terminal_repaint_recovery_shift_input_t input;
    input.candidate_active                  = candidate.active;
    input.primary_buffer_active             = m_active_buffer_id == Terminal_buffer_id::PRIMARY;
    input.scrollback_rows_unchanged         = candidate.scrollback_rows == scrollback_size();
    input.line_start_clear_before_text      = candidate.line_start_clear_before_text;
    input.explicit_non_home_repaint_address =
        candidate.explicit_non_home_repaint_address;
    input.continuing_repaint_episode =
        m_primary_repaint_recovery_episode_active;

    // Match the helper's cheap rejection gates before projecting either grid.
    if (!input.candidate_active ||
        !input.primary_buffer_active ||
        !input.scrollback_rows_unchanged ||
        candidate.rows.size() != active_grid_rows().size() ||
        (input.line_start_clear_before_text &&
            input.explicit_non_home_repaint_address))
    {
        return {};
    }

    if (candidate.text_row_columns == m_config.grid_size.columns &&
        candidate.text_rows.size() == candidate.rows.size())
    {
        input.candidate_rows = candidate.text_rows;
    }
    else {
        input.candidate_rows.reserve(candidate.rows.size());
        for (const Terminal_screen_row& row : candidate.rows) {
            input.candidate_rows.push_back(
                row_text_from_cells(row.cells, 0, m_config.grid_size.columns));
        }
    }

    input.current_rows.reserve(active_grid_rows().size());
    for (const Terminal_screen_row& row : active_grid_rows()) {
        input.current_rows.push_back(
            row_text_from_cells(row.cells, 0, m_config.grid_size.columns));
    }

    return internal::primary_repaint_recovery_shift_result(input);
}

void Terminal_screen_model::record_recovery_attempt(
    terminal_recovery_attempt_t attempt)
{
    if (m_recovery_attempts.size() < k_terminal_recovery_attempt_result_limit) {
        m_recovery_attempts.push_back(std::move(attempt));
    }
    else {
        ++m_dropped_recovery_attempts;
    }
}

bool Terminal_screen_model::row_has_visible_text(const Terminal_screen_row& row) const
{
    return !row_text_from_cells(row.cells, 0, m_config.grid_size.columns).isEmpty();
}

void Terminal_screen_model::carriage_return()
{
    mark_cursor_dirty();
    m_cursor.column = 0;
    m_pending_wrap  = false;
    mark_cursor_dirty();
}

void Terminal_screen_model::line_feed()
{
    active_grid_rows()[(std::size_t)m_cursor.row].soft_wrap_columns = 0;
    advance_row();
}

void Terminal_screen_model::wrap_line()
{
    // Image rows break incoming and outgoing text wraps (D2), so later text
    // cannot join them into another logical line. At the bottom margin the
    // wrap scrolls onto a fresh row instead.
    const int  next_row = m_cursor.row + 1;
    const bool wraps_onto_image_row =
        m_cursor.row != m_scroll_bottom         &&
        next_row     <  m_config.grid_size.rows &&
        active_grid_rows()[(std::size_t)next_row].image_slice != nullptr;
    Terminal_screen_row& row = active_grid_rows()[(std::size_t)m_cursor.row];
    int soft_wrap_columns    = m_pending_wrap ? m_config.grid_size.columns : m_cursor.column;
    if (row.image_slice != nullptr || wraps_onto_image_row) {
        soft_wrap_columns = 0;
    }
    row.soft_wrap_columns = soft_wrap_columns;
    carriage_return();
    advance_row();
}

void Terminal_screen_model::advance_row()
{
    mark_cursor_dirty();
    m_pending_wrap = false;

    if (m_cursor.row == m_scroll_bottom) {
        scroll_active_region_up();
        mark_cursor_dirty();
        return;
    }

    if (m_cursor.row < m_config.grid_size.rows - 1) {
        ++m_cursor.row;
        mark_cursor_dirty();
        return;
    }
}

// One line feed's worth of scrolling at the bottom margin: only a primary
// screen region that starts at the top row feeds history.
void Terminal_screen_model::scroll_active_region_up()
{
    scroll_up_region(
        m_scroll_top,
        m_scroll_bottom,
        m_active_buffer_id == Terminal_buffer_id::PRIMARY &&
            m_scroll_top == 0);
}

// The decoded-size cap is the retained history's largest record.
std::size_t Terminal_screen_model::sixel_raster_limit_bytes() const
{
    return terminal_history_ring_max_record_bytes(
        m_primary_backing.retained_history.capacity_bytes);
}

void Terminal_screen_model::place_sixel_image(
    const Screen_sixel_image_mutation& image,
    std::vector<Parser_action>&        generated_actions)
{
    // Only the cell pixel size says which text rows an image covers. Without
    // it the image is dropped whole, cursor movement included, the way the
    // CSI 14 t and CSI 16 t pixel reports are unsupported then.
    if (!m_config.cell_pixel_size.has_value()) {
        generated_actions.push_back(make_unsupported_sequence_diagnostic(
            QStringLiteral("DCS sixel"),
            Parser_sequence_family::DCS,
            0U,
            0U,
            Parser_recovery_strategy::DISCARD_STRING));
        return;
    }

    // With DECSDM reset the image starts at the cursor. As in OpenConsole, an
    // image that would start below the bottom margin is dropped whole.
    if (!m_sixel_display_mode && m_cursor.row > m_scroll_bottom) {
        return;
    }

    const terminal_cell_pixel_size_t cell = *m_config.cell_pixel_size;

    // As in OpenConsole, one sixel row is at most as tall as the rows the
    // image can scroll, the scroll region or with DECSDM set the page (owner
    // decision D5), which also bounds the scrolling a graphics new line
    // causes. A larger aspect ratio is clamped after decoding: each sixel
    // pixel keeps the rows the clamped ratio gives it, and the final sixel
    // row and the scrolls follow the clamped ratio.
    const std::int64_t aspect_rows = m_sixel_display_mode
        ? m_config.grid_size.rows
        : m_scroll_bottom - m_scroll_top + 1;
    Sixel_placement placement;
    placement.raster         = image.raster;
    placement.cell           = cell;
    placement.display_mode   = m_sixel_display_mode;
    placement.origin         = m_sixel_display_mode ? terminal_grid_position_t{0, 0} : m_cursor;
    placement.decoded_aspect = image.pixel_aspect_ratio;
    placement.aspect         = static_cast<int>(std::clamp<std::int64_t>(
        aspect_rows * cell.height / 6,
        1,
        placement.decoded_aspect));
    if (!image.raster.isNull()) {
        placement.width  = image.raster.width();
        placement.height = static_cast<int>(sixel_row_at_aspect(
            image.raster.height(),
            placement.decoded_aspect,
            placement.aspect));
        placement.band_count = (placement.height + cell.height - 1) / cell.height;
    }
    placement.final_cursor_y =
        sixel_row_at_aspect(image.final_cursor_y, placement.decoded_aspect, placement.aspect);

    // The VT340 puts the text cursor on the row that the top of the final
    // sixel row falls in, at the image's first column, and scrolls the region
    // just enough for that whole sixel row to fit above the bottom margin.
    // With DECSDM set the image never scrolls.
    std::int64_t placed_bands    = std::min(placement.band_count, m_config.grid_size.rows);
    std::int64_t planned_scrolls = 0;
    if (!placement.display_mode) {
        const std::int64_t final_sixel_row_bottom =
            placement.final_cursor_y + 6 * placement.aspect;
        const std::int64_t covered_rows =
            (final_sixel_row_bottom + cell.height - 1) / cell.height;
        placement.scroll_count = std::max<std::int64_t>(
            0,
            placement.origin.row + covered_rows - 1 - m_scroll_bottom);

        const std::int64_t rows_from_origin = m_scroll_bottom - placement.origin.row + 1;
        placed_bands = std::min<std::int64_t>(
            placement.band_count,
            rows_from_origin + placement.scroll_count);
        const std::int64_t band_scrolls =
            std::max<std::int64_t>(0, placed_bands - rows_from_origin);
        planned_scrolls = band_scrolls + std::min<std::int64_t>(
            placement.scroll_count - band_scrolls,
            m_scroll_bottom - m_scroll_top + 1);
    }

    // What the whole placement costs at most, from the steps it will take.
    const bool resamples = placement.aspect < placement.decoded_aspect && !image.raster.isNull();
    placement.total_cost =
        (resamples
            ? static_cast<std::uint64_t>(placement.width) *
                static_cast<std::uint64_t>(placement.height)
            : 0U) +
        static_cast<std::uint64_t>(placed_bands) * sixel_band_cost(placement, 0) +
        static_cast<std::uint64_t>(planned_scrolls) * sixel_scroll_cost();
    m_sixel_placement = std::move(placement);

    // A placement that fits what the budget has left runs now. One that does
    // not waits for an ingest of its own, which starts it with a fresh budget,
    // so a partly placed image never shares an ingest with the output before
    // it, and the caller can publish that output first.
    if (m_sixel_work_budget == nullptr ||
        m_sixel_work_budget->fits(m_sixel_placement->total_cost))
    {
        [[maybe_unused]] const bool placed = advance_sixel_placement(generated_actions);
        Q_ASSERT(placed);
    }
}

// Copying a band out of the raster and finding the cells it covers touch each
// band pixel about twice; making the slice and marking the row cost about as
// much as k_sixel_band_step_units pixels besides.
std::uint64_t Terminal_screen_model::sixel_band_cost(
    const Sixel_placement& placement,
    int                    band) const
{
    const std::int64_t band_width = std::min<std::int64_t>(
        placement.width,
        static_cast<std::int64_t>(m_config.grid_size.columns - placement.origin.column) *
            placement.cell.width);
    const std::int64_t band_height = std::min<std::int64_t>(
        placement.cell.height,
        static_cast<std::int64_t>(placement.height) -
            static_cast<std::int64_t>(band) * placement.cell.height);
    return k_sixel_band_step_units +
        2U * static_cast<std::uint64_t>(std::max<std::int64_t>(0, band_width)) *
            static_cast<std::uint64_t>(std::max<std::int64_t>(0, band_height));
}

// One region scroll: the rows move and are marked, and the row leaving the top
// of a primary region is encoded into history with any image it carries.
std::uint64_t Terminal_screen_model::sixel_scroll_cost() const
{
    const std::uint64_t region_rows =
        static_cast<std::uint64_t>(m_scroll_bottom - m_scroll_top + 1);
    const std::uint64_t row_image_pixels = m_config.cell_pixel_size.has_value()
        ? static_cast<std::uint64_t>(m_config.grid_size.columns) *
            static_cast<std::uint64_t>(m_config.cell_pixel_size->width) *
            static_cast<std::uint64_t>(m_config.cell_pixel_size->height)
        : 0U;
    return k_sixel_scroll_step_units + region_rows * k_sixel_scroll_row_units + row_image_pixels;
}

bool Terminal_screen_model::advance_sixel_placement(std::vector<Parser_action>& generated_actions)
{
    Q_ASSERT(m_sixel_placement.has_value());
    Sixel_placement&   placement = *m_sixel_placement;
    Sixel_work_budget* budget    = m_sixel_work_budget;

    if (!placement.started) {
        const bool resamples =
            placement.aspect < placement.decoded_aspect && !placement.raster.isNull();
        const std::uint64_t cost = resamples
            ? static_cast<std::uint64_t>(placement.width) *
                static_cast<std::uint64_t>(placement.height)
            : 0U;
        if (!try_charge_sixel_work(budget, cost)) {
            return false;
        }
        if (resamples) {
            placement.raster = sixel_raster_at_aspect(
                placement.raster,
                placement.decoded_aspect,
                placement.aspect);
        }
        placement.started = true;
    }

    // Each band is placed before any scroll moves it, so the bands a region
    // at the top of the screen scrolls off reach history with their rows.
    // Bands left over once the region has scrolled enough are clipped; with
    // DECSDM set they are clipped at the bottom of the page.
    const terminal_cell_pixel_size_t cell = placement.cell;
    while (!placement.bands_done && placement.next_band < placement.band_count) {
        const int band = placement.next_band;
        std::int64_t row = placement.display_mode
            ? band
            : placement.origin.row + band - placement.scrolls;
        const bool scrolls_first = !placement.display_mode && row > m_scroll_bottom;
        if ((placement.display_mode && band >= m_config.grid_size.rows) ||
            (scrolls_first && placement.scrolls == placement.scroll_count))
        {
            break;
        }
        const std::uint64_t cost =
            sixel_band_cost(placement, band) + (scrolls_first ? sixel_scroll_cost() : 0U);
        if (!try_charge_sixel_work(budget, cost)) {
            return false;
        }
        if (scrolls_first) {
            scroll_active_region_up();
            ++placement.scrolls;
            row = m_scroll_bottom;
        }
        placement.refused_composite_bytes = std::max(
            placement.refused_composite_bytes,
            place_image_band(
                placement.raster,
                band * cell.height,
                static_cast<int>(row),
                placement.origin.column,
                cell));
        ++placement.next_band;
    }

    if (!placement.bands_done) {
        placement.bands_done = true;

        // A row whose images together would exceed the decoded-size cap keeps
        // the newer one; the largest refused composite is reported once.
        if (placement.refused_composite_bytes > 0U) {
            generated_actions.push_back(make_payload_limit_diagnostic(
                QStringLiteral("DCS sixel"),
                placement.refused_composite_bytes,
                sixel_raster_limit_bytes(),
                Parser_sequence_family::DCS));
        }

        // The image's geometry may ask for far more scrolls than its pixels
        // fill. Once the region has scrolled its full height it is blank, so
        // further scrolls are skipped (owner decision D5): the screen and the
        // cursor end as if they ran, and history gains at most one region of
        // blank rows.
        placement.trailing_scrolls_left = placement.display_mode
            ? 0
            : std::min<std::int64_t>(
                placement.scroll_count - placement.scrolls,
                m_scroll_bottom - m_scroll_top + 1);
    }

    while (placement.trailing_scrolls_left > 0) {
        if (!try_charge_sixel_work(budget, sixel_scroll_cost())) {
            return false;
        }
        scroll_active_region_up();
        --placement.trailing_scrolls_left;
    }

    // DECSDM set leaves the cursor where it was.
    if (!placement.display_mode) {
        const std::int64_t cursor_row = std::clamp<std::int64_t>(
            placement.origin.row - placement.scroll_count +
                placement.final_cursor_y / cell.height,
            0,
            m_config.grid_size.rows - 1);
        if (placement.scroll_count > 0 || cursor_row != placement.origin.row) {
            mark_cursor_dirty();
            m_cursor.row   = static_cast<int>(cursor_row);
            m_pending_wrap = false;
            mark_cursor_dirty();
        }
    }

    m_sixel_placement.reset();
    return true;
}

std::size_t Terminal_screen_model::place_image_band(
    const QImage&              raster,
    int                        band_top,
    int                        row,
    int                        first_column,
    terminal_cell_pixel_size_t cell)
{
    // Clip at the right margin, and copy the band out of the decoder's raster,
    // which may be a view keeping a larger capacity buffer alive.
    const int band_width = static_cast<int>(std::min<std::int64_t>(
        raster.width(),
        static_cast<std::int64_t>(m_config.grid_size.columns - first_column) * cell.width));
    const int band_height = std::min(cell.height, raster.height() - band_top);

    // The band is laid out as the slice it may become, so its columns come
    // from the one slice rule; make_image_slice gives it a revision.
    Terminal_image_slice band{
        QImage(band_width, band_height, QImage::Format_RGBA8888_Premultiplied),
        first_column,
        cell,
        0U,
    };
    // QImage reports a failed allocation with a null image, not an exception.
    if (band.pixels.isNull()) {
        throw std::bad_alloc();
    }
    const int band_columns = terminal_image_slice_column_span(band);

    // A cell is covered when its block receives at least one drawn pixel;
    // undrawn pixels are all zero.
    std::vector<unsigned char> covered_columns(static_cast<std::size_t>(band_columns), 0U);
    bool any_covered = false;
    for (int y = 0; y < band_height; ++y) {
        const auto* source =
            reinterpret_cast<const std::uint32_t*>(raster.constScanLine(band_top + y));
        std::memcpy(
            band.pixels.scanLine(y),
            source,
            static_cast<std::size_t>(band_width) * sizeof(std::uint32_t));
        for (int column = 0; column < band_columns; ++column) {
            if (covered_columns[static_cast<std::size_t>(column)] != 0U) {
                continue;
            }
            const int x_end = std::min(band_width, (column + 1) * cell.width);
            for (int x = column * cell.width; x < x_end; ++x) {
                if (source[x] != 0U) {
                    covered_columns[static_cast<std::size_t>(column)] = 1U;
                    any_covered = true;
                    break;
                }
            }
        }
    }

    // A band with nothing drawn leaves the row as it was.
    if (!any_covered) {
        return 0U;
    }

    // Everything that can fail is prepared first: the row's new image, the
    // copy of its cells a text change needs, and the room to mark the row
    // dirty. Only then do the row's text, wraps and image change, together.
    Terminal_screen_row& screen_row = active_grid_rows()[static_cast<std::size_t>(row)];

    // S4 draws the band over an earlier image on the row, unless their
    // composite would exceed the decoded-size cap (I5): the newer band then
    // replaces the earlier image, and the refused size is reported.
    std::size_t refused_composite_bytes = 0U;
    int         slice_first_column      = first_column;
    if (screen_row.image_slice != nullptr) {
        const composite_layout_t layout = composite_image_layout(
            *screen_row.image_slice,
            band.pixels,
            first_column,
            cell);
        if (layout.bytes > static_cast<std::int64_t>(sixel_raster_limit_bytes())) {
            refused_composite_bytes = static_cast<std::size_t>(layout.bytes);
        }
        else {
            band.pixels        = composite_image_band(*screen_row.image_slice, band.pixels, layout);
            slice_first_column = layout.first_column;
        }
    }
    std::shared_ptr<const Terminal_image_slice> image =
        make_image_slice(std::move(band.pixels), slice_first_column, cell);

    // Text under an image would draw over it, so a covered cell loses its
    // text and hyperlink and keeps its style, the way an erase leaves a cell;
    // a wide glyph with a covered cell is cleared whole.
    const auto clear_covered_text = [&](bool apply)
    {
        bool changed = false;
        for (int index = 0; index < band_columns; ++index) {
            if (covered_columns[static_cast<std::size_t>(index)] == 0U) {
                continue;
            }

            const int base_column = cell_base_column_in_row(screen_row, first_column + index);
            const Cell& base_cell = screen_row.cells[static_cast<std::size_t>(base_column)];
            const int span_end = std::min(
                m_config.grid_size.columns,
                base_column + std::max(1, base_cell.display_width));
            Cell cleared;
            if (base_cell.style_id != k_default_terminal_style_id) {
                cleared.occupied = true;
                cleared.style_id = base_cell.style_id;
            }

            for (int column = base_column; column < span_end; ++column) {
                Cell& cell_to_clear = screen_row.cells[static_cast<std::size_t>(column)];
                const bool already_cleared =
                    cell_to_clear.occupied          == cleared.occupied          &&
                    cell_to_clear.style_id          == cleared.style_id          &&
                    cell_to_clear.hyperlink_id      == cleared.hyperlink_id      &&
                    cell_to_clear.wide_continuation == cleared.wide_continuation &&
                    cell_to_clear.display_width     == cleared.display_width     &&
                    cell_to_clear.text              == cleared.text;
                if (!already_cleared) {
                    changed = true;
                    if (apply) {
                        cell_to_clear = cleared;
                    }
                }
            }
        }
        return changed;
    };
    std::vector<Cell> before_cells;
    if (clear_covered_text(false)) {
        before_cells = screen_row.cells;
    }
    reserve_dirty_row_marks(1U);

    // A row that shows an image starts its own logical line (owner decision
    // D2), so placing one hard-terminates the soft wraps into and out of its
    // row, and reflow never has to split one image over several rows.
    break_soft_wrap_before(row);
    screen_row.soft_wrap_columns = 0;
    if (!before_cells.empty()) {
        clear_covered_text(true);
    }
    screen_row.image_slice = std::move(image);
    mark_terminal_content_changed();
    mark_dirty(row);
    if (!before_cells.empty()) {
        advance_row_content_generation_if_changed(screen_row, before_cells);
    }
    return refused_composite_bytes;
}

// As a history record over the record limit keeps its text and drops its
// image (A7), every row on the screens, and every row a repaint-recovery
// candidate copied from the primary screen, drops an image over the cap and
// keeps its text and metadata, so no row image the model holds exceeds the
// cap (I5). Applying allocates nothing once dirty marks for the active
// screen's rows are reserved, and only a drop on the active screen changes
// what the terminal shows.
std::size_t Terminal_screen_model::drop_row_images_over_cap(std::size_t cap_bytes, bool apply)
{
    const std::int64_t cap = static_cast<std::int64_t>(cap_bytes);
    std::size_t largest_bytes = 0U;
    bool        shown_row_changed = false;
    const auto visit = [&](std::vector<Terminal_screen_row>& rows, bool active) {
        for (std::size_t index = 0; index < rows.size(); ++index) {
            Terminal_screen_row& row = rows[index];
            if (row.image_slice == nullptr) {
                continue;
            }
            const std::int64_t bytes =
                std::int64_t{row.image_slice->pixels.width()} * row.image_slice->pixels.height() * 4;
            if (bytes <= cap) {
                continue;
            }
            largest_bytes = std::max(largest_bytes, static_cast<std::size_t>(bytes));
            if (apply) {
                row.image_slice.reset();
                if (active) {
                    mark_dirty(static_cast<int>(index));
                    shown_row_changed = true;
                }
            }
        }
    };
    visit(
        m_primary_backing.active_grid_state().rows,
        m_active_buffer_id == Terminal_buffer_id::PRIMARY);
    visit(
        m_alternate_grid.active_grid_state().rows,
        m_active_buffer_id == Terminal_buffer_id::ALTERNATE);
    visit(m_primary_repaint_recovery_candidate.rows, false);
    if (shown_row_changed) {
        mark_terminal_content_changed();
    }
    return largest_bytes;
}

std::shared_ptr<const Terminal_image_slice> Terminal_screen_model::make_image_slice(
    QImage                     pixels,
    int                        first_column,
    terminal_cell_pixel_size_t cell_pixel_size)
{
    // Every new slice takes the next revision, which identifies its content. A
    // renderer keys cached copies on it and outlives a session and its model,
    // so revisions are unique across all models in the process.
    static std::atomic<std::uint64_t> next_revision{1U};
    return std::make_shared<const Terminal_image_slice>(Terminal_image_slice{
        std::move(pixels),
        first_column,
        cell_pixel_size,
        next_revision.fetch_add(1U, std::memory_order_relaxed),
    });
}

// Text written or erased in a cell leaves no image under it (S1): the row's
// image without the pixels of cells [first_column, end_column), measured in
// the cells the image was placed on, or null when no drawn pixel is left. The
// row itself is left alone, so a caller computes this before changing cells
// and a failed allocation leaves the row whole.
std::shared_ptr<const Terminal_image_slice> Terminal_screen_model::image_slice_without_cells(
    const Terminal_screen_row& row,
    int                        first_column,
    int                        end_column)
{
    if (row.image_slice == nullptr) {
        return nullptr;
    }

    const int                        slice_first_column = row.image_slice->first_column;
    const terminal_cell_pixel_size_t slice_cell         = row.image_slice->cell_pixel_size;
    const QImage&                    slice_pixels       = row.image_slice->pixels;
    const int x_first = std::max(0, (first_column - slice_first_column) * slice_cell.width);
    const int x_end   = std::min(
        slice_pixels.width(),
        (end_column - slice_first_column) * slice_cell.width);
    if (x_first >= x_end || !image_block_has_drawn_pixel(slice_pixels, x_first, x_end)) {
        return row.image_slice;
    }

    QImage pixels = slice_pixels.copy();
    // QImage reports a failed allocation with a null image, not an exception.
    if (pixels.isNull()) {
        throw std::bad_alloc();
    }
    for (int y = 0; y < pixels.height(); ++y) {
        auto* line = reinterpret_cast<std::uint32_t*>(pixels.scanLine(y));
        std::fill(line + x_first, line + x_end, 0U);
    }

    return image_block_has_drawn_pixel(pixels, 0, pixels.width())
        ? make_image_slice(std::move(pixels), slice_first_column, slice_cell)
        : nullptr;
}

// ICH and DCH move a row's cells from from_column on by `shift` columns, to
// the right when positive, and the image moves with them (S1). Image columns
// the shift pushes past the right margin, or that DCH deletes, are lost; so
// are moving columns already past the margin, as no cell carries them in.
// Like image_slice_without_cells, this computes the row's new image, null when
// none is left, and leaves the row alone.
std::shared_ptr<const Terminal_image_slice> Terminal_screen_model::image_slice_shifted(
    const Terminal_screen_row& row,
    int                        from_column,
    int                        shift)
{
    if (row.image_slice == nullptr) {
        return nullptr;
    }

    const Terminal_image_slice& slice = *row.image_slice;
    const int slice_end_column = slice.first_column + terminal_image_slice_column_span(slice);
    if (slice_end_column <= from_column) {
        return row.image_slice;
    }

    struct column_segment_t
    {
        int first  = 0;
        int end    = 0;
        int offset = 0;
    };

    // Columns left of from_column stay; columns from it on move, less those
    // DCH deletes and those that would land at or past the right margin.
    const int columns = m_config.grid_size.columns;
    const column_segment_t segments[] = {
        {slice.first_column, std::min(slice_end_column, from_column), 0},
        {
            std::max(slice.first_column, from_column + std::max(0, -shift)),
            std::min({slice_end_column, columns, columns - shift}),
            shift,
        },
    };

    const int cell_width   = slice.cell_pixel_size.width;
    const int pixel_width  = slice.pixels.width();
    int       first_column = std::numeric_limits<int>::max();
    int       end_x        = 0;
    for (const column_segment_t& segment : segments) {
        if (segment.first < segment.end) {
            first_column = std::min(first_column, segment.first + segment.offset);
        }
    }
    if (first_column == std::numeric_limits<int>::max()) {
        return nullptr;
    }
    for (const column_segment_t& segment : segments) {
        if (segment.first < segment.end) {
            const int source_end_x =
                std::min(pixel_width, (segment.end - slice.first_column) * cell_width);
            const int source_first_x = (segment.first - slice.first_column) * cell_width;
            const int target_first_x = (segment.first + segment.offset - first_column) * cell_width;
            end_x = std::max(end_x, target_first_x + source_end_x - source_first_x);
        }
    }

    QImage pixels(end_x, slice.pixels.height(), QImage::Format_RGBA8888_Premultiplied);
    // QImage reports a failed allocation with a null image, not an exception.
    if (pixels.isNull()) {
        throw std::bad_alloc();
    }
    pixels.fill(0U);
    for (const column_segment_t& segment : segments) {
        if (segment.first >= segment.end) {
            continue;
        }

        const int source_first_x = (segment.first - slice.first_column) * cell_width;
        const int source_end_x   = std::min(pixel_width, (segment.end - slice.first_column) * cell_width);
        const int target_first_x = (segment.first + segment.offset - first_column) * cell_width;
        for (int y = 0; y < pixels.height(); ++y) {
            std::memcpy(
                pixels.scanLine(y) + static_cast<std::size_t>(target_first_x) * sizeof(std::uint32_t),
                slice.pixels.constScanLine(y) + static_cast<std::size_t>(source_first_x) * sizeof(std::uint32_t),
                static_cast<std::size_t>(source_end_x - source_first_x) * sizeof(std::uint32_t));
        }
    }

    return image_block_has_drawn_pixel(pixels, 0, pixels.width())
        ? make_image_slice(std::move(pixels), first_column, slice.cell_pixel_size)
        : nullptr;
}

void Terminal_screen_model::backspace()
{
    mark_cursor_dirty();
    if (m_cursor.column > 0) {
        --m_cursor.column;
    }
    m_pending_wrap = false;
    mark_cursor_dirty();
}

void Terminal_screen_model::horizontal_tab()
{
    int target = m_config.grid_size.columns - 1;
    for (int column = m_cursor.column + 1; column < m_config.grid_size.columns; ++column) {
        if (m_tab_stops[static_cast<std::size_t>(column)]) {
            target = column;
            break;
        }
    }

    mark_cursor_dirty();
    m_cursor.column = target;
    m_pending_wrap = false;
    mark_cursor_dirty();
}

void Terminal_screen_model::mark_cursor_dirty()
{
    mark_dirty(m_cursor.row);
}

void Terminal_screen_model::mark_dirty(int row)
{
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled && m_text_span_profile_depth > 0) {
        ++m_profile_stats.dirty_marks_from_text_writes;
    }
    if (m_dirty_row_stats.enabled) {
        ++m_dirty_row_stats.mark_requests;
        ++dirty_row_stats_bucket().mark_requests;
    }
#endif

    if (row < 0 || row >= m_config.grid_size.rows) {
#if VNM_TERMINAL_PROFILING_ENABLED
        if (m_dirty_row_stats.enabled) {
            ++m_dirty_row_stats.out_of_bounds_mark_requests;
            ++dirty_row_stats_bucket().out_of_bounds_mark_requests;
        }
#endif
        return;
    }

    const std::size_t row_index = static_cast<std::size_t>(row);
    size_dirty_row_flags();

    if (row == m_last_dirty_row || m_dirty_row_flags[row_index] != 0U) {
#if VNM_TERMINAL_PROFILING_ENABLED
        if (m_dirty_row_stats.enabled) {
            ++m_dirty_row_stats.duplicate_mark_requests;
            ++dirty_row_stats_bucket().duplicate_mark_requests;
        }
#endif
        return;
    }

    m_dirty_rows.push_back(row);
    m_dirty_row_flags[row_index] = 1U;
    m_last_dirty_row = row;
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_dirty_row_stats.enabled) {
        Terminal_screen_model_dirty_row_bucket_stats& bucket = dirty_row_stats_bucket();
        ++m_dirty_row_stats.unique_pending_row_marks;
        ++bucket.unique_pending_row_marks;
        update_pending_dirty_row_stats_watermark();
    }
#endif
}

void Terminal_screen_model::mark_dirty_rows(int first, int last)
{
    for (int row = first; row <= last; ++row) {
        mark_dirty(row);
    }
}

void Terminal_screen_model::size_dirty_row_flags()
{
    if (m_dirty_row_flags.size() !=
        static_cast<std::size_t>(m_config.grid_size.rows))
    {
        std::vector<unsigned char> resized_flags(
            static_cast<std::size_t>(m_config.grid_size.rows),
            0U);
        for (const int dirty_row : m_dirty_rows) {
            const std::size_t dirty_row_index = static_cast<std::size_t>(dirty_row);
            if (dirty_row_index < resized_flags.size()) {
                resized_flags[dirty_row_index] = 1U;
            }
        }
        m_dirty_row_flags = std::move(resized_flags);
    }
}

// Allocates what the next `marks` calls of mark_dirty on grid rows can need,
// so a change that must not fail halfway can reserve it first and mark its
// rows afterwards.
void Terminal_screen_model::reserve_dirty_row_marks(std::size_t marks)
{
    size_dirty_row_flags();
    // Each mark appends at most one row. Growing at least as push_back would
    // keeps repeated reserves amortized.
    const std::size_t needed = m_dirty_rows.size() + marks;
    if (needed > m_dirty_rows.capacity()) {
        m_dirty_rows.reserve(std::max<std::size_t>(needed, 2U * m_dirty_rows.size()));
    }
}

void Terminal_screen_model::mark_terminal_content_changed()
{
    if (m_primary_repaint_recovery_candidate.active &&
        m_active_buffer_id == Terminal_buffer_id::PRIMARY)
    {
        m_primary_repaint_recovery_candidate.visible_row_identity_ambiguous = true;
    }
    else {
        m_primary_repaint_recovery_episode_active = false;
    }

    m_terminal_content_changed = true;
}

void Terminal_screen_model::mark_active_buffer_changed()
{
    m_active_buffer_changed = true;
}

void Terminal_screen_model::mark_grid_reflow_changed()
{
    m_grid_reflow_changed = true;
}

void Terminal_screen_model::mark_viewport_changed()
{
    m_viewport_changed = true;
}

void Terminal_screen_model::mark_mode_state_changed()
{
    m_mode_state_changed = true;
}

void Terminal_screen_model::mark_mouse_reporting_mode_changed()
{
    m_mouse_reporting_mode_changed = true;
    mark_mode_state_changed();
}

void Terminal_screen_model::mark_alternate_scroll_mode_changed()
{
    m_alternate_scroll_mode_changed = true;
    mark_mode_state_changed();
}

void Terminal_screen_model::mark_all_dirty()
{
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_dirty_row_stats.enabled) {
        ++m_dirty_row_stats.mark_all_dirty_calls;
        ++dirty_row_stats_bucket().mark_all_dirty_calls;
    }
#endif

    for (int row = 0; row < m_config.grid_size.rows; ++row) {
        mark_dirty(row);
    }
}

void Terminal_screen_model::repair_wide_spans_in_row(
    std::vector<Cell>& row,
    int                column_count) const
{
    int active_span_end = -1;

    for (int column = 0; column < column_count; ++column) {
        Cell& cell = row[static_cast<std::size_t>(column)];
        if (cell.wide_continuation) {
            if (column >= active_span_end) {
                cell = Cell{};
            }
            continue;
        }

        if (column < active_span_end) {
            cell = Cell{};
            continue;
        }

        if (!cell.occupied) {
            continue;
        }

        if (cell.display_width <= 1) {
            active_span_end = column + 1;
            continue;
        }

        if (cell.display_width > column_count - column) {
            const int clear_end = std::min(column_count, column + cell.display_width);
            for (int clear_column = column; clear_column < clear_end; ++clear_column) {
                row[static_cast<std::size_t>(clear_column)] = Cell{};
            }
            active_span_end = column + 1;
            continue;
        }

        active_span_end = column + cell.display_width;
    }
}

void Terminal_screen_model::clear_dirty()
{
    // Parser actions usually touch zero, one, or two rows. Clear only those
    // flags, rather than scanning the full grid before every action. Bounds
    // checks also cover pending marks left from a larger pre-resize grid.
    for (const int row : m_dirty_rows) {
        const std::size_t index = static_cast<std::size_t>(row);
        if (index < m_dirty_row_flags.size()) {
            m_dirty_row_flags[index] = 0U;
        }
    }
    m_dirty_rows.clear();
    m_last_dirty_row                = -1;
    m_viewport_changed              = false;
    m_terminal_content_changed      = false;
    m_active_buffer_changed         = false;
    m_grid_reflow_changed           = false;
    m_mode_state_changed            = false;
    m_mouse_reporting_mode_changed  = false;
    m_alternate_scroll_mode_changed = false;
}

void Terminal_screen_model::clear_backing_deltas()
{
    m_backing_deltas.clear();
}

void Terminal_screen_model::clear_recovery_proposals()
{
    m_recovery_proposals.clear();
}

void Terminal_screen_model::clear_selection_continuity()
{
    if (m_selection_continuity_published) {
        m_selection_continuity.reset();
        m_selection_continuity_published = false;
    }
}

terminal_selection_continuity_capability_t&
Terminal_screen_model::selection_continuity()
{
    if (m_selection_continuity_published) {
        m_selection_continuity.reset();
        m_selection_continuity_published = false;
    }
    if (!m_selection_continuity.has_value()) {
        m_selection_continuity.emplace();
    }
    return *m_selection_continuity;
}

void Terminal_screen_model::record_selection_successor(
    terminal_selection_line_successor_t successor)
{
    terminal_selection_continuity_capability_t& continuity = selection_continuity();
    continuity.successors_by_old_retained_line_id[successor.old_handle.row_sequence]
        .push_back(successor);
}

void Terminal_screen_model::finalize_selection_continuity_rows()
{
    if (!m_selection_continuity.has_value()) {
        return;
    }

    for (auto& [old_retained_line_id, successors] :
        m_selection_continuity->successors_by_old_retained_line_id)
    {
        Q_UNUSED(old_retained_line_id);
        for (terminal_selection_line_successor_t& successor : successors) {
            const Terminal_retained_line_lookup_result lookup = retained_line_lookup(
                Terminal_buffer_id::PRIMARY,
                successor.final_handle);
            if (!lookup.exact_match || lookup.retained_line_id_match_count != 1) {
                continue;
            }
            successor.final_logical_row = lookup.exact_logical_row;
            successor.row_delta = successor.final_logical_row - successor.old_logical_row;
        }
    }

    for (terminal_selection_survivor_proof_t& proof :
        m_selection_continuity->survivor_proofs)
    {
        if (!terminal_history_handle_has_identity(proof.final_handle)) {
            continue;
        }
        const Terminal_retained_line_lookup_result lookup = retained_line_lookup(
            Terminal_buffer_id::PRIMARY,
            proof.final_handle);
        if (lookup.exact_match && lookup.retained_line_id_match_count == 1) {
            proof.final_logical_row = lookup.exact_logical_row;
        }
    }
}

void Terminal_screen_model::adopt_publication_changes(ingest_publication_t&& publication)
{
    m_dirty_rows.clear();
    if (m_dirty_row_flags.size() !=
        static_cast<std::size_t>(m_config.grid_size.rows))
    {
        m_dirty_row_flags.assign(
            static_cast<std::size_t>(m_config.grid_size.rows),
            0U);
    }
    else {
        std::fill(m_dirty_row_flags.begin(), m_dirty_row_flags.end(), 0U);
    }
    for (const int row : publication.dirty_rows) {
        const std::size_t row_index = static_cast<std::size_t>(row);
        if (row_index >= m_dirty_row_flags.size() ||
            m_dirty_row_flags[row_index] != 0U)
        {
            continue;
        }
        m_dirty_row_flags[row_index] = 1U;
        m_dirty_rows.push_back(row);
    }
    m_last_dirty_row = m_dirty_rows.empty() ? -1 : m_dirty_rows.back();
    m_terminal_content_changed      = publication.terminal_content_changed;
    m_active_buffer_changed         = publication.active_buffer_changed;
    m_grid_reflow_changed           = publication.grid_reflow_changed;
    m_viewport_changed              = publication.viewport_changed;
    m_mode_state_changed            = publication.mode_state_changed;
    m_mouse_reporting_mode_changed  = publication.mouse_reporting_mode_changed;
    m_alternate_scroll_mode_changed = publication.alternate_scroll_mode_changed;
}

void Terminal_screen_model::accumulate_pending_changes(
    ingest_publication_t& publication) const
{
    publication.dirty_rows.insert(m_dirty_rows.begin(), m_dirty_rows.end());
    publication.viewport_changed =
        publication.viewport_changed || m_viewport_changed;
    publication.terminal_content_changed =
        publication.terminal_content_changed || m_terminal_content_changed;
    publication.active_buffer_changed =
        publication.active_buffer_changed || m_active_buffer_changed;
    publication.grid_reflow_changed =
        publication.grid_reflow_changed || m_grid_reflow_changed;
    publication.mode_state_changed =
        publication.mode_state_changed || m_mode_state_changed;
    publication.mouse_reporting_mode_changed =
        publication.mouse_reporting_mode_changed ||
        m_mouse_reporting_mode_changed;
    publication.alternate_scroll_mode_changed =
        publication.alternate_scroll_mode_changed ||
        m_alternate_scroll_mode_changed;
}

void Terminal_screen_model::accumulate_publication_changes(
    ingest_publication_t&       target,
    const ingest_publication_t& source) const
{
    target.dirty_rows.insert(source.dirty_rows.begin(), source.dirty_rows.end());
    target.dirty_rows_have_stable_mutation_identity =
        target.dirty_rows_have_stable_mutation_identity &&
        source.dirty_rows_have_stable_mutation_identity;
    target.viewport_changed = target.viewport_changed || source.viewport_changed;
    target.terminal_content_changed =
        target.terminal_content_changed || source.terminal_content_changed;
    target.active_buffer_changed =
        target.active_buffer_changed || source.active_buffer_changed;
    target.grid_reflow_changed =
        target.grid_reflow_changed || source.grid_reflow_changed;
    target.mode_state_changed =
        target.mode_state_changed || source.mode_state_changed;
    target.mouse_reporting_mode_changed =
        target.mouse_reporting_mode_changed || source.mouse_reporting_mode_changed;
    target.alternate_scroll_mode_changed =
        target.alternate_scroll_mode_changed || source.alternate_scroll_mode_changed;
}

void Terminal_screen_model::assign_trailing_changes(
    const ingest_publication_t&                    publication,
    terminal_screen_model_trailing_changes_t* trailing_changes) const
{
    if (trailing_changes == nullptr) {
        return;
    }

    trailing_changes->terminal_content_changed = publication.terminal_content_changed;
    trailing_changes->active_buffer_changed = publication.active_buffer_changed;
    trailing_changes->grid_reflow_changed = publication.grid_reflow_changed;
    trailing_changes->viewport_changed = publication.viewport_changed;
    trailing_changes->mode_state_changed = publication.mode_state_changed;
    trailing_changes->mouse_reporting_mode_changed =
        publication.mouse_reporting_mode_changed;
    trailing_changes->alternate_scroll_mode_changed =
        publication.alternate_scroll_mode_changed;
}

void Terminal_screen_model::emit_pending_resize_transition()
{
    Q_ASSERT(m_resize_transition_sink != nullptr);
    Q_ASSERT(m_resize_transition_publication != nullptr);
    Q_ASSERT(m_pending_resize_transition.has_value());

    accumulate_publication_changes(
        *m_resize_transition_publication,
        m_synchronized_selection_changes);
    m_synchronized_selection_changes = {};
    accumulate_pending_changes(*m_resize_transition_publication);
    terminal_screen_model_resize_transition_t transition;
    transition.result = finalize_resize_transition_result(
        *m_resize_transition_publication);
    transition.grid_size_before = m_pending_resize_transition->first;
    transition.grid_size_after  = m_pending_resize_transition->second;
    transition.sequence         = m_resize_transition_sink->sequence;
    transition.ordinal          = ++m_resize_transition_ordinal;
    m_pending_resize_transition.reset();
    m_skip_next_resize_transition_accumulation = true;

    m_resize_transition_sink->consume(
        m_resize_transition_sink->context,
        transition);

    clear_backing_deltas();
    clear_recovery_proposals();
    clear_selection_continuity();
    *m_resize_transition_publication = {};
}

void Terminal_screen_model::emit_text_area_resize_request(
    terminal_grid_size_t grid_size,
    bool                 model_grid_changed)
{
    if (m_resize_transition_sink == nullptr ||
        m_resize_transition_sink->consume_text_area_resize_request == nullptr)
    {
        return;
    }

    terminal_screen_model_text_area_resize_request_t request;
    request.grid_size          = grid_size;
    request.sequence           = m_resize_transition_sink->sequence;
    request.ordinal            = ++m_text_area_resize_request_ordinal;
    request.model_grid_changed = model_grid_changed;
    m_resize_transition_sink->consume_text_area_resize_request(
        m_resize_transition_sink->context,
        request);
}

Terminal_screen_model_result Terminal_screen_model::finalize_resize_transition_result(
    const ingest_publication_t& publication)
{
    refresh_active_grid_retained_lookup_indexes();
    finalize_selection_continuity_rows();

    Terminal_screen_model_result result;
    result.dirty_rows.assign(publication.dirty_rows.begin(), publication.dirty_rows.end());
    result.dirty_rows_have_stable_mutation_identity =
        publication.dirty_rows_have_stable_mutation_identity;
    result.terminal_content_changed     = publication.terminal_content_changed;
    result.active_buffer_changed        = publication.active_buffer_changed;
    result.grid_reflow_changed          = publication.grid_reflow_changed;
    result.viewport_changed              = publication.viewport_changed;
    result.mode_state_changed            = publication.mode_state_changed;
    result.mouse_reporting_mode_changed  = publication.mouse_reporting_mode_changed;
    result.alternate_scroll_mode_changed = publication.alternate_scroll_mode_changed;
    result.scrollback_rows               = scrollback_size();
    result.backing_deltas                = m_backing_deltas;
    result.recovery_proposals            = m_recovery_proposals;
    result.recovery_attempts             = std::move(m_recovery_attempts);
    result.selection_continuity          = m_selection_continuity;
    m_selection_continuity_published     = true;
    result.dropped_recovery_attempts     = m_dropped_recovery_attempts;
    m_recovery_attempts.clear();
    m_dropped_recovery_attempts = 0U;
    result.evicted_scrollback_rows = compatibility_evicted_scrollback_rows();
    return result;
}

Terminal_screen_model_result Terminal_screen_model::finalize_result(
    Terminal_screen_model_result result,
    result_change_overrides_t overrides)
{
    refresh_active_grid_retained_lookup_indexes();
    finalize_selection_continuity_rows();
    result.dirty_rows = dirty_rows();
    result.dirty_rows_have_stable_mutation_identity =
        overrides.dirty_rows_have_stable_mutation_identity.value_or(true);
    result.terminal_content_changed =
        overrides.terminal_content_changed.value_or(m_terminal_content_changed);
    result.active_buffer_changed =
        overrides.active_buffer_changed.value_or(m_active_buffer_changed);
    result.grid_reflow_changed =
        overrides.grid_reflow_changed.value_or(m_grid_reflow_changed);
    result.viewport_changed =
        overrides.viewport_changed.value_or(m_viewport_changed);
    result.mode_state_changed =
        overrides.mode_state_changed.value_or(m_mode_state_changed);
    result.mouse_reporting_mode_changed =
        overrides.mouse_reporting_mode_changed.value_or(m_mouse_reporting_mode_changed);
    result.alternate_scroll_mode_changed =
        overrides.alternate_scroll_mode_changed.value_or(
            m_alternate_scroll_mode_changed);
    result.scrollback_rows               = scrollback_size();
    result.backing_deltas                = m_backing_deltas;
    result.recovery_proposals            = m_recovery_proposals;
    result.recovery_attempts             = std::move(m_recovery_attempts);
    result.selection_continuity          = m_selection_continuity;
    m_selection_continuity_published     = true;
    result.dropped_recovery_attempts     = m_dropped_recovery_attempts;
    m_recovery_attempts.clear();
    m_dropped_recovery_attempts = 0U;
    result.evicted_scrollback_rows       = compatibility_evicted_scrollback_rows();
    return result;
}

int Terminal_screen_model::compatibility_evicted_scrollback_rows() const
{
    int rows = 0;
    for (const terminal_backing_delta_t& delta : m_backing_deltas) {
        rows += delta.evicted_scrollback_rows;
        rows += delta.discarded_scrollback_rows;
    }
    return rows;
}

void Terminal_screen_model::record_backing_delta(terminal_backing_delta_t delta)
{
    m_backing_deltas.push_back(delta);
}

void Terminal_screen_model::record_active_grid_delta(
    Terminal_backing_delta_kind    kind,
    terminal_grid_size_t           grid_size_before,
    terminal_grid_size_t           grid_size_after)
{
    terminal_backing_delta_t delta;
    delta.kind                 = kind;
    delta.buffer_id            = m_active_buffer_id;
    delta.active_buffer_before = m_active_buffer_id;
    delta.active_buffer_after  = m_active_buffer_id;
    delta.grid_size_before     = grid_size_before;
    delta.grid_size_after      = grid_size_after;
    record_backing_delta(delta);
}

void Terminal_screen_model::record_mode_transition_delta(
    Terminal_buffer_id active_buffer_before,
    Terminal_buffer_id active_buffer_after)
{
    terminal_backing_delta_t delta;
    delta.kind                 = Terminal_backing_delta_kind::MODE_TRANSITIONED;
    delta.buffer_id            = active_buffer_after;
    delta.active_buffer_before = active_buffer_before;
    delta.active_buffer_after  = active_buffer_after;
    delta.grid_size_before     = m_config.grid_size;
    delta.grid_size_after      = m_config.grid_size;
    record_backing_delta(delta);
}

void Terminal_screen_model::record_primary_history_delta(
    Terminal_backing_delta_kind    kind,
    int                            scrollback_rows_before,
    int                            scrollback_rows_after,
    int                            appended_scrollback_rows,
    int                            evicted_scrollback_rows,
    int                            discarded_scrollback_rows)
{
    terminal_backing_delta_t delta;
    delta.kind                       = kind;
    delta.buffer_id                  = Terminal_buffer_id::PRIMARY;
    delta.active_buffer_before       = m_active_buffer_id;
    delta.active_buffer_after        = m_active_buffer_id;
    delta.grid_size_before           = m_config.grid_size;
    delta.grid_size_after            = m_config.grid_size;
    delta.scrollback_rows_before     = scrollback_rows_before;
    delta.scrollback_rows_after      = scrollback_rows_after;
    delta.appended_scrollback_rows   = appended_scrollback_rows;
    delta.evicted_scrollback_rows    = evicted_scrollback_rows;
    delta.discarded_scrollback_rows  = discarded_scrollback_rows;
    record_backing_delta(delta);
}

void Terminal_screen_model::evict_oldest_scrollback_rows(int row_count)
{
    if (row_count <= 0) {
        return;
    }

    const int scrollback_rows_before = scrollback_size();
    const int rows_to_evict = std::min(row_count, scrollback_rows_before);
    if (rows_to_evict <= 0) {
        return;
    }

    const std::vector<terminal_history_handle_t> evicted_handles =
        m_primary_backing.discard_oldest_retained_history_records(rows_to_evict);
    if (evicted_handles.empty()) {
        return;
    }

    for (const terminal_history_handle_t handle : evicted_handles) {
        erase_retained_lookup_entry(Terminal_buffer_id::PRIMARY, handle.row_sequence);
    }
    const int evicted_rows = static_cast<int>(evicted_handles.size());
    m_scrollback_evicted_rows += evicted_rows;
    record_primary_history_delta(
        Terminal_backing_delta_kind::PRIMARY_HISTORY_EVICTED,
        scrollback_rows_before,
        scrollback_size(),
        0,
        evicted_rows,
        0);
}

#if VNM_TERMINAL_PROFILING_ENABLED
Terminal_screen_model_dirty_row_bucket_stats&
Terminal_screen_model::dirty_row_stats_bucket() const
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_dirty_row_stats_start_time);
    const std::uint64_t bucket_index =
        m_dirty_row_timeline.bucket_width_ms > 0U
            ? static_cast<std::uint64_t>(elapsed.count()) /
                m_dirty_row_timeline.bucket_width_ms
            : 0U;
    const std::size_t bucket_count = static_cast<std::size_t>(bucket_index + 1U);
    if (m_dirty_row_timeline.buckets.size() < bucket_count) {
        const std::size_t previous_size = m_dirty_row_timeline.buckets.size();
        m_dirty_row_timeline.buckets.resize(bucket_count);
        for (std::size_t index = previous_size;
            index < m_dirty_row_timeline.buckets.size();
            ++index)
        {
            Terminal_screen_model_dirty_row_bucket_stats& bucket =
                m_dirty_row_timeline.buckets[index];
            bucket.start_ms =
                static_cast<std::uint64_t>(index) * m_dirty_row_timeline.bucket_width_ms;
            bucket.end_ms = bucket.start_ms + m_dirty_row_timeline.bucket_width_ms;
        }
    }

    return m_dirty_row_timeline.buckets[static_cast<std::size_t>(bucket_index)];
}

void Terminal_screen_model::update_pending_dirty_row_stats_watermark()
{
    const std::uint64_t pending_dirty_rows =
        static_cast<std::uint64_t>(m_dirty_rows.size());
    m_dirty_row_stats.max_pending_dirty_rows = std::max<std::uint64_t>(
        m_dirty_row_stats.max_pending_dirty_rows,
        pending_dirty_rows);
    dirty_row_stats_bucket().max_pending_dirty_rows = std::max<std::uint64_t>(
        dirty_row_stats_bucket().max_pending_dirty_rows,
        pending_dirty_rows);
}

void Terminal_screen_model::update_synchronized_dirty_row_stats_watermark()
{
    const std::uint64_t synchronized_dirty_rows =
        static_cast<std::uint64_t>(m_synchronized_dirty_rows.size());
    m_dirty_row_stats.max_synchronized_dirty_rows = std::max<std::uint64_t>(
        m_dirty_row_stats.max_synchronized_dirty_rows,
        synchronized_dirty_rows);
    dirty_row_stats_bucket().max_synchronized_dirty_rows = std::max<std::uint64_t>(
        dirty_row_stats_bucket().max_synchronized_dirty_rows,
        synchronized_dirty_rows);
}
#endif

std::vector<int> Terminal_screen_model::dirty_rows() const
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_screen_model::dirty_rows");

#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_dirty_row_stats.enabled) {
        ++m_dirty_row_stats.dirty_rows_snapshot_calls;
        m_dirty_row_stats.dirty_rows_snapshot_rows +=
            static_cast<std::uint64_t>(m_dirty_rows.size());
        Terminal_screen_model_dirty_row_bucket_stats& bucket = dirty_row_stats_bucket();
        ++bucket.dirty_rows_snapshot_calls;
        bucket.dirty_rows_snapshot_rows +=
            static_cast<std::uint64_t>(m_dirty_rows.size());
    }
#endif

    std::vector<int> rows(m_dirty_rows.begin(), m_dirty_rows.end());
    std::sort(rows.begin(), rows.end());
    return rows;
}

void Terminal_screen_model::collect_synchronized_changes()
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_screen_model::collect_synchronized_changes");

    const bool skip_resize_transition_accumulation =
        m_skip_next_resize_transition_accumulation;
    m_skip_next_resize_transition_accumulation = false;
    if (!skip_resize_transition_accumulation) {
        const bool synchronized_dirty_rows_present = !m_dirty_rows.empty();
        accumulate_pending_changes(m_synchronized_selection_changes);
        if (synchronized_dirty_rows_present) {
            m_synchronized_selection_changes
                .dirty_rows_have_stable_mutation_identity = false;
        }
        if (m_resize_transition_publication != nullptr) {
            accumulate_pending_changes(*m_resize_transition_publication);
            if (synchronized_dirty_rows_present) {
                m_resize_transition_publication
                    ->dirty_rows_have_stable_mutation_identity = false;
            }
        }
    }

#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_dirty_row_stats.enabled) {
        ++m_dirty_row_stats.collect_synchronized_calls;
        m_dirty_row_stats.collect_synchronized_rows +=
            static_cast<std::uint64_t>(m_dirty_rows.size());
        Terminal_screen_model_dirty_row_bucket_stats& bucket = dirty_row_stats_bucket();
        ++bucket.collect_synchronized_calls;
        bucket.collect_synchronized_rows +=
            static_cast<std::uint64_t>(m_dirty_rows.size());
    }
#endif

    m_synchronized_dirty_rows.insert(m_dirty_rows.begin(), m_dirty_rows.end());
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_dirty_row_stats.enabled) {
        update_synchronized_dirty_row_stats_watermark();
    }
#endif
    m_synchronized_viewport_changed =
        m_synchronized_viewport_changed || m_viewport_changed;
    m_synchronized_terminal_content_changed =
        m_synchronized_terminal_content_changed || m_terminal_content_changed;
    m_synchronized_active_buffer_changed =
        m_synchronized_active_buffer_changed || m_active_buffer_changed;
    m_synchronized_grid_reflow_changed =
        m_synchronized_grid_reflow_changed || m_grid_reflow_changed;
    m_synchronized_mode_state_changed =
        m_synchronized_mode_state_changed || m_mode_state_changed;
    m_synchronized_mouse_reporting_mode_changed =
        m_synchronized_mouse_reporting_mode_changed ||
        m_mouse_reporting_mode_changed;
    m_synchronized_alternate_scroll_mode_changed =
        m_synchronized_alternate_scroll_mode_changed ||
        m_alternate_scroll_mode_changed;
    clear_dirty();
}

void Terminal_screen_model::publish_pending_changes(ingest_publication_t& publication)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_screen_model::publish_pending_changes");

    if (m_resize_transition_publication != nullptr) {
        if (m_skip_next_resize_transition_accumulation) {
            m_skip_next_resize_transition_accumulation = false;
        }
        else {
            accumulate_pending_changes(*m_resize_transition_publication);
        }
    }

#if VNM_TERMINAL_PROFILING_ENABLED
    const std::size_t previous_dirty_row_count = publication.dirty_rows.size();
#endif
    publication.dirty_rows.insert(m_dirty_rows.begin(), m_dirty_rows.end());
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_dirty_row_stats.enabled) {
        ++m_dirty_row_stats.publish_pending_calls;
        const std::uint64_t published_unique_rows = static_cast<std::uint64_t>(
            publication.dirty_rows.size() -
            previous_dirty_row_count);
        m_dirty_row_stats.published_unique_rows += published_unique_rows;
        Terminal_screen_model_dirty_row_bucket_stats& bucket = dirty_row_stats_bucket();
        ++bucket.publish_pending_calls;
        bucket.published_unique_rows += published_unique_rows;
    }
#endif
    publication.viewport_changed =
        publication.viewport_changed || m_viewport_changed;
    publication.terminal_content_changed =
        publication.terminal_content_changed || m_terminal_content_changed;
    publication.active_buffer_changed =
        publication.active_buffer_changed || m_active_buffer_changed;
    publication.grid_reflow_changed =
        publication.grid_reflow_changed || m_grid_reflow_changed;
    publication.mode_state_changed =
        publication.mode_state_changed || m_mode_state_changed;
    publication.mouse_reporting_mode_changed =
        publication.mouse_reporting_mode_changed ||
        m_mouse_reporting_mode_changed;
    publication.alternate_scroll_mode_changed =
        publication.alternate_scroll_mode_changed ||
        m_alternate_scroll_mode_changed;
    clear_dirty();
}

void Terminal_screen_model::release_synchronized_changes(ingest_publication_t& publication)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_screen_model::release_synchronized_changes");

#if VNM_TERMINAL_PROFILING_ENABLED
    const std::size_t previous_dirty_row_count = publication.dirty_rows.size();
#endif
    publication.dirty_rows.insert(
        m_synchronized_dirty_rows.begin(),
        m_synchronized_dirty_rows.end());
    if (!m_synchronized_dirty_rows.empty()) {
        publication.dirty_rows_have_stable_mutation_identity = false;
    }
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_dirty_row_stats.enabled) {
        ++m_dirty_row_stats.release_synchronized_calls;
        const std::uint64_t released_synchronized_rows = static_cast<std::uint64_t>(
            publication.dirty_rows.size() -
            previous_dirty_row_count);
        m_dirty_row_stats.released_synchronized_rows += released_synchronized_rows;
        Terminal_screen_model_dirty_row_bucket_stats& bucket = dirty_row_stats_bucket();
        ++bucket.release_synchronized_calls;
        bucket.released_synchronized_rows += released_synchronized_rows;
    }
#endif
    publication.viewport_changed =
        publication.viewport_changed || m_synchronized_viewport_changed;
    publication.terminal_content_changed =
        publication.terminal_content_changed || m_synchronized_terminal_content_changed;
    publication.active_buffer_changed =
        publication.active_buffer_changed || m_synchronized_active_buffer_changed;
    publication.grid_reflow_changed =
        publication.grid_reflow_changed || m_synchronized_grid_reflow_changed;
    publication.mode_state_changed =
        publication.mode_state_changed || m_synchronized_mode_state_changed;
    publication.mouse_reporting_mode_changed =
        publication.mouse_reporting_mode_changed ||
        m_synchronized_mouse_reporting_mode_changed;
    publication.alternate_scroll_mode_changed =
        publication.alternate_scroll_mode_changed ||
        m_synchronized_alternate_scroll_mode_changed;
    if (m_resize_transition_publication != nullptr) {
        accumulate_publication_changes(
            *m_resize_transition_publication,
            m_synchronized_selection_changes);
    }
    m_synchronized_selection_changes = {};
    m_synchronized_dirty_rows.clear();
    m_synchronized_viewport_changed              = false;
    m_synchronized_terminal_content_changed      = false;
    m_synchronized_active_buffer_changed         = false;
    m_synchronized_grid_reflow_changed           = false;
    m_synchronized_mode_state_changed            = false;
    m_synchronized_mouse_reporting_mode_changed  = false;
    m_synchronized_alternate_scroll_mode_changed = false;
}

Terminal_hyperlink_id Terminal_screen_model::next_hyperlink_id()
{
    if (m_next_hyperlink_id == k_no_terminal_hyperlink_id) {
        compact_hyperlink_ids();
    }

    if (m_next_hyperlink_id == k_no_terminal_hyperlink_id) {
        throw std::overflow_error("terminal hyperlink id space exhausted");
    }

    const Terminal_hyperlink_id id = m_next_hyperlink_id;
    m_next_hyperlink_id = id == k_max_terminal_hyperlink_id
        ? k_no_terminal_hyperlink_id
        : static_cast<Terminal_hyperlink_id>(id + 1U);
    return id;
}

Terminal_hyperlink_id Terminal_screen_model::active_hyperlink_id_for_identity(
    const QByteArray& identity_key)
{
    const auto found = m_active_hyperlink_ids.find(identity_key);
    if (found != m_active_hyperlink_ids.end()) {
        return found->second;
    }

    // Interning is driven by terminal output, and an OSC 8 start does not have
    // to print anything: a producer can replace the current link indefinitely
    // and leave every superseded identity behind, because the only routine
    // pruning happens on a grid resize or a scrollback-limit change. Prune on
    // the way in too, the way the style table compacts before it grows, so what
    // the registry holds tracks the links the screen still shows rather than
    // every link that has ever been announced. Rows that scrolled away carry
    // their own identity copies, so an entry dropped here costs the reuse of
    // its id and not the link.
    if (m_active_hyperlink_ids.size() >= m_next_hyperlink_prune_count) {
        retain_referenced_active_hyperlink_ids();
    }

    const Terminal_hyperlink_id hyperlink_id = next_hyperlink_id();
    m_active_hyperlink_ids.emplace(identity_key, hyperlink_id);
    return hyperlink_id;
}

terminal_hyperlink_identity_by_id_t
Terminal_screen_model::active_hyperlink_identity_keys_by_id() const
{
    terminal_hyperlink_identity_by_id_t identity_keys_by_id;
    for (const auto& entry : m_active_hyperlink_ids) {
        identity_keys_by_id.emplace(entry.second, entry.first);
    }
    return identity_keys_by_id;
}

void Terminal_screen_model::retain_referenced_active_hyperlink_ids()
{
    if (m_current_hyperlink_id == k_no_terminal_hyperlink_id &&
        m_active_hyperlink_ids.empty())
    {
        return;
    }

    const std::size_t identities_before_pruning = m_active_hyperlink_ids.size();
    std::set<Terminal_hyperlink_id> live_ids;

    const auto collect_cells = [&](const std::vector<Terminal_screen_row>& rows) {
        for (const Terminal_screen_row& row : rows) {
            for (const Cell& cell : row.cells) {
                if (cell.hyperlink_id != k_no_terminal_hyperlink_id) {
                    live_ids.insert(cell.hyperlink_id);
                }
            }
        }
    };

    if (m_current_hyperlink_id != k_no_terminal_hyperlink_id) {
        live_ids.insert(m_current_hyperlink_id);
    }
    collect_cells(active_grid_rows());
    if (m_active_buffer_id == Terminal_buffer_id::PRIMARY) {
        collect_cells(m_alternate_grid.active_grid_state().rows);
    }
    else {
        collect_cells(m_primary_backing.active_grid_state().rows);
    }
    for (auto it = m_active_hyperlink_ids.begin(); it != m_active_hyperlink_ids.end();) {
        if (live_ids.find(it->second) == live_ids.end()) {
            it = m_active_hyperlink_ids.erase(it);
        }
        else {
            ++it;
        }
    }

    // The next prune is measured from what survived this one, so a screen that
    // legitimately holds many links does not re-scan on every new identity, and
    // a screen that holds none goes back to the starting allowance.
    m_next_hyperlink_prune_count =
        m_active_hyperlink_ids.size() + k_terminal_hyperlink_prune_threshold;
    m_hyperlink_table_stats.reclaimed_hyperlink_ids += static_cast<std::uint64_t>(
        identities_before_pruning - m_active_hyperlink_ids.size());
}

void Terminal_screen_model::fail_hyperlink_compaction_allocation_if_requested(
    Terminal_hyperlink_compaction_allocation_phase phase)
{
    if (m_fail_next_hyperlink_compaction_allocation_phase_for_testing == phase) {
        m_fail_next_hyperlink_compaction_allocation_phase_for_testing.reset();
        throw std::bad_alloc();
    }
}

void Terminal_screen_model::compact_hyperlink_ids()
{
    std::set<Terminal_hyperlink_id> known_ids_before_compaction;
    std::map<Terminal_hyperlink_id, QByteArray> identity_by_old_id;

    const auto record_identity = [&](Terminal_hyperlink_id id, const QByteArray& identity) {
        known_ids_before_compaction.insert(id);
        const auto [found, inserted] = identity_by_old_id.emplace(id, identity);
        if (!inserted && found->second != identity) {
            throw std::runtime_error(
                "terminal hyperlink id compaction found conflicting identity metadata");
        }
    };

    for (const auto& entry : m_active_hyperlink_ids) {
        record_identity(entry.second, entry.first);
    }
    if (m_primary_repaint_recovery_candidate.active) {
        for (const auto& entry : m_primary_repaint_recovery_candidate.hyperlink_identity_keys) {
            record_identity(entry.first, entry.second);
        }
    }

    std::set<Terminal_hyperlink_id> live_active_ids;
    const auto collect_live_active_id = [&](Terminal_hyperlink_id id) {
        if (id != k_no_terminal_hyperlink_id) {
            live_active_ids.insert(id);
        }
    };

    const auto collect_active_cells = [&](const std::vector<Terminal_screen_row>& rows) {
        for (const Terminal_screen_row& row : rows) {
            for (const Cell& cell : row.cells) {
                collect_live_active_id(cell.hyperlink_id);
            }
        }
    };

    collect_live_active_id(m_current_hyperlink_id);
    collect_active_cells(m_primary_backing.active_grid_state().rows);
    collect_active_cells(m_alternate_grid.active_grid_state().rows);

    std::set<Terminal_hyperlink_id> referenced_ids = live_active_ids;
    const auto collect_referenced_id = [&](Terminal_hyperlink_id id) {
        if (id != k_no_terminal_hyperlink_id) {
            referenced_ids.insert(id);
        }
    };
    const auto collect_candidate_rows = [&](const std::vector<Terminal_screen_row>& rows) {
        for (const Terminal_screen_row& row : rows) {
            for (const Cell& cell : row.cells) {
                collect_referenced_id(cell.hyperlink_id);
            }
        }
    };

    if (m_primary_repaint_recovery_candidate.active) {
        collect_candidate_rows(m_primary_repaint_recovery_candidate.rows);
        for (const auto& entry : m_primary_repaint_recovery_candidate.hyperlink_identity_keys) {
            collect_referenced_id(entry.first);
        }
    }

    std::map<QByteArray, Terminal_hyperlink_id> new_ids_by_identity;
    fail_hyperlink_compaction_allocation_if_requested(
        Terminal_hyperlink_compaction_allocation_phase::IDENTITY_TO_NEW_ID);
    for (Terminal_hyperlink_id old_id : referenced_ids) {
        const auto identity = identity_by_old_id.find(old_id);
        if (identity == identity_by_old_id.end() || identity->second.isEmpty()) {
            throw std::runtime_error("terminal hyperlink id compaction missing identity metadata");
        }

        if (new_ids_by_identity.find(identity->second) == new_ids_by_identity.end()) {
            if (new_ids_by_identity.size() >=
                static_cast<std::size_t>(k_max_terminal_hyperlink_id))
            {
                throw std::overflow_error("terminal hyperlink id space exhausted");
            }

            const Terminal_hyperlink_id new_id =
                static_cast<Terminal_hyperlink_id>(new_ids_by_identity.size() + 1U);
            new_ids_by_identity.emplace(identity->second, new_id);
        }
    }

    std::map<Terminal_hyperlink_id, Terminal_hyperlink_id> remap_by_old_id;
    fail_hyperlink_compaction_allocation_if_requested(
        Terminal_hyperlink_compaction_allocation_phase::OLD_TO_NEW_ID);
    for (Terminal_hyperlink_id old_id : referenced_ids) {
        remap_by_old_id.emplace(old_id, new_ids_by_identity.at(identity_by_old_id.at(old_id)));
    }

    const auto rewritten_id = [&](Terminal_hyperlink_id old_id) -> Terminal_hyperlink_id {
        if (old_id == k_no_terminal_hyperlink_id) {
            return k_no_terminal_hyperlink_id;
        }

        const auto found = remap_by_old_id.find(old_id);
        if (found == remap_by_old_id.end()) {
            throw std::runtime_error("terminal hyperlink id compaction missing live remap");
        }
        return found->second;
    };

    std::map<QByteArray, Terminal_hyperlink_id> active_hyperlink_ids;
    fail_hyperlink_compaction_allocation_if_requested(
        Terminal_hyperlink_compaction_allocation_phase::ACTIVE_IDENTITY_REPLACEMENT);
    for (const auto& entry : m_active_hyperlink_ids) {
        if (live_active_ids.find(entry.second) != live_active_ids.end()) {
            active_hyperlink_ids.emplace(entry.first, rewritten_id(entry.second));
        }
    }

    std::map<Terminal_hyperlink_id, QByteArray> candidate_identity_keys;
    if (m_primary_repaint_recovery_candidate.active) {
        fail_hyperlink_compaction_allocation_if_requested(
            Terminal_hyperlink_compaction_allocation_phase::
                RECOVERY_CANDIDATE_IDENTITY_REPLACEMENT);
        for (const auto& entry : m_primary_repaint_recovery_candidate.hyperlink_identity_keys) {
            const Terminal_hyperlink_id new_id = rewritten_id(entry.first);
            candidate_identity_keys.emplace(new_id, entry.second);
        }
    }

    const auto validate_row_ids = [&](const std::vector<Terminal_screen_row>& rows) {
        for (const Terminal_screen_row& row : rows) {
            for (const Cell& cell : row.cells) {
                (void)rewritten_id(cell.hyperlink_id);
            }
        }
    };
    (void)rewritten_id(m_current_hyperlink_id);
    validate_row_ids(m_primary_backing.active_grid_state().rows);
    validate_row_ids(m_alternate_grid.active_grid_state().rows);
    if (m_primary_repaint_recovery_candidate.active) {
        validate_row_ids(m_primary_repaint_recovery_candidate.rows);
    }

    Q_ASSERT(known_ids_before_compaction.size() >= new_ids_by_identity.size());
    const std::uint64_t reclaimed_hyperlink_ids =
        known_ids_before_compaction.size() - new_ids_by_identity.size();

    const Terminal_hyperlink_id next_hyperlink_id =
        new_ids_by_identity.size() >= static_cast<std::size_t>(k_max_terminal_hyperlink_id)
            ? k_no_terminal_hyperlink_id
            : static_cast<Terminal_hyperlink_id>(new_ids_by_identity.size() + 1U);

    const auto rewrite_id = [&](Terminal_hyperlink_id& id) {
        if (id == k_no_terminal_hyperlink_id) {
            return;
        }

        const auto found = remap_by_old_id.find(id);
        Q_ASSERT(found != remap_by_old_id.end());
        id = found->second;
    };
    const auto rewrite_row_ids = [&](std::vector<Terminal_screen_row>& rows) {
        for (Terminal_screen_row& row : rows) {
            for (Cell& cell : row.cells) {
                rewrite_id(cell.hyperlink_id);
            }
        }
    };

    rewrite_id(m_current_hyperlink_id);
    rewrite_row_ids(m_primary_backing.active_grid_state().rows);
    rewrite_row_ids(m_alternate_grid.active_grid_state().rows);
    if (m_primary_repaint_recovery_candidate.active) {
        rewrite_row_ids(m_primary_repaint_recovery_candidate.rows);
    }
    m_active_hyperlink_ids.swap(active_hyperlink_ids);
    if (m_primary_repaint_recovery_candidate.active) {
        m_primary_repaint_recovery_candidate.hyperlink_identity_keys.swap(
            candidate_identity_keys);
    }

    ++m_hyperlink_table_stats.compaction_count;
    m_hyperlink_table_stats.reclaimed_hyperlink_ids += reclaimed_hyperlink_ids;
    m_next_hyperlink_id = next_hyperlink_id;
    m_next_hyperlink_prune_count =
        m_active_hyperlink_ids.size() + k_terminal_hyperlink_prune_threshold;
}

Terminal_screen_model::retained_row_record_t Terminal_screen_model::seal_retained_row_record(
    const Terminal_screen_row&               screen_row,
    Terminal_retained_line_provenance_source source,
    const std::map<Terminal_hyperlink_id, QByteArray>* hyperlink_identity_keys,
    const terminal_hyperlink_identity_by_id_t* active_hyperlink_identity_keys_by_id)
{
    retained_row_record_t record;
    record.row                   = screen_row;
    record.metadata.source_width = m_config.grid_size.columns;
    record.metadata.wrap_state = screen_row.soft_wrap_columns > 0
        ? Terminal_retained_row_wrap_state::SOFT_WRAP
        : Terminal_retained_row_wrap_state::HARD_BOUNDARY;
    if (source != Terminal_retained_line_provenance_source::TERMINAL_STORAGE) {
        rebase_retained_line_id_preserving_content(record.row, source);
    }
    materialize_retained_row_styles(record);
    materialize_retained_row_hyperlinks(
        record,
        hyperlink_identity_keys,
        active_hyperlink_identity_keys_by_id);
    return record;
}

Terminal_history_row_record Terminal_screen_model::history_row_record_from_retained_record(
    const retained_row_record_t& retained_record)
{
    Terminal_history_row_record history_record;
    history_record.provenance = retained_record.row.retained_line_provenance;
    history_record.content_origin_spans = retained_record.row.content_origin_spans;
    history_record.style_table = retained_record.style_table;
    history_record.hyperlink_identity_keys = retained_record.hyperlink_identity_keys;
    history_record.metadata = retained_record.metadata;
    history_record.image_slice = retained_record.row.image_slice;
    history_record.cells.reserve(retained_record.row.cells.size());

    for (const Cell& cell : retained_record.row.cells) {
        history_record.cells.push_back({
            cell.text,
            cell.display_width,
            cell.wide_continuation,
            cell.occupied,
            cell.style_id,
            cell.hyperlink_id,
        });
    }

    return history_record;
}

Terminal_screen_model::retained_row_record_t
Terminal_screen_model::retained_row_record_from_history_row_record(
    const Terminal_history_row_record& history_record)
{
    retained_row_record_t retained_record;
    retained_record.row.retained_line_provenance = history_record.provenance;
    retained_record.row.content_origin_spans = history_record.content_origin_spans;
    retained_record.style_table = history_record.style_table;
    retained_record.hyperlink_identity_keys = history_record.hyperlink_identity_keys;
    retained_record.metadata = history_record.metadata;
    retained_record.row.image_slice = history_record.image_slice;
    retained_record.row.cells.reserve(history_record.cells.size());

    for (std::size_t index = 0; index < history_record.cells.size(); ++index) {
        const Terminal_history_row_cell& cell = history_record.cells[index];
        Cell restored_cell;
        restored_cell.text = cell.text;
        restored_cell.text_category = Terminal_render_cell_text::category_for_text(
            QStringView(cell.text));
        restored_cell.display_width = cell.display_width;
        restored_cell.wide_continuation = cell.wide_continuation;
        restored_cell.occupied = cell.occupied;
        restored_cell.style_id = cell.style_id;
        restored_cell.hyperlink_id = cell.hyperlink_id;
        restored_cell.natural_display_width = cell.wide_continuation
            ? 0
            : std::max(1, cell.display_width);
        if (restored_cell.text_category ==
                Terminal_render_cell_text_category::NON_ASCII &&
            cell.display_width == 1 &&
            cell.occupied && !cell.wide_continuation)
        {
            // Editing can move a clipped-wide cell away from the right margin.
            // Its original width is recoverable from the retained Unicode;
            // display_width alone would erase it during materialization.
            const QByteArray text_bytes = cell.text.toUtf8();
            const Terminal_utf8_width_result width = measure_utf8_width(text_bytes);
            if (width.status == Terminal_unicode_width_status::OK &&
                width.cells > restored_cell.display_width)
            {
                restored_cell.natural_display_width =
                    std::max(restored_cell.natural_display_width, width.cells);
            }
        }
        retained_record.row.cells.push_back(std::move(restored_cell));
    }

    return retained_record;
}

void Terminal_screen_model::materialize_retained_row_styles(
    retained_row_record_t& row) const
{
    row.style_table.clear();

    std::map<terminal_text_style_lookup_key_t, Terminal_style_id> row_refs_by_style;
    for (Cell& cell : row.row.cells) {
        if (cell.style_id == k_default_terminal_style_id) {
            continue;
        }

        const std::size_t source_style_index =
            static_cast<std::size_t>(cell.style_id);
        if (source_style_index >= m_styles.size()) {
            throw_retained_history_storage_failure();
        }

        const Terminal_text_style& style = m_styles[source_style_index];
        if (style == make_default_terminal_text_style()) {
            cell.style_id = k_default_terminal_style_id;
            continue;
        }

        const terminal_text_style_lookup_key_t key = terminal_text_style_lookup_key(style);
        const auto found = row_refs_by_style.find(key);
        if (found != row_refs_by_style.end()) {
            cell.style_id = found->second;
            continue;
        }

        const Terminal_style_id row_ref =
            static_cast<Terminal_style_id>(row.style_table.size() + 1U);
        row_refs_by_style.emplace(key, row_ref);
        row.style_table.push_back(style);
        cell.style_id = row_ref;
    }
}

void Terminal_screen_model::materialize_retained_row_hyperlinks(
    retained_row_record_t& row,
    const std::map<Terminal_hyperlink_id, QByteArray>* preserved_identity_keys,
    const terminal_hyperlink_identity_by_id_t* active_identity_keys_by_id) const
{
    std::map<Terminal_hyperlink_id, QByteArray> old_identity_keys =
        std::move(row.hyperlink_identity_keys);
    row.hyperlink_identity_keys.clear();

    std::map<QByteArray, Terminal_hyperlink_id> row_refs_by_identity_key;
    const terminal_hyperlink_identity_by_id_t* active_identity_keys =
        active_identity_keys_by_id;
    std::optional<terminal_hyperlink_identity_by_id_t> lazy_active_identity_keys_by_id;
    for (Cell& cell : row.row.cells) {
        if (cell.hyperlink_id == k_no_terminal_hyperlink_id) {
            continue;
        }

        const Terminal_hyperlink_id source_hyperlink_id = cell.hyperlink_id;
        const QByteArray* identity_key = nullptr;

        auto old_found = old_identity_keys.find(cell.hyperlink_id);
        if (old_found != old_identity_keys.end()) {
            identity_key = &old_found->second;
        }
        else if (preserved_identity_keys != nullptr) {
            auto preserved_found =
                preserved_identity_keys->find(cell.hyperlink_id);
            if (preserved_found != preserved_identity_keys->end()) {
                identity_key = &preserved_found->second;
            }
        }

        if (identity_key == nullptr) {
            if (active_identity_keys == nullptr) {
                lazy_active_identity_keys_by_id = active_hyperlink_identity_keys_by_id();
                active_identity_keys = &*lazy_active_identity_keys_by_id;
            }

            const auto active_found =
                active_identity_keys->find(source_hyperlink_id);
            if (active_found != active_identity_keys->end()) {
                identity_key = &active_found->second;
            }
        }

        if (identity_key == nullptr || identity_key->isEmpty()) {
            throw_retained_history_storage_failure();
        }

        const auto found = row_refs_by_identity_key.find(*identity_key);
        if (found != row_refs_by_identity_key.end()) {
            cell.hyperlink_id = found->second;
            continue;
        }

        const Terminal_hyperlink_id row_ref =
            static_cast<Terminal_hyperlink_id>(row_refs_by_identity_key.size()) + 1U;
        row_refs_by_identity_key.emplace(*identity_key, row_ref);
        row.hyperlink_identity_keys.emplace(row_ref, *identity_key);
        cell.hyperlink_id = row_ref;
    }
}

int Terminal_screen_model::active_grid_row_count() const
{
    return m_config.grid_size.rows;
}

int Terminal_screen_model::primary_backing_row_count() const
{
    return scrollback_size() + active_grid_row_count();
}

int Terminal_screen_model::primary_backing_active_grid_first_row() const
{
    return scrollback_size();
}

bool Terminal_screen_model::active_grid_row_is_valid(active_grid_row_t row) const
{
    return row.value >= 0 && row.value < active_grid_row_count();
}

bool Terminal_screen_model::primary_backing_row_is_valid(primary_backing_row_t row) const
{
    return row.value >= 0 && row.value < primary_backing_row_count();
}

bool Terminal_screen_model::viewport_row_is_valid(viewport_row_t row) const
{
    return row.value >= 0 && row.value < m_config.grid_size.rows;
}

const std::vector<Terminal_screen_model::Terminal_screen_row>&
Terminal_screen_model::primary_active_grid_rows() const
{
    return m_active_buffer_id == Terminal_buffer_id::PRIMARY
        ? active_grid_rows()
        : m_primary_backing.active_grid_state().rows;
}

const std::vector<Terminal_screen_model::Terminal_screen_row>&
Terminal_screen_model::alternate_active_grid_rows() const
{
    return m_active_buffer_id == Terminal_buffer_id::ALTERNATE
        ? active_grid_rows()
        : m_alternate_grid.active_grid_state().rows;
}

Terminal_screen_model::primary_backing_row_t
Terminal_screen_model::primary_backing_row_from_active(active_grid_row_t row) const
{
    return {primary_backing_active_grid_first_row() + row.value};
}

std::optional<Terminal_screen_model::active_grid_row_t>
Terminal_screen_model::active_grid_row_from_primary_backing(primary_backing_row_t row) const
{
    if (!primary_backing_row_is_valid(row)) {
        return std::nullopt;
    }

    active_grid_row_t converted{
        row.value - primary_backing_active_grid_first_row(),
    };
    if (!active_grid_row_is_valid(converted)) {
        return std::nullopt;
    }

    return converted;
}

std::optional<Terminal_screen_model::primary_backing_row_t>
Terminal_screen_model::primary_backing_row_from_viewport(
    const Terminal_viewport_state& viewport,
    viewport_row_t                 row) const
{
    if (viewport.active_buffer != Terminal_buffer_id::PRIMARY || !viewport_row_is_valid(row)) {
        return std::nullopt;
    }

    primary_backing_row_t converted{
        viewport.scrollback_rows - viewport.offset_from_tail + row.value,
    };
    if (!primary_backing_row_is_valid(converted)) {
        return std::nullopt;
    }

    return converted;
}

std::optional<Terminal_screen_model::viewport_row_t>
Terminal_screen_model::viewport_row_from_primary_backing(
    const Terminal_viewport_state& viewport,
    primary_backing_row_t          row) const
{
    if (viewport.active_buffer != Terminal_buffer_id::PRIMARY ||
        !primary_backing_row_is_valid(row))
    {
        return std::nullopt;
    }

    viewport_row_t converted =
        viewport_row_from_primary_backing_unbounded(viewport, row);
    if (!viewport_row_is_valid(converted)) {
        return std::nullopt;
    }

    return converted;
}

Terminal_screen_model::viewport_row_t
Terminal_screen_model::viewport_row_from_primary_backing_unbounded(
    const Terminal_viewport_state& viewport,
    primary_backing_row_t          row) const
{
    const int first_visible_row =
        viewport.scrollback_rows - viewport.offset_from_tail;
    return {row.value - first_visible_row};
}

std::optional<Terminal_screen_model::Terminal_screen_row>
Terminal_screen_model::primary_backing_row(primary_backing_row_t row) const
{
    if (!primary_backing_row_is_valid(row)) {
        return std::nullopt;
    }

    if (row.value < scrollback_size()) {
        std::optional<retained_row_record_t> retained_record;
        {
            VNM_TERMINAL_PROFILE_SCOPE(
                "Terminal_screen_model::primary_backing_row::retained_history_materialize");
            retained_record = m_primary_backing.materialize_retained_history_record(
                static_cast<std::size_t>(row.value),
                Terminal_history_row_record_image_decode::SKIP_PIXELS);
        }
        return retained_record.has_value()
            ? std::optional<Terminal_screen_row>(std::move(retained_record->row))
            : std::nullopt;
    }

    const std::optional<active_grid_row_t> active_row =
        active_grid_row_from_primary_backing(row);
    if (!active_row.has_value()) {
        return std::nullopt;
    }

    return primary_active_grid_rows()[static_cast<std::size_t>(active_row->value)];
}

const Terminal_screen_model::Terminal_screen_row*
Terminal_screen_model::alternate_active_row(active_grid_row_t row) const
{
    if (!active_grid_row_is_valid(row)) {
        return nullptr;
    }

    return &alternate_active_grid_rows()[static_cast<std::size_t>(row.value)];
}

std::vector<Terminal_screen_model::Cell>
Terminal_screen_model::visual_row_projection_for_current_geometry(
    const std::vector<Cell>& row) const
{
    const int column_count = m_config.grid_size.columns;
    std::vector<Cell> projection;
    projection.reserve(static_cast<std::size_t>(column_count));

    const int copied_column_count =
        std::min(column_count, static_cast<int>(row.size()));
    projection.insert(
        projection.end(),
        row.begin(),
        row.begin() + copied_column_count);
    projection.resize(static_cast<std::size_t>(column_count));
    repair_wide_spans_in_row(projection, column_count);
    return projection;
}

const std::vector<Terminal_screen_model::Cell>&
Terminal_screen_model::row_cells_for_current_geometry(
    const std::vector<Cell>&   row,
    std::vector<Cell>&         visual_projection) const
{
    if (static_cast<int>(row.size()) == m_config.grid_size.columns) {
        return row;
    }

    visual_projection = visual_row_projection_for_current_geometry(row);
    return visual_projection;
}

QString Terminal_screen_model::row_text_from_cells(
    const std::vector<Cell>&   row,
    int                        first_column,
    int                        end_column) const
{
    std::vector<Cell> visual_projection;
    const std::vector<Cell>& visual_row =
        row_cells_for_current_geometry(row, visual_projection);
    const int column_count = m_config.grid_size.columns;
    const int bounded_first_column = std::clamp(first_column, 0, column_count);
    const int bounded_end_column   = std::clamp(end_column,   0, column_count);

    QString text;
    for (int column = bounded_first_column; column < bounded_end_column; ++column) {
        const Cell& cell = visual_row[static_cast<std::size_t>(column)];
        if (cell.wide_continuation) {
            continue;
        }

        text += cell.occupied ? cell.text : QStringLiteral(" ");
    }

    if (bounded_end_column == column_count) {
        while (!text.isEmpty() && text.back() == QChar(u' ')) {
            text.chop(1);
        }
    }

    return text;
}

terminal_retained_history_ordinal_range_t
Terminal_screen_model::retained_history_ordinal_range() const
{
    const Retained_history_storage& storage = m_primary_backing.retained_history;
    return {
        storage.index.empty()
            ? storage.next_ordinal
            : storage.index.front().ordinal,
        storage.next_ordinal,
    };
}

bool Terminal_screen_model::search_row_text(
    Terminal_buffer_id        buffer_id,
    int                       logical_row,
    Terminal_search_row_text& out_text) const
{
    return search_row_text_impl(buffer_id, logical_row, true, out_text);
}

bool Terminal_screen_model::search_source_row_text(
    Terminal_buffer_id        buffer_id,
    int                       logical_row,
    Terminal_search_row_text& out_text) const
{
    return search_row_text_impl(buffer_id, logical_row, false, out_text);
}

bool Terminal_screen_model::search_row_text_impl(
    Terminal_buffer_id        buffer_id,
    int                       logical_row,
    bool                      current_geometry,
    Terminal_search_row_text& out_text) const
{
    out_text.clear();
    if (logical_row < 0) {
        return false;
    }

    // The active-grid row is borrowed rather than taken through
    // primary_backing_row, which returns a whole row by value.
    const std::vector<Cell>*           source_cells = nullptr;
    std::optional<Terminal_screen_row> materialized;
    if (buffer_id == Terminal_buffer_id::ALTERNATE) {
        const Terminal_screen_row* row =
            alternate_active_row(active_grid_row_t{logical_row});
        if (row == nullptr) {
            return false;
        }
        source_cells = &row->cells;
    }
    else
    if (logical_row < scrollback_size()) {
        std::optional<retained_row_record_t> record =
            m_primary_backing.materialize_retained_history_record(
                static_cast<std::size_t>(logical_row),
                Terminal_history_row_record_image_decode::SKIP_PIXELS);
        if (!record.has_value()) {
            return false;
        }
        materialized = std::move(record->row);
        source_cells = &materialized->cells;
    }
    else {
        const std::optional<active_grid_row_t> active_row =
            active_grid_row_from_primary_backing(primary_backing_row_t{logical_row});
        if (!active_row.has_value()) {
            return false;
        }
        source_cells = &primary_active_grid_rows()[
            static_cast<std::size_t>(active_row->value)].cells;
    }

    out_text.source_width = static_cast<std::int32_t>(source_cells->size());
    out_text.units.reserve(source_cells->size());
    std::vector<Cell> visual_projection;
    const std::vector<Cell>& visual_row = current_geometry
        ? row_cells_for_current_geometry(*source_cells, visual_projection)
        : *source_cells;
    const int column_count = static_cast<int>(visual_row.size());
    int       next_column  = 0;
    for (int column = 0; column < column_count; ++column) {
        const Cell& cell = visual_row[static_cast<std::size_t>(column)];
        if (!cell.occupied || cell.wide_continuation || cell.text.isEmpty()) {
            continue;
        }

        while (next_column < column) {
            out_text.append_padding_column(next_column);
            ++next_column;
        }

        const int end_column = column + std::max(1, cell.display_width);
        out_text.append_cell_text(cell.text, column, end_column);
        next_column = std::max(next_column, end_column);
    }

    return true;
}

std::optional<std::vector<Terminal_screen_model::Cell>>
Terminal_screen_model::logical_row_cells(
    Terminal_buffer_id         buffer_id,
    int                        logical_row) const
{
    if (logical_row < 0) {
        return std::nullopt;
    }

    if (buffer_id == Terminal_buffer_id::ALTERNATE) {
        const Terminal_screen_row* row = alternate_active_row(active_grid_row_t{logical_row});
        return row != nullptr
            ? std::optional<std::vector<Cell>>(row->cells)
            : std::nullopt;
    }

    const std::optional<Terminal_screen_row> row =
        primary_backing_row(primary_backing_row_t{logical_row});
    return row.has_value()
        ? std::optional<std::vector<Cell>>(row->cells)
        : std::nullopt;
}

}
