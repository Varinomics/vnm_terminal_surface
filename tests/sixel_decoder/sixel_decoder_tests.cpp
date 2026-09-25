#include "vnm_terminal/internal/parser_action.h"
#include "vnm_terminal/internal/terminal_byte_stream_parser.h"
#include "vnm_terminal/internal/terminal_history_ring.h"
#include "vnm_terminal/internal/terminal_screen_model.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QtGui/qrgb.h>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// Expected behavior comes from the VT330/VT340 Programmer Reference Manual,
// vol. 2 (oracle dec-vt330-vt340-graphics-manual): chapter 14 for the sixel
// string and chapter 2 for the default color map and the HLS hue circle. The
// fixtures are written from that text. The decoded-size cap, the handling of
// aborted strings and the dispatch of other DCS strings follow the recorded
// product decisions in docs/terminal_sequence_matrix.md.

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

constexpr QRgb k_transparent = 0U;
constexpr QRgb k_black       = qRgb(0, 0, 0);
constexpr QRgb k_white       = qRgb(255, 255, 255);
constexpr QRgb k_red         = qRgb(255, 0, 0);
constexpr QRgb k_green       = qRgb(0, 255, 0);
constexpr QRgb k_blue        = qRgb(0, 0, 255);

QByteArray sixel_dcs(const QByteArray& parameters, const QByteArray& data)
{
    return QByteArray("\x1bP") + parameters + 'q' + data + QByteArray("\x1b\\");
}

std::vector<term::Screen_sixel_image_mutation> images_in(
    const std::vector<term::Parser_action>& actions)
{
    std::vector<term::Screen_sixel_image_mutation> images;
    for (const term::Parser_action& action : actions) {
        if (term::parser_action_kind(action) != term::Parser_action_kind::SCREEN_MUTATION) {
            continue;
        }

        const term::Screen_mutation& mutation = std::get<term::Screen_mutation>(action.payload);
        if (const auto* image = std::get_if<term::Screen_sixel_image_mutation>(&mutation)) {
            images.push_back(*image);
        }
    }
    return images;
}

std::vector<term::Parser_payload_diagnostic> diagnostics_in(
    const std::vector<term::Parser_action>& actions)
{
    std::vector<term::Parser_payload_diagnostic> diagnostics;
    for (const term::Parser_action& action : actions) {
        if (term::parser_action_kind(action) == term::Parser_action_kind::DIAGNOSTIC) {
            diagnostics.push_back(std::get<term::Parser_payload_diagnostic>(action.payload));
        }
    }
    return diagnostics;
}

bool has_printed_text(const std::vector<term::Parser_action>& actions, const QString& text)
{
    for (const term::Parser_action& action : actions) {
        if (term::parser_action_kind(action) != term::Parser_action_kind::SCREEN_MUTATION) {
            continue;
        }

        const term::Screen_mutation& mutation = std::get<term::Screen_mutation>(action.payload);
        const auto* print = std::get_if<term::Screen_print_text_mutation>(&mutation);
        if (print != nullptr && print->text == text) {
            return true;
        }
    }
    return false;
}

std::vector<term::Parser_action> parse(const QByteArray& bytes)
{
    term::Terminal_byte_stream_parser parser;
    return parser.ingest(bytes);
}

// Decodes a string that must yield exactly one image and no diagnostic.
term::Screen_sixel_image_mutation single_image(
    const QByteArray&  bytes,
    const std::string& label,
    bool&              ok)
{
    const std::vector<term::Parser_action>              actions = parse(bytes);
    const std::vector<term::Screen_sixel_image_mutation> images = images_in(actions);
    ok &= check(images.size() == 1U,              label + ": one image");
    ok &= check(diagnostics_in(actions).empty(), label + ": no diagnostic");
    return images.empty() ? term::Screen_sixel_image_mutation{} : images.front();
}

std::optional<QRgb> pixel_at(const term::Screen_sixel_image_mutation& image, int x, int y)
{
    if (!image.raster.valid(x, y)) {
        return std::nullopt;
    }
    return image.raster.pixel(x, y);
}

