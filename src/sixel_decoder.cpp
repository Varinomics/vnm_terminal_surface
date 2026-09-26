#include "vnm_terminal/internal/sixel_decoder.h"

#include "vnm_terminal/internal/terminal_history_ring.h"

#include <QString>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace vnm_terminal::internal {

namespace {

constexpr QImage::Format k_sixel_raster_format = QImage::Format_RGBA8888_Premultiplied;
constexpr int            k_sixel_row_pixels      = 6;
constexpr unsigned char  k_sixel_data_first      = 0x3fU;
constexpr unsigned char  k_sixel_data_last       = 0x7eU;

// Numeric parameters saturate here. The ceiling only has to keep the
// arithmetic exact: an extent this large is far past the decoded-size cap,
// and no register number, aspect ratio or color coordinate needs more.
constexpr int k_sixel_parameter_limit = 32767;

// The image cursor and extent saturate at the largest value the image
// carries. Repeats and graphics new lines move the cursor without bound and
// the cap only ever holds the extent, so this keeps both extents below 2^31,
// their product below 2^62 and the decoded size below 2^64.
constexpr std::int64_t k_sixel_geometry_limit = std::numeric_limits<int>::max();

// A cursor position moved by at most one repeat or one sixel row.
std::int64_t advanced(std::int64_t position, std::int64_t distance)
{
    return std::min(position + distance, k_sixel_geometry_limit);
}

// Data drawn before any color is selected uses the last register of the
// VT340's sixteen, as the hardware does according to microsoft/terminal.
constexpr int k_sixel_default_register = 15;

struct rgb_percent_t
{
    int red;
    int green;
    int blue;
};

// VT340 default color map in RGB percent (manual vol. 2, table 2-3).
constexpr std::array<rgb_percent_t, 16> k_vt340_default_color_map = {{
    { 0,  0,  0},
    {20, 20, 80},
    {80, 13, 13},
    {20, 80, 20},
    {80, 20, 80},
    {20, 80, 80},
    {80, 80, 20},
    {53, 53, 53},
    {26, 26, 26},
    {33, 33, 60},
    {60, 26, 26},
    {33, 60, 33},
    {60, 33, 60},
    {33, 60, 60},
    {60, 60, 33},
    {80, 80, 80},
}};

// The raster is QImage::Format_RGBA8888_Premultiplied, which is byte
// ordered, so the packed word depends on the host byte order. Every drawn
// pixel is opaque, which makes the premultiplied and straight readings agree.
constexpr std::uint32_t pack_rgba8(int red, int green, int blue)
{
    if constexpr (std::endian::native == std::endian::little) {
        return
            static_cast<std::uint32_t>(red)          |
            static_cast<std::uint32_t>(green) << 8U  |
            static_cast<std::uint32_t>(blue)  << 16U |
            0xff000000U;
    }
    else {
        return
            static_cast<std::uint32_t>(red)   << 24U |
            static_cast<std::uint32_t>(green) << 16U |
            static_cast<std::uint32_t>(blue)  << 8U  |
            0x000000ffU;
    }
}

constexpr int channel_from_percent(int percent)
{
    return (std::min(percent, 100) * 255 + 50) / 100;
}

constexpr std::uint32_t color_from_rgb_percent(int red, int green, int blue)
{
    return pack_rgba8(
        channel_from_percent(red),
        channel_from_percent(green),
        channel_from_percent(blue));
}

std::uint32_t color_from_hls(int hue, int lightness, int saturation)
{
    // DEC's hue circle has blue at 0, red at 120 and green at 240 degrees
    // (manual vol. 2, ch. 2), a third of a turn behind the circle that starts
    // at red, so the angle is rotated before the usual HSL conversion.
    const double hue_degrees = ((hue % 360) + 240) % 360;
    const double l           = std::min(lightness, 100) / 100.0;
    const double s           = std::min(saturation, 100) / 100.0;
    const double a           = s * std::min(l, 1.0 - l);

    const auto channel = [&](int n) {
        const double k = std::fmod(n + hue_degrees / 30.0, 12.0);
        const double v = l - a * std::max(-1.0, std::min({k - 3.0, 9.0 - k, 1.0}));
        return static_cast<int>(std::lround(v * 255.0));
    };

    return pack_rgba8(channel(0), channel(8), channel(4));
}

// Registers past the VT340's sixteen start with its map repeated, the color a
// VT340 shows for such a color number.
constexpr std::array<std::uint32_t, k_sixel_color_register_count> make_default_registers()
{
    std::array<std::uint32_t, k_sixel_color_register_count> registers{};
    for (std::size_t i = 0; i < registers.size(); ++i) {
        const rgb_percent_t& color = k_vt340_default_color_map[i % k_vt340_default_color_map.size()];
        registers[i] = color_from_rgb_percent(color.red, color.green, color.blue);
    }
    return registers;
}

constexpr std::array<std::uint32_t, k_sixel_color_register_count> k_default_registers =
    make_default_registers();

// P1 (manual vol. 2, ch. 14): omitted, 0, 1, 5 and 6 are 2:1, 2 is 5:1, 3
// and 4 are 3:1, 7 to 9 are 1:1. Values past the table draw square pixels,
// as OpenConsole does, so both ends of a ConPTY agree on the image height.
int pixel_aspect_ratio_from_macro_parameter(int parameter)
{
    switch (parameter) {
        case 0:
        case 1:
        case 5:
        case 6:  return 2;
        case 2:  return 5;
        case 3:
        case 4:  return 3;
        default: return 1;
    }
}

bool is_parameter_byte(unsigned char byte)
{
    return (byte >= '0' && byte <= '9') || byte == ';';
}

// The capacity usually outgrows the extent when no raster size was declared.
// Cropping by copy would move up to the whole cap in the drain slice that
// sees ST, so the image views the extent of the capacity buffer instead, and
// the view owns that buffer.
QImage view_of_extent(QImage capacity, int width, int height)
{
    if (capacity.width() == width && capacity.height() == height) {
        return capacity;
    }

    auto* owner = new QImage(std::move(capacity));
    return QImage(
        owner->bits(),
        width,
        height,
        owner->bytesPerLine(),
        owner->format(),
        [](void* buffer_owner) { delete static_cast<QImage*>(buffer_owner); },
        owner);
}

}

