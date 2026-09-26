#pragma once

#include "vnm_terminal/internal/parser_action.h"
#include "vnm_terminal/internal/utf8_scan.h"
#include <QByteArrayView>
#include <QImage>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace vnm_terminal::internal {

constexpr int          k_sixel_color_register_count = 256;

// A decoded raster is RGBA8: the decoded-size cap bounds width x height x this.
constexpr std::int64_t k_sixel_bytes_per_pixel      = 4;

// The sixel work one bounded drain step may do, in units of about one pixel
// written or copied. A description of a few bytes can expand into an image as
// large as the decoded-size cap, so decoding and placement charge each step
// before it runs and stop when the next one does not fit; the caller resumes
// them in a later step. The first charge of a budget always fits, so every
// step makes progress. A raster reservation is priced from the byte that
// causes it and paid for with that byte, before anything changes; only an
// image end is charged afterwards. No budget means no limit.
class Sixel_work_budget final
{
public:
    explicit Sixel_work_budget(std::uint64_t units)
    :
        m_remaining(units)
    {}

    bool try_charge(std::uint64_t units)
    {
        if (m_charged && units > m_remaining) {
            m_refused = true;
            return false;
        }
        charge_after(units);
        return true;
    }

    // Whether the budget has nothing left or refused a step: the caller
    // returns at the next boundary rather than start more expensive work.
    bool exhausted() const { return m_refused || (m_charged && m_remaining == 0U); }

    // Whether work of this size fits in what is left, without the first
    // charge's allowance.
    bool fits(std::uint64_t units) const { return units <= m_remaining; }

    void charge_after(std::uint64_t units)
    {
        m_charged    = true;
        m_remaining -= std::min(units, m_remaining);
    }

private:
    std::uint64_t m_remaining;
    bool          m_charged = false;
    bool          m_refused = false;
};

inline bool try_charge_sixel_work(Sixel_work_budget* budget, std::uint64_t units)
{
    return budget == nullptr || budget->try_charge(units);
}

// Decodes the data of one sixel device control string (VT330/VT340
// Programmer Reference Manual vol. 2, chapter 14) as it streams in, so an
// image never has to be buffered as text. The parser hands it the header
// parameters when it recognizes the sixel DCS, every data byte in arrival
// order, and then either the string terminator (finish) or an abort.
//
// The decoded raster is capped at a byte size the owner supplies and may
// change at any time; the cap is checked whenever the image grows, not
// against the declared raster size alone. An image over the cap stores no
// pixels but keeps tracking its geometry, because placement still scrolls and
// moves the cursor for it.
class Sixel_decoder
{
public:
    Sixel_decoder();

    // The decoder draws through a pointer into its raster. A copy would share
    // that buffer implicitly and write through the original's pointer; a
    // move takes the buffer itself along, so the pointer stays valid.
    Sixel_decoder(const Sixel_decoder&)            = delete;
    Sixel_decoder& operator=(const Sixel_decoder&) = delete;
    Sixel_decoder(Sixel_decoder&&)                 = default;
    Sixel_decoder& operator=(Sixel_decoder&&)      = default;

    void set_raster_limit_bytes(std::size_t limit_bytes) { m_limit_bytes = limit_bytes; }

    bool active() const { return m_active; }

    void begin(QByteArrayView header_parameters);

    // Decodes data until it ends or the budget cannot pay for the next byte's
    // work (a draw, a graphics new line, a raster reservation), and returns how
    // many bytes it took; the caller hands over the rest later, and a byte
    // left over has changed nothing, the command it would complete included. With the string's UTF-8 scan state it also
    // stops before a byte that could end the string (ESC, CAN, SUB, ST or
    // CSI outside a UTF-8 sequence), and advances that state over exactly
    // the bytes it looks at, so the caller scans no byte twice.
    qsizetype decode(
        QByteArrayView               data,
        std::vector<Parser_action>&  actions,
        Sixel_work_budget*           budget      = nullptr,
        Terminal_utf8_scan_state*    string_scan = nullptr);

    // Ends the image at its string terminator. The end is one step that
    // always runs: its fill and band expansion are charged to the budget
    // afterwards, so the work after it waits for a later step.
    void finish(std::vector<Parser_action>& actions, Sixel_work_budget* budget = nullptr);
    void abort();

private:
    enum class Command
    {
        NONE,
        REPEAT,
        COLOR,
        RASTER_ATTRIBUTES,
    };

    void start_command(Command command);
    void collect_parameter_byte(unsigned char byte);
    int  finish_command();
    void apply_color_introducer();
    void apply_raster_attributes();
    std::uint64_t draw_cost(int bits) const;
    std::uint64_t band_expansion_cost() const;
    std::uint64_t finish_cost() const;
    void draw_sixel(int bits, int repeat);
    void expand_band();
    void next_line();
    // The capacity reserve() grows a raster of the given capacity to for a
    // request; none when the capacity already covers it.
    std::optional<std::pair<std::int64_t, std::int64_t>> grown_capacity(
        std::int64_t capacity_width,
        std::int64_t capacity_height,
        std::int64_t width,
        std::int64_t height) const;
    std::uint64_t reservation_cost(unsigned char byte) const;
    void reserve(std::int64_t width, std::int64_t height);
    void grow_extent_within_limit();
    std::int64_t limit_pixels() const;
    void emit_limit_diagnostic(std::vector<Parser_action>& actions);
    void release();

    std::array<std::uint32_t, k_sixel_color_register_count>
                               m_registers{};
    int                        m_selected_register   = 0;

    Command                    m_command             = Command::NONE;
    std::array<int, 5>         m_parameters{};
    int                        m_parameter_index     = 0;

    int                        m_pixel_aspect_ratio  = 1;
    bool                       m_background_fill     = true;
    bool                       m_raster_locked       = false;
    std::int64_t               m_declared_width      = 0;
    std::int64_t               m_declared_height     = 0;

    std::int64_t               m_x                   = 0;
    std::int64_t               m_y                   = 0;
    std::int64_t               m_extent_width        = 0;
    std::int64_t               m_extent_height       = 0;
    std::int64_t               m_band_width          = 0;
    int                        m_band_drawn_bits     = 0;

    QImage                     m_raster;
    std::uint32_t*             m_pixels              = nullptr;
    std::int64_t               m_stride_pixels       = 0;

    // Every pixel raster reservations have allocated, so that an image end
    // charges for the growth it causes.
    std::uint64_t              m_reserved_pixels     = 0U;

    std::size_t                m_limit_bytes;
    std::size_t                m_exceeded_bytes      = 0U;
    bool                       m_limit_exceeded      = false;
    bool                       m_limit_reported      = false;
    bool                       m_active              = false;
};

}