bool check_size(
    const term::Screen_sixel_image_mutation&   image,
    int                                        width,
    int                                        height,
    const std::string&                         label)
{
    bool ok = true;
    ok &= check(image.width  == width,  label + ": width");
    ok &= check(image.height == height, label + ": height");
    ok &= check(image.raster.isNull() ||
        (image.raster.width() == width && image.raster.height() == height),
        label + ": raster size matches the extent");
    return ok;
}

bool check_pixel(
    const term::Screen_sixel_image_mutation&   image,
    int                                        x,
    int                                        y,
    QRgb                                       expected,
    const std::string&                         label)
{
    return check(pixel_at(image, x, y) == expected,
        label + ": pixel " + std::to_string(x) + "," + std::to_string(y));
}

// The color a lone six-pixel sixel is drawn in, square pixels, no background.
std::optional<QRgb> sixel_color(const QByteArray& color_commands, const std::string& label, bool& ok)
{
    const term::Screen_sixel_image_mutation image =
        single_image(sixel_dcs("0;1", "\"1;1" + color_commands + '~'), label, ok);
    return pixel_at(image, 0, 0);
}

bool test_sixel_dcs_dispatch()
{
    bool ok = true;

    // Ch. 14: 'q' marks the device control string as sixel data, introduced
    // by DCS or its 7-bit form ESC P and ended by ST or ESC \.
    const std::vector<term::Parser_action> seven_bit = parse(sixel_dcs({}, "~"));
    ok &= check(images_in(seven_bit).size() == 1U && diagnostics_in(seven_bit).empty(),
        "7-bit sixel DCS yields one image");

    const std::vector<term::Parser_action> c1 = parse(QByteArray("\x90q~\x9c", 4));
    ok &= check(images_in(c1).size() == 1U && diagnostics_in(c1).empty(),
        "C1 sixel DCS yields one image");

    // DECRQSS and XTGETTCAP headers end in 'q' behind an intermediate, and a
    // private marker or another final byte makes a different DCS: each stays
    // an unsupported DCS diagnosed over its whole buffered payload.
    const struct
    {
        const char* bytes;
        std::size_t payload_size;
        const char* label;
    }
    other_strings[] = {
        {"\x1bP$qm\x1b\\",     3U, "DECRQSS"},
        {"\x1bP+q544e\x1b\\",  6U, "XTGETTCAP"},
        {"\x1bP?1q~\x1b\\",    4U, "private marker"},
        {"\x1bP1p\x1b\\",      2U, "other final byte"},
    };
    for (const auto& other : other_strings) {
        const std::vector<term::Parser_action>             actions     = parse(QByteArray(other.bytes));
        const std::vector<term::Parser_payload_diagnostic> diagnostics = diagnostics_in(actions);
        const std::string                                  label       = other.label;
        ok &= check(images_in(actions).empty(), label + ": no image");
        ok &= check(diagnostics.size() == 1U,   label + ": one diagnostic");
        if (diagnostics.size() == 1U) {
            ok &= check(diagnostics[0].code == term::Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
                label + ": unsupported DCS");
            ok &= check(diagnostics[0].family == term::Parser_sequence_family::DCS,
                label + ": DCS family");
            ok &= check(diagnostics[0].raw_payload_size == other.payload_size,
                label + ": payload size");
        }
    }

    // Sixel data streams to the decoder instead of the DCS buffer, so an
    // image string longer than the DCS payload limit still decodes.
    QByteArray long_data("~");
    long_data.append(QByteArray(
        static_cast<qsizetype>(term::k_dcs_payload_limit_bytes + 1024U), '$'));
    const term::Screen_sixel_image_mutation long_image =
        single_image(sixel_dcs("0;1", long_data), "sixel past the DCS limit", ok);
    ok &= check_size(long_image, 1, 12, "sixel past the DCS limit");

    return ok;
}