Sixel_decoder::Sixel_decoder()
:
    m_limit_bytes(terminal_history_ring_max_record_bytes(
        k_terminal_default_retained_history_capacity_bytes))
{}

void Sixel_decoder::begin(QByteArrayView header_parameters)
{
    start_command(Command::NONE);
    for (const char character : header_parameters) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (is_parameter_byte(byte)) {
            collect_parameter_byte(byte);
        }
    }

    // P2 1 leaves the pixels no sixel sets at their current color; 0, 2 and
    // the values the manual does not list paint them with the background.
    // P3, the horizontal grid size, has no meaning on a display.
    m_pixel_aspect_ratio = pixel_aspect_ratio_from_macro_parameter(m_parameters[0]);
    m_background_fill    = m_parameters[1] != 1;
    start_command(Command::NONE);

    m_registers         = k_default_registers;
    m_selected_register = k_sixel_default_register;
    m_raster_locked     = false;
    m_declared_width    = 0;
    m_declared_height   = 0;
    m_x                 = 0;
    m_y                 = 0;
    m_extent_width      = 0;
    m_extent_height     = 0;
    m_exceeded_bytes    = 0U;
    m_limit_exceeded    = false;
    m_limit_reported    = false;
    m_active            = true;
}

qsizetype Sixel_decoder::decode(
    QByteArrayView               data,
    std::vector<Parser_action>&  actions,
    Sixel_work_budget*           budget,
    Terminal_utf8_scan_state*    string_scan)
{
    qsizetype consumed = 0;
    for (; consumed < data.size(); ++consumed) {
        const unsigned char byte = static_cast<unsigned char>(data[consumed]);

        // Inside a UTF-8 sequence a C1 byte is data, as the parser's
        // terminator scan reads it. A draw byte a budget refuses below is
        // ASCII, which leaves the scan state as it would find it again.
        if (string_scan != nullptr &&
            !utf8_scan_consumes_byte(byte, *string_scan) &&
            (byte == 0x1bU || byte == 0x18U || byte == 0x1aU ||
                byte == 0x9bU || byte == 0x9cU))
        {
            break;
        }

        if (m_command != Command::NONE && is_parameter_byte(byte)) {
            collect_parameter_byte(byte);
            continue;
        }

        // A byte's work is paid for before anything changes: its draw or
        // graphics new line, and the raster reservations it causes, whether
        // by completing raster attributes or by drawing past the capacity. A
        // byte the budget cannot pay for is left whole for later, with the
        // command it would complete.
        const bool data_byte = byte >= k_sixel_data_first && byte <= k_sixel_data_last;
        const std::uint64_t cost =
            (data_byte   ? draw_cost(byte - k_sixel_data_first) :
             byte == '-' ? band_expansion_cost()                : 0U) +
            reservation_cost(byte);
        if (cost > 0U && !try_charge_sixel_work(budget, cost)) {
            break;
        }

        int repeat = 1;
        if (m_command != Command::NONE) {
            repeat = finish_command();
        }

        if (data_byte) {
            draw_sixel(byte - k_sixel_data_first, repeat);
            continue;
        }

        // Anything else, controls included, is not sixel data and is ignored.
        switch (byte) {
            case '!': start_command(Command::REPEAT);            break;
            case '#': start_command(Command::COLOR);             break;
            case '"': start_command(Command::RASTER_ATTRIBUTES); break;
            case '$': m_x = 0;                                   break;
            case '-': next_line();                               break;
            default:                                             break;
        }
    }

    emit_limit_diagnostic(actions);
    return consumed;
}

