#pragma once

#include "vnm_terminal/internal/parser_action.h"
#include "vnm_terminal/internal/utf8_scan.h"
#include <QByteArrayView>
#include <QImage>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vnm_terminal::internal {

constexpr int          k_sixel_color_register_count = 256;

// A decoded raster is RGBA8: the decoded-size cap bounds width x height x this.
constexpr std::int64_t k_sixel_bytes_per_pixel      = 4;

// The sixel work one bounded drain step may do, in units of about one pixel
// written or copied. A description of a few bytes can expand into an image as
// large as the decoded-size cap. Decoding charges the work each byte does once
// it is done, allocations included, and stops after the byte that spends the
// budget, never before a byte, so its stops are the places a chunk could end.
// Placement charges each of its steps before it runs and waits when the next
// one does not fit. The first charge of a budget always fits, so every step
// makes progress. No budget means no limit. The budget also keeps a ledger
// of its step: the units charged past the budget, and the rasters allocated
// and grid rows scrolled, which bound the step besides its units.
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
        m_overrun   += units - std::min(units, m_remaining);
        m_remaining -= std::min(units, m_remaining);
    }

    // A raster is a decoder reservation or a resampled image, up to the
    // decoded-size cap; the smaller buffers a band or a scroll makes are paid
    // for in its units. The rows are those region scrolls at the bottom
    // margin move, by placement or by line feeds.
    void count_raster_allocations(std::uint64_t rasters) { m_raster_allocations += rasters; }
    void count_rows_moved(std::uint64_t rows)            { m_rows_moved         += rows;    }

    std::uint64_t overrun()            const { return m_overrun;            }
    std::uint64_t raster_allocations() const { return m_raster_allocations; }
    std::uint64_t rows_moved()         const { return m_rows_moved;         }

private:
    std::uint64_t m_remaining;
    bool          m_charged            = false;
    bool          m_refused            = false;
    std::uint64_t m_overrun            = 0U;
    std::uint64_t m_raster_allocations = 0U;
    std::uint64_t m_rows_moved         = 0U;
};

// The sixel work one drain step may do, in Sixel_work_budget units: about two
// million pixel writes or copies, chosen so a step is a small fraction of the
// drain budget on typical hardware. The most a call does past it is one
// indivisible step, whose unit bounds sixel_placement_tests asserts.
constexpr std::uint64_t k_sixel_work_units_per_drain_step = 2000000U;

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

    // Decodes data until it ends, or until a byte's work spends the budget:
    // that byte is taken and charged, and decoding stops after it. Returns how
    // many bytes it took; the caller hands over the rest later, exactly as if
    // the chunk had ended there. With the string's UTF-8 scan state it also
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
    void decode_byte(unsigned char byte);
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

    // The rasters reservations have allocated and their pixels, so that the
    // byte or the image end that causes one is charged for it.
    std::uint64_t              m_reservations        = 0U;
    std::uint64_t              m_reserved_pixels     = 0U;

    std::size_t                m_limit_bytes;
    std::size_t                m_exceeded_bytes      = 0U;
    bool                       m_limit_exceeded      = false;
    bool                       m_limit_reported      = false;
    bool                       m_active              = false;
};

}