bool test_sixel_aborts()
{
    bool ok = true;

    // A recovery boundary abandons the image, and the control sequence that
    // cut it off still runs.
    const std::vector<term::Parser_action> recovered =
        parse(QByteArray("\x1bPq#1~\x1b[0mB"));
    const std::vector<term::Parser_payload_diagnostic> recovery_diagnostics =
        diagnostics_in(recovered);
    ok &= check(images_in(recovered).empty(), "recovered sixel yields no image");
    ok &= check(recovery_diagnostics.size() == 1U &&
        recovery_diagnostics[0].code == term::Parser_diagnostic_code::MALFORMED_INPUT &&
        recovery_diagnostics[0].recovery == term::Parser_recovery_strategy::RESET_TO_GROUND,
        "recovered sixel reports the DCS recovery");
    ok &= check(has_printed_text(recovered, QStringLiteral("B")),
        "text after a recovered sixel prints");

    // CAN and SUB cancel the image without a diagnostic and return to text.
    for (const char cancel : {'\x18', '\x1a'}) {
        QByteArray bytes("\x1bPq#1~");
        bytes.append(cancel);
        bytes.append('B');
        const std::vector<term::Parser_action> cancelled = parse(bytes);
        const std::string label = cancel == '\x18' ? "CAN" : "SUB";
        ok &= check(images_in(cancelled).empty(),      label + " cancels the image");
        ok &= check(diagnostics_in(cancelled).empty(), label + " is not diagnosed");
        ok &= check(has_printed_text(cancelled, QStringLiteral("B")),
            label + ": text after the cancelled sixel prints");
    }

    // The next image starts from a fresh decoder, not from the abandoned
    // repeat or color definitions.
    QByteArray after_cancel("\x1bP0;1q#1;2;100;0;0!5\x18");
    after_cancel.append(sixel_dcs("0;1", "\"1;1#1~"));
    const std::vector<term::Parser_action> fresh = parse(after_cancel);
    const std::vector<term::Screen_sixel_image_mutation> fresh_images = images_in(fresh);
    ok &= check(fresh_images.size() == 1U, "image after a cancelled one decodes");
    if (fresh_images.size() == 1U) {
        ok &= check_size(fresh_images[0], 1, 6, "image after a cancelled one");
        ok &= check_pixel(fresh_images[0], 0, 0, qRgb(51, 51, 204),
            "image after a cancelled one uses the default register 1");
    }

    return ok;
}

bool test_sixel_data_characters()
{
    bool ok = true;

    // Ch. 14: a data character is its code minus 3/15, six vertical pixels
    // with the least significant bit at the top; 't' is 110101.
    const term::Screen_sixel_image_mutation t =
        single_image(sixel_dcs("0;1", "\"1;1#1;2;100;0;0t"), "t", ok);
    ok &= check_size(t, 1, 6, "t");
    const QRgb t_column[] = {k_red, k_transparent, k_red, k_transparent, k_red, k_red};
    for (int y = 0; y < 6; ++y) {
        ok &= check_pixel(t, 0, y, t_column[y], "t");
    }

    // '?' is 000000: it draws nothing but advances the active position.
    const term::Screen_sixel_image_mutation blank_then_full =
        single_image(sixel_dcs("0;1", "\"1;1#1;2;100;0;0?~"), "?~", ok);
    ok &= check_size(blank_then_full, 2, 6, "?~");
    for (int y = 0; y < 6; ++y) {
        ok &= check_pixel(blank_then_full, 0, y, k_transparent, "?~");
        ok &= check_pixel(blank_then_full, 1, y, k_red,         "?~");
    }

    // "! Pn character" repeats the character Pn times.
    const term::Screen_sixel_image_mutation repeated =
        single_image(sixel_dcs("0;1", "\"1;1#1;2;100;0;0!12@A"), "repeat", ok);
    ok &= check_size(repeated, 13, 2, "repeat");
    ok &= check_pixel(repeated, 11, 0, k_red,         "repeat");
    ok &= check_pixel(repeated, 12, 0, k_transparent, "repeat");
    ok &= check_pixel(repeated, 12, 1, k_red,         "repeat");

    return ok;
}