std::uint64_t Sixel_decoder::finish_cost() const
{
    const bool fills = m_background_fill && !m_limit_exceeded;
    return band_expansion_cost() +
        (fills ? static_cast<std::uint64_t>(m_extent_width * m_extent_height) : 0U);
}

void Sixel_decoder::finish(std::vector<Parser_action>& actions, Sixel_work_budget* budget)
{
    // A command still collecting parameters completes at the terminator, so
    // a trailing color definition applies. A repeat has nothing to repeat.
    const std::uint64_t reserved_before = m_reserved_pixels;
    finish_command();

    // The cap may have shrunk since the image last grew. The end is one step
    // that always runs, so what it allocates is charged afterwards.
    grow_extent_within_limit();
    if (budget != nullptr) {
        budget->charge_after(finish_cost() + (m_reserved_pixels - reserved_before));
    }
    expand_band();
    emit_limit_diagnostic(actions);

    Screen_sixel_image_mutation image;
    image.width              = static_cast<int>(m_extent_width);
    image.height             = static_cast<int>(m_extent_height);
    image.final_cursor_y     = static_cast<int>(m_y);
    image.pixel_aspect_ratio = m_pixel_aspect_ratio;

    if (!m_limit_exceeded && m_extent_width > 0 && m_extent_height > 0) {
        if (m_background_fill) {
            // The pixels no sixel set take register 0 as it stands at the end
            // of the image, which is what the VT340's indexed bitmap shows.
            const std::uint32_t background = m_registers[0];
            for (std::int64_t y = 0; y < m_extent_height; ++y) {
                std::uint32_t* row = m_pixels + y * m_stride_pixels;
                std::replace(row, row + m_extent_width, std::uint32_t{0U}, background);
            }
        }

        image.raster = view_of_extent(std::move(m_raster), image.width, image.height);
    }

    actions.push_back({Screen_mutation{std::move(image)}});
    release();
    m_active = false;
}

void Sixel_decoder::abort()
{
    release();
    m_active = false;
}

void Sixel_decoder::start_command(Command command)
{
    m_command         = command;
    m_parameters      = {};
    m_parameter_index = 0;
}

void Sixel_decoder::collect_parameter_byte(unsigned char byte)
{
    // Parameters past the fifth, the most any sixel command takes, are
    // ignored. An omitted parameter reads as 0 (manual vol. 2, ch. 14).
    const int parameter_count = static_cast<int>(m_parameters.size());
    if (byte == ';') {
        m_parameter_index = std::min(m_parameter_index + 1, parameter_count);
        return;
    }

    if (m_parameter_index < parameter_count) {
        int& value = m_parameters[static_cast<std::size_t>(m_parameter_index)];
        value = std::min(value * 10 + (byte - '0'), k_sixel_parameter_limit);
    }
}

int Sixel_decoder::finish_command()
{
    const Command command = m_command;
    m_command = Command::NONE;

    switch (command) {
        case Command::REPEAT:
            // A count of 0 draws the sixel once, like an omitted count.
            return std::max(m_parameters[0], 1);
        case Command::COLOR:
            apply_color_introducer();
            return 1;
        case Command::RASTER_ATTRIBUTES:
            apply_raster_attributes();
            return 1;
        case Command::NONE:
        default:
            return 1;
    }
}

