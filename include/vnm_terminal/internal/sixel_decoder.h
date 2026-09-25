#pragma once

#include "vnm_terminal/internal/parser_action.h"
#include <QByteArrayView>
#include <QImage>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vnm_terminal::internal {

constexpr int k_sixel_color_register_count = 256;

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

    void set_raster_limit_bytes(std::size_t limit_bytes) { m_limit_bytes = limit_bytes; }

    bool active() const { return m_active; }

    void begin(QByteArrayView header_parameters);
    void decode(QByteArrayView data, std::vector<Parser_action>& actions);
    void finish(std::vector<Parser_action>& actions);
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
    void draw_sixel(int bits, int repeat);
    void next_line();
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

    QImage                     m_raster;
    std::uint32_t*             m_pixels              = nullptr;
    std::int64_t               m_stride_pixels       = 0;

    std::size_t                m_limit_bytes;
    std::size_t                m_exceeded_bytes      = 0U;
    bool                       m_limit_exceeded      = false;
    bool                       m_limit_reported      = false;
    bool                       m_active              = false;
};

}