bool test_sixel_carriage_return_and_next_line()
{
    bool ok = true;

    // '$' returns the active position to the left border of the same sixel
    // line, so the data after it overprints.
    const term::Screen_sixel_image_mutation overprinted = single_image(
        sixel_dcs("0;1", "\"1;1#1;2;100;0;0~~$#2;2;0;0;100?@"), "overprint", ok);
    ok &= check_size(overprinted, 2, 6, "overprint");
    ok &= check_pixel(overprinted, 0, 0, k_red,  "overprint");
    ok &= check_pixel(overprinted, 1, 0, k_blue, "overprint");
    ok &= check_pixel(overprinted, 1, 1, k_red,  "overprint");

    // '-' moves it to the left border of the next sixel line.
    const term::Screen_sixel_image_mutation next_line =
        single_image(sixel_dcs("0;1", "\"1;1#1;2;100;0;0~-@"), "next line", ok);
    ok &= check_size(next_line, 1, 7, "next line");
    ok &= check_pixel(next_line, 0, 6, k_red, "next line");
    ok &= check(next_line.final_cursor_y == 6, "next line: cursor at the second sixel line");

    // Leaving sixel mode puts the text cursor at the sixel active position,
    // the top of the current sixel line, so a trailing '-' moves it.
    const term::Screen_sixel_image_mutation trailing =
        single_image(sixel_dcs("0;1", "\"1;1~--"), "trailing next line", ok);
    ok &= check_size(trailing, 1, 6, "trailing next line");
    ok &= check(trailing.final_cursor_y == 12, "trailing next lines move the cursor");

    const term::Screen_sixel_image_mutation tall = single_image(sixel_dcs({}, "~-"), "2:1 line", ok);
    ok &= check(tall.final_cursor_y == 12, "a 2:1 sixel line is twelve pixels");

    return ok;
}

bool test_sixel_colors()
{
    bool ok = true;

    // Table 14-1: '#' Pc ; Pu ; Px ; Py ; Pz defines register Pc in HLS
    // (Pu 1) or RGB (Pu 2) and selects it; RGB is in percent.
    ok &= check(sixel_color("#1;2;20;80;60", "RGB", ok) == qRgb(51, 204, 153), "RGB percent");
    ok &= check(sixel_color("#255;2;0;100;0", "register 255", ok) == k_green,
        "registers reach 255");

    // Ch. 2: the HLS hue angles of the primaries are blue 0, red 120 and
    // green 240 degrees; lightness 100 is white and 0 is black.
    ok &= check(sixel_color("#1;1;0;50;100",   "HLS blue",  ok) == k_blue,  "HLS blue");
    ok &= check(sixel_color("#1;1;120;50;100", "HLS red",   ok) == k_red,   "HLS red");
    ok &= check(sixel_color("#1;1;240;50;100", "HLS green", ok) == k_green, "HLS green");
    ok &= check(sixel_color("#1;1;0;100;0",    "HLS white", ok) == k_white, "HLS white");
    ok &= check(sixel_color("#1;1;0;0;0",      "HLS black", ok) == k_black, "HLS black");

    // '#' Pc alone selects a register; undefined ones hold the VT340 default
    // color map (ch. 2, table 2-3).
    ok &= check(sixel_color("#1",  "default 1",  ok) == qRgb(51, 51, 204),   "default blue");
    ok &= check(sixel_color("#2",  "default 2",  ok) == qRgb(204, 33, 33),   "default red");
    ok &= check(sixel_color("#3",  "default 3",  ok) == qRgb(51, 204, 51),   "default green");
    ok &= check(sixel_color("#7",  "default 7",  ok) == qRgb(135, 135, 135), "default gray 50%");
    ok &= check(sixel_color("#15", "default 15", ok) == qRgb(204, 204, 204), "default gray 75%");

    // Registers are private to each image (xterm mode 1070, permanently
    // set): a definition in one image does not reach the next.
    QByteArray two_images = sixel_dcs("0;1", "\"1;1#1;2;100;0;0~");
    two_images.append(sixel_dcs("0;1", "\"1;1#1~"));
    const std::vector<term::Screen_sixel_image_mutation> images = images_in(parse(two_images));
    ok &= check(images.size() == 2U, "two images decode");
    if (images.size() == 2U) {
        ok &= check_pixel(images[0], 0, 0, k_red,             "first image defines register 1");
        ok &= check_pixel(images[1], 0, 0, qRgb(51, 51, 204), "second image sees the default");
    }

    return ok;
}