void Sixel_decoder::apply_color_introducer()
{
    // Registers are private to the image. A color number past the register
    // file wraps into it, the way a smaller register file maps it. With
    // coordinates the introducer defines the register as well as selecting
    // it; an unknown coordinate system only selects.
    const std::size_t color_register =
        static_cast<std::size_t>(m_parameters[0] % k_sixel_color_register_count);
    if (m_parameter_index > 0) {
        const int x = m_parameters[2];
        const int y = m_parameters[3];
        const int z = m_parameters[4];
        switch (m_parameters[1]) {
            case 1:  m_registers[color_register] = color_from_hls(x, y, z);         break;
            case 2:  m_registers[color_register] = color_from_rgb_percent(x, y, z); break;
            default:                                                                break;
        }
    }

    m_selected_register = static_cast<int>(color_register);
}

void Sixel_decoder::apply_raster_attributes()
{
    // The manual has raster attributes precede the sixel data. Holding to
    // that keeps one pixel aspect ratio for the whole image, so every band
    // expands its rows by the same amount.
    if (m_raster_locked) {
        return;
    }

    // Pan/Pad is rounded up, as OpenConsole does, so the image rows agree
    // across a ConPTY; an omitted or zero Pad leaves the ratio alone.
    const int numerator   = m_parameters[0];
    const int denominator = m_parameters[1];
    if (denominator > 0) {
        m_pixel_aspect_ratio = std::max(1, (numerator + denominator - 1) / denominator);
    }

    // Ph and Pv are device pixels, unscaled by the aspect ratio. An omitted
    // or zero value keeps the previous one.
    if (m_parameters[2] > 0) { m_declared_width  = m_parameters[2]; }
    if (m_parameters[3] > 0) { m_declared_height = m_parameters[3]; }

    if (m_background_fill) {
        // A declared raster is painted with the background even where no
        // sixel reaches, so it is part of the image.
        m_extent_width  = std::max(m_extent_width,  m_declared_width);
        m_extent_height = std::max(m_extent_height, m_declared_height);
        grow_extent_within_limit();
        return;
    }

    // Undrawn pixels of a transparent image are not part of it, but the
    // declared raster is still the best estimate of the capacity it needs.
    if (m_declared_width * m_declared_height <= limit_pixels()) {
        reserve(m_declared_width, m_declared_height);
    }
}

// A draw writes each set bit's first row once per repeat; aspect rows follow
// in the band expansion. An empty sixel only moves the cursor.
std::uint64_t Sixel_decoder::draw_cost(int bits) const
{
    const int repeat = m_command == Command::REPEAT ? std::max(m_parameters[0], 1) : 1;
    return bits == 0
        ? 1U
        : static_cast<std::uint64_t>(repeat) *
            static_cast<std::uint64_t>(std::popcount(static_cast<unsigned int>(bits)));
}

std::uint64_t Sixel_decoder::band_expansion_cost() const
{
    if (m_pixel_aspect_ratio <= 1) {
        return 0U;
    }
    return static_cast<std::uint64_t>(m_band_width) *
        static_cast<std::uint64_t>(std::popcount(static_cast<unsigned int>(m_band_drawn_bits))) *
        static_cast<std::uint64_t>(m_pixel_aspect_ratio - 1);
}

void Sixel_decoder::draw_sixel(int bits, int repeat)
{
    m_raster_locked = true;
    if (bits != 0) {
        // The least significant bit is the top pixel, so the highest set bit
        // is the lowest pixel drawn; the rows below it stay outside the image.
        const std::int64_t drawn_rows =
            static_cast<std::int64_t>(std::bit_width(static_cast<unsigned int>(bits))) *
            m_pixel_aspect_ratio;
        m_extent_width  = std::max(m_extent_width,  advanced(m_x, repeat));
        m_extent_height = std::max(m_extent_height, advanced(m_y, drawn_rows));
        grow_extent_within_limit();

        if (!m_limit_exceeded) {
            m_band_width       = std::max(m_band_width, m_x + repeat);
            m_band_drawn_bits |= bits;
            const std::uint32_t color = m_registers[static_cast<std::size_t>(m_selected_register)];
            for (int bit = 0; bit < k_sixel_row_pixels; ++bit) {
                if ((bits & (1 << bit)) == 0) {
                    continue;
                }

                std::uint32_t* first_row =
                    m_pixels + (m_y + bit * m_pixel_aspect_ratio) * m_stride_pixels + m_x;
                std::fill_n(first_row, repeat, color);
            }
        }
    }

    m_x = advanced(m_x, repeat);
}

void Sixel_decoder::expand_band()
{
    // Color passes can repaint a band many times before DECGNL or ST. Only
    // its six source rows need those writes; repeat their final colors once
    // the band closes so aspect scaling never multiplies overdraw work.
    if (m_pixel_aspect_ratio > 1) {
        for (int bit = 0; bit < k_sixel_row_pixels; ++bit) {
            if ((m_band_drawn_bits & (1 << bit)) == 0) {
                continue;
            }

            std::uint32_t* first_row =
                m_pixels + (m_y + bit * m_pixel_aspect_ratio) * m_stride_pixels;
            for (int i = 1; i < m_pixel_aspect_ratio; ++i) {
                std::copy_n(first_row, m_band_width, first_row + i * m_stride_pixels);
            }
        }
    }

    m_band_width      = 0;
    m_band_drawn_bits = 0;
}

void Sixel_decoder::next_line()
{
    expand_band();
    m_raster_locked = true;
    m_x             = 0;
    m_y             = advanced(m_y, k_sixel_row_pixels * m_pixel_aspect_ratio);
}

std::optional<std::pair<std::int64_t, std::int64_t>> Sixel_decoder::grown_capacity(
    std::int64_t capacity_width,
    std::int64_t capacity_height,
    std::int64_t width,
    std::int64_t height) const
{
    if (width  == 0 || height == 0 ||
        (width <= capacity_width && height <= capacity_height))
    {
        return std::nullopt;
    }

    // Grow geometrically so an image that arrives one sixel or one band at a
    // time costs amortized linear copying, but never hold more than the cap.
    const std::int64_t limit = limit_pixels();
    std::int64_t grown_width  = capacity_width;
    std::int64_t grown_height = capacity_height;
    if (width  > capacity_width)  { grown_width  = std::max(width,  capacity_width  * 2); }
    if (height > capacity_height) { grown_height = std::max(height, capacity_height * 2); }

    // Near the cap, a dimension that is not growing keeps its capacity when
    // the growing one still fits beside it, and the growing one takes what
    // the cap leaves. Otherwise the slack the cap leaves around the request
    // is split evenly between the two dimensions, so an image that widens
    // and deepens band by band halves that slack with each copy instead of
    // copying on every band. The request covers every drawn pixel and fits
    // the cap, so narrowing a dimension down towards it loses none.
    if (grown_width * grown_height > limit) {
        if (height <= capacity_height && capacity_height * width <= limit) {
            grown_width = limit / capacity_height;
        }
        else
        if (width <= capacity_width && capacity_width * height <= limit) {
            grown_height = limit / capacity_width;
        }
        else {
            grown_width  = width + (limit / height - width) / 2;
            grown_height = limit / grown_width;
        }
    }
    return std::pair{grown_width, grown_height};
}