bool test_sixel_aspect_ratio()
{
    bool ok = true;

    // Ch. 14, P1: omitted, 0, 1, 5 and 6 are 2:1, 2 is 5:1, 3 and 4 are 3:1,
    // 7 to 9 are 1:1 (vertical:horizontal).
    const struct
    {
        const char* macro_parameter;
        int         aspect_ratio;
    }
    macro_table[] = {
        {"",  2}, {"0", 2}, {"1", 2}, {"2", 5}, {"3", 3}, {"4", 3},
        {"5", 2}, {"6", 2}, {"7", 1}, {"8", 1}, {"9", 1},
    };
    for (const auto& entry : macro_table) {
        const std::string label = std::string("P1 '") + entry.macro_parameter + "'";
        const term::Screen_sixel_image_mutation image =
            single_image(sixel_dcs(entry.macro_parameter, "~"), label, ok);
        ok &= check(image.pixel_aspect_ratio == entry.aspect_ratio, label + ": aspect ratio");
        ok &= check_size(image, 1, 6 * entry.aspect_ratio, label);
    }

    // Raster attributes Pan/Pad override the macro parameter.
    ok &= check_size(single_image(sixel_dcs("2", "\"1;1~"), "1:1 over P1 2", ok), 1, 6,
        "1:1 over P1 2");
    ok &= check_size(single_image(sixel_dcs("7", "\"2;1~"), "2:1 over P1 7", ok), 1, 12,
        "2:1 over P1 7");

    // Each sixel pixel covers that many rows: at 2:1 bit 1 fills rows 2 and 3.
    const term::Screen_sixel_image_mutation scaled =
        single_image(sixel_dcs("0;1", "\"2;1#1;2;100;0;0A"), "2:1 pixel", ok);
    ok &= check_size(scaled, 1, 4, "2:1 pixel");
    ok &= check_pixel(scaled, 0, 1, k_transparent, "2:1 pixel");
    ok &= check_pixel(scaled, 0, 2, k_red,         "2:1 pixel");
    ok &= check_pixel(scaled, 0, 3, k_red,         "2:1 pixel");

    // P3, the horizontal grid size, is ignored.
    ok &= check_size(single_image(sixel_dcs("7;1;9", "~"), "P3", ok), 1, 6, "P3 ignored");

    return ok;
}

bool test_sixel_background()
{
    bool ok = true;

    // Ch. 14, P2: with 0 or 2 (the default) pixel positions specified as 0
    // are set to the background color, register 0; with 1 they remain at
    // their current color, which leaves them transparent in the image.
    for (const char* filled : {"", "0;0", "0;2"}) {
        const std::string label = std::string("P2 '") + filled + "'";
        const term::Screen_sixel_image_mutation image =
            single_image(sixel_dcs(filled, "\"1;1#1;2;100;0;0~A"), label, ok);
        ok &= check_pixel(image, 1, 0, k_black, label + ": zero bit is background");
        ok &= check_pixel(image, 1, 1, k_red,   label + ": set bit is drawn");
    }

    const term::Screen_sixel_image_mutation transparent =
        single_image(sixel_dcs("0;1", "\"1;1#1;2;100;0;0~A"), "P2 1", ok);
    ok &= check_pixel(transparent, 1, 0, k_transparent, "P2 1: zero bit keeps its color");

    // Product decision: without a background the image is what its sixels
    // draw, and a declared raster only sizes the buffer.
    const term::Screen_sixel_image_mutation drawn_only = single_image(
        sixel_dcs("0;1", "\"1;1;8;12#1;2;100;0;0~"), "transparent declared raster", ok);
    ok &= check_size(drawn_only, 1, 6, "transparent declared raster");
    ok &= check_pixel(drawn_only, 0, 5, k_red, "transparent declared raster");

    // Ph and Pv give the image a background without background sixel data,
    // and they do not limit the image the data draws.
    const term::Screen_sixel_image_mutation declared =
        single_image(sixel_dcs({}, "\"1;1;4;3"), "declared raster", ok);
    ok &= check_size(declared, 4, 3, "declared raster");
    ok &= check_pixel(declared, 3, 2, k_black, "declared raster");

    const term::Screen_sixel_image_mutation overflowing =
        single_image(sixel_dcs({}, "\"1;1;2;2!4~"), "data past the raster", ok);
    ok &= check_size(overflowing, 4, 6, "data past the raster");

    const term::Screen_sixel_image_mutation blue_background =
        single_image(sixel_dcs({}, "#0;2;0;0;100\"1;1;2;2"), "register 0 background", ok);
    ok &= check_pixel(blue_background, 1, 1, k_blue, "background is register 0");

    // An omitted parameter reads as 0: here P1 0 (2:1) and P2 1.
    const term::Screen_sixel_image_mutation omitted =
        single_image(sixel_dcs(";1", "#1;2;100;0;0A"), "omitted P1", ok);
    ok &= check_size(omitted, 1, 4, "omitted P1");
    ok &= check_pixel(omitted, 0, 1, k_transparent, "omitted P1");
    ok &= check_pixel(omitted, 0, 2, k_red,         "omitted P1");

    return ok;
}