// What the raster reservations one byte causes will allocate, worked out
// before anything changes, so that the byte is paid for whole or left whole:
// the raster attributes it completes (a declared transparent raster, or a
// background raster's extent) and the extent its draw reaches. It follows
// apply_raster_attributes, draw_sixel and grow_extent_within_limit step by
// step, on copies of the state they change.
std::uint64_t Sixel_decoder::reservation_cost(unsigned char byte) const
{
    std::int64_t  capacity_width  = m_raster.width();
    std::int64_t  capacity_height = m_raster.height();
    std::int64_t  extent_width    = m_extent_width;
    std::int64_t  extent_height   = m_extent_height;
    std::int64_t  aspect          = m_pixel_aspect_ratio;
    bool          limit_exceeded  = m_limit_exceeded;
    std::uint64_t cost            = 0U;
    const auto reserve_to = [&](std::int64_t width, std::int64_t height) {
        if (const auto grown = grown_capacity(capacity_width, capacity_height, width, height)) {
            capacity_width  = grown->first;
            capacity_height = grown->second;
            cost += static_cast<std::uint64_t>(capacity_width * capacity_height);
        }
    };
    const auto grow_to_extent = [&]() {
        if (limit_exceeded) {
            return;
        }
        if (extent_width * extent_height <= limit_pixels()) {
            reserve_to(extent_width, extent_height);
            return;
        }
        limit_exceeded  = true;
        capacity_width  = 0;
        capacity_height = 0;
    };

    if (m_command == Command::RASTER_ATTRIBUTES && !m_raster_locked) {
        if (m_parameters[1] > 0) {
            aspect = std::max(1, (m_parameters[0] + m_parameters[1] - 1) / m_parameters[1]);
        }
        const std::int64_t declared_width  = m_parameters[2] > 0 ? m_parameters[2] : m_declared_width;
        const std::int64_t declared_height = m_parameters[3] > 0 ? m_parameters[3] : m_declared_height;
        if (m_background_fill) {
            extent_width  = std::max(extent_width,  declared_width);
            extent_height = std::max(extent_height, declared_height);
            grow_to_extent();
        }
        else
        if (declared_width * declared_height <= limit_pixels()) {
            reserve_to(declared_width, declared_height);
        }
    }

    const int bits = byte >= k_sixel_data_first && byte <= k_sixel_data_last
        ? byte - k_sixel_data_first
        : 0;
    if (bits != 0) {
        const int repeat = m_command == Command::REPEAT ? std::max(m_parameters[0], 1) : 1;
        extent_width  = std::max(extent_width,  advanced(m_x, repeat));
        extent_height = std::max(
            extent_height,
            advanced(
                m_y,
                static_cast<std::int64_t>(std::bit_width(static_cast<unsigned int>(bits))) * aspect));
        grow_to_extent();
    }
    return cost;
}

// Reservations are paid for by the byte that causes them (reservation_cost),
// or by an image end afterwards; this only allocates, and counts.
void Sixel_decoder::reserve(std::int64_t width, std::int64_t height)
{
    const std::optional<std::pair<std::int64_t, std::int64_t>> capacity =
        grown_capacity(m_raster.width(), m_raster.height(), width, height);
    if (!capacity.has_value()) {
        return;
    }
    const auto [grown_width, grown_height] = *capacity;

    QImage grown;
    if (m_raster.isNull()) {
        grown = QImage(static_cast<int>(grown_width), static_cast<int>(grown_height), k_sixel_raster_format);
        grown.fill(0U);
    }
    else {
        grown = m_raster.copy(0, 0, static_cast<int>(grown_width), static_cast<int>(grown_height));
    }

    // QImage reports a failed allocation with a null image, not an exception.
    if (grown.isNull()) {
        throw std::bad_alloc();
    }
    m_reserved_pixels += static_cast<std::uint64_t>(grown_width * grown_height);

    m_raster        = std::move(grown);
    m_pixels        = reinterpret_cast<std::uint32_t*>(m_raster.bits());
    m_stride_pixels = m_raster.bytesPerLine() / k_sixel_bytes_per_pixel;
}

void Sixel_decoder::grow_extent_within_limit()
{
    if (m_limit_exceeded) {
        return;
    }

    const std::int64_t extent_pixels = m_extent_width * m_extent_height;
    if (extent_pixels <= limit_pixels()) {
        reserve(m_extent_width, m_extent_height);
        return;
    }

    // Unsigned, since four times a product of two saturated extents can pass
    // 2^63; it stays below 2^64.
    m_limit_exceeded = true;
    m_exceeded_bytes = static_cast<std::size_t>(
        static_cast<std::uint64_t>(extent_pixels) * static_cast<std::uint64_t>(k_sixel_bytes_per_pixel));
    release();
}

std::int64_t Sixel_decoder::limit_pixels() const
{
    return static_cast<std::int64_t>(m_limit_bytes) / k_sixel_bytes_per_pixel;
}

void Sixel_decoder::emit_limit_diagnostic(std::vector<Parser_action>& actions)
{
    if (!m_limit_exceeded || m_limit_reported) {
        return;
    }

    m_limit_reported = true;
    actions.push_back(make_payload_limit_diagnostic(
        QStringLiteral("DCS sixel"),
        m_exceeded_bytes,
        m_limit_bytes,
        Parser_sequence_family::DCS));
}

void Sixel_decoder::release()
{
    m_raster          = QImage();
    m_pixels          = nullptr;
    m_stride_pixels   = 0;
    m_band_width      = 0;
    m_band_drawn_bits = 0;
}

}