bool check_over_cap(
    const std::vector<term::Parser_action>&    actions,
    std::size_t                                decoded_bytes,
    std::size_t                                limit_bytes,
    int                                        width,
    int                                        height,
    const std::string&                         label)
{
    bool ok = true;
    const std::vector<term::Screen_sixel_image_mutation> images      = images_in(actions);
    const std::vector<term::Parser_payload_diagnostic>   diagnostics = diagnostics_in(actions);
    ok &= check(images.size() == 1U,      label + ": the image still ends");
    ok &= check(diagnostics.size() == 1U, label + ": one diagnostic");
    if (images.size() == 1U) {
        ok &= check(images[0].raster.isNull(), label + ": no pixels kept");
        ok &= check_size(images[0], width, height, label);
    }
    if (diagnostics.size() == 1U) {
        ok &= check(diagnostics[0].code == term::Parser_diagnostic_code::PAYLOAD_LIMIT_EXCEEDED,
            label + ": limit diagnostic");
        ok &= check(diagnostics[0].raw_payload_size == decoded_bytes, label + ": decoded size");
        ok &= check(diagnostics[0].limit_bytes == limit_bytes,        label + ": cap");
    }
    return ok;
}

bool test_sixel_decoded_size_cap()
{
    bool ok = true;

    // The decoded size is the extent times four bytes, checked as the image
    // grows. Past the cap the pixels go, but the geometry placement needs is
    // still tracked to the terminator.
    constexpr std::size_t k_limit = 1024U;
    term::Terminal_byte_stream_parser parser;
    parser.set_sixel_raster_limit_bytes(k_limit);

    const std::vector<term::Parser_action> grown =
        parser.ingest(sixel_dcs("0;1", "\"1;1!20~-!20~-!20~"));
    ok &= check_over_cap(grown, 20U * 18U * 4U, k_limit, 20, 18, "grown past the cap");
    const std::vector<term::Screen_sixel_image_mutation> grown_images = images_in(grown);
    ok &= check(!grown_images.empty() && grown_images[0].final_cursor_y == 12,
        "grown past the cap: cursor still tracked");

    // A transparent image is only what it draws, whatever raster it declares.
    const std::vector<term::Parser_action> declared_large =
        parser.ingest(sixel_dcs("0;1", "\"1;1;100;100~"));
    ok &= check(diagnostics_in(declared_large).empty(), "large declared raster alone is not over");
    const std::vector<term::Screen_sixel_image_mutation> declared_images = images_in(declared_large);
    ok &= check(declared_images.size() == 1U && !declared_images[0].raster.isNull(),
        "large declared raster: the drawn image is kept");

    // A small declared raster does not bound what the data draws.
    ok &= check_over_cap(
        parser.ingest(sixel_dcs("0;1", "\"1;1;2;2!300~")),
        300U * 6U * 4U,
        k_limit,
        300,
        6,
        "data past a small declared raster");

    // A filled background is part of the image, drawn or not.
    ok &= check_over_cap(
        parser.ingest(sixel_dcs({}, "\"1;1;100;100")),
        100U * 100U * 4U,
        k_limit,
        100,
        100,
        "filled raster past the cap");

    return ok;
}

bool test_sixel_growth_near_the_cap()
{
    bool ok = true;

    // Images without raster attributes grow the buffer as they draw. Close to
    // the cap it is reshaped rather than doubled, and every drawn pixel has to
    // survive each reshape.
    constexpr std::size_t k_limit = 65536U;
    term::Terminal_byte_stream_parser parser;
    parser.set_sixel_raster_limit_bytes(k_limit);
    const auto decode = [&](const QByteArray& data, const std::string& label) {
        const std::vector<term::Parser_action>              actions = parser.ingest(sixel_dcs("0;1", data));
        const std::vector<term::Screen_sixel_image_mutation> images = images_in(actions);
        ok &= check(images.size() == 1U,              label + ": one image");
        ok &= check(diagnostics_in(actions).empty(), label + ": within the cap");
        return images.empty() ? term::Screen_sixel_image_mutation{} : images.front();
    };

    // Widening and deepening together: band k is 4 (k + 1) pixels wide,
    // ending at 104 x 156 pixels, 99% of the cap.
    QByteArray staircase("\"1;1#1;2;100;0;0");
    for (int band = 0; band < 26; ++band) {
        staircase.append('!' + QByteArray::number(4 * (band + 1)) + "~-");
    }
    const term::Screen_sixel_image_mutation stairs = decode(staircase, "staircase");
    ok &= check_size(stairs, 104, 156, "staircase");
    for (int band = 0; band < 26; ++band) {
        const int         right = 4 * (band + 1);
        const std::string label = "staircase band " + std::to_string(band);
        ok &= check_pixel(stairs, right - 1, 6 * band,     k_red, label);
        ok &= check_pixel(stairs, right - 1, 6 * band + 5, k_red, label);
        if (right < 104) {
            ok &= check_pixel(stairs, right, 6 * band, k_transparent, label);
        }
    }

    // Deepening at a fixed width, ending at 40 x 408 pixels.
    QByteArray column("\"1;1#1;2;100;0;0");
    for (int band = 0; band < 68; ++band) {
        column.append("!40~-");
    }
    const term::Screen_sixel_image_mutation deep = decode(column, "column");
    ok &= check_size(deep, 40, 408, "column");
    ok &= check_pixel(deep, 0,  0,   k_red, "column");
    ok &= check_pixel(deep, 39, 407, k_red, "column");

    // Widening at a fixed height, ending at 2648 x 6 pixels.
    const term::Screen_sixel_image_mutation wide =
        decode("\"1;1#1;2;100;0;0!2048~!600~", "row");
    ok &= check_size(wide, 2648, 6, "row");
    ok &= check_pixel(wide, 0,    0, k_red, "row");
    ok &= check_pixel(wide, 2647, 5, k_red, "row");

    return ok;
}

bool test_model_supplies_the_cap()
{
    bool ok = true;

    // The cap is the retained history's largest record and follows the ring
    // capacity, including a change while an image streams.
    term::Terminal_screen_model_config config;
    config.grid_size = {24, 80};
    term::Terminal_screen_model model(config);

    const std::size_t small_limit = term::terminal_history_ring_max_record_bytes(
        term::terminal_history_ring_aligned_capacity(
            term::k_terminal_min_retained_history_capacity_bytes));
    const QByteArray  image       = sixel_dcs({}, "\"1;1;1100;1100");
    const std::size_t image_bytes = 1100U * 1100U * 4U;

    const std::vector<term::Screen_sixel_image_mutation> kept =
        images_in(model.ingest(image).actions);
    ok &= check(kept.size() == 1U && !kept[0].raster.isNull(),
        "the default ring keeps a 1100 pixel square image");

    model.set_retained_history_capacity_bytes(term::k_terminal_min_retained_history_capacity_bytes);
    ok &= check_over_cap(model.ingest(image).actions, image_bytes, small_limit, 1100, 1100,
        "the smallest ring drops it");

    model.set_retained_history_capacity_bytes(term::k_terminal_default_retained_history_capacity_bytes);
    const std::vector<term::Screen_sixel_image_mutation> restored =
        images_in(model.ingest(image).actions);
    ok &= check(restored.size() == 1U && !restored[0].raster.isNull(),
        "restoring the ring keeps it again");

    model.ingest(QByteArray("\x1bPq\"1;1;200;200~"));
    model.set_retained_history_capacity_bytes(term::k_terminal_min_retained_history_capacity_bytes);
    ok &= check_over_cap(model.ingest(QByteArray("\x1b\\")).actions, 200U * 200U * 4U, small_limit,
        200, 200, "a ring cut while the image streams");

    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_sixel_dcs_dispatch();
    ok &= test_sixel_aborts();
    ok &= test_sixel_data_characters();
    ok &= test_sixel_carriage_return_and_next_line();
    ok &= test_sixel_colors();
    ok &= test_sixel_aspect_ratio();
    ok &= test_sixel_background();
    ok &= test_sixel_decoded_size_cap();
    ok &= test_sixel_growth_near_the_cap();
    ok &= test_model_supplies_the_cap();
    return ok ? 0 : 1;
}
