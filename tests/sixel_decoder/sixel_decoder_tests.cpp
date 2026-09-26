#include "vnm_terminal/internal/parser_action.h"
#include "vnm_terminal/internal/terminal_byte_stream_parser.h"
#include "vnm_terminal/internal/terminal_history_ring.h"
#include "vnm_terminal/internal/terminal_screen_model.h"
#include "helpers/parser_ingest.h"
#include "helpers/test_check.h"

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QtGui/qrgb.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
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
using vnm_terminal::test_helpers::ingest_all;

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
    return ingest_all(parser, bytes);
}

// Every field of every action payload, so that two action lists compare
// exactly, payloads and order.
auto fields(const term::Screen_print_text_mutation& mutation)
{
    return std::tie(mutation.text, mutation.row, mutation.column, mutation.printable_ascii_only);
}
auto fields(const term::Screen_carriage_return_mutation&) { return std::tuple<>(); }
auto fields(const term::Screen_line_feed_mutation&)       { return std::tuple<>(); }
auto fields(const term::Screen_backspace_mutation&)       { return std::tuple<>(); }
auto fields(const term::Screen_horizontal_tab_mutation&)  { return std::tuple<>(); }
auto fields(const term::Screen_bell_mutation&)            { return std::tuple<>(); }
auto fields(const term::Screen_set_title_mutation& mutation)     { return std::tie(mutation.title); }
auto fields(const term::Screen_set_icon_name_mutation& mutation) { return std::tie(mutation.icon_name); }
auto fields(const term::Screen_set_hyperlink_mutation& mutation) { return std::tie(mutation.identity_key); }
auto fields(const term::Screen_sixel_image_mutation& image)
{
    return std::tie(
        image.raster, image.width, image.height, image.final_cursor_y, image.pixel_aspect_ratio);
}
auto fields(const term::Terminal_sgr_operation& operation)
{
    return std::tie(
        operation.kind,
        operation.attributes,
        operation.color.kind,
        operation.color.palette_index,
        operation.color.rgba);
}
auto fields(const term::Parser_control_sequence& sequence)
{
    return std::tie(
        sequence.family,
        sequence.action,
        sequence.parameters,
        sequence.private_marker,
        sequence.intermediates,
        sequence.final_bytes,
        sequence.payload,
        sequence.terminator,
        sequence.raw_bytes);
}
auto fields(const term::Terminal_reply& reply)
{
    return std::tie(reply.wire_bytes, reply.source_sequence, reply.kind, reply.source_family);
}
auto fields(const term::Terminal_color_query& query)
{
    return std::tie(query.kind, query.palette_index, query.source_sequence);
}
auto fields(const term::Parser_payload_diagnostic& diagnostic)
{
    return std::tie(
        diagnostic.code,
        diagnostic.source_sequence,
        diagnostic.raw_payload_size,
        diagnostic.limit_bytes,
        diagnostic.family,
        diagnostic.recovery);
}
auto fields(const term::Parser_notification& notification)
{
    return std::tie(notification.kind, notification.text, notification.rows, notification.columns);
}
auto fields(const term::Terminal_osc52_write_request& request)
{
    return std::tie(
        request.request_id,
        request.target_selection,
        request.decoded_payload,
        request.raw_payload_size,
        request.source_sequence);
}

bool same(const term::Terminal_sgr_sequence& left, const term::Terminal_sgr_sequence& right)
{
    return left.raw_parameters == right.raw_parameters &&
        std::equal(
            left.operations.begin(), left.operations.end(),
            right.operations.begin(), right.operations.end(),
            [](const term::Terminal_sgr_operation& l, const term::Terminal_sgr_operation& r) {
                return fields(l) == fields(r);
            });
}

template <typename T>
bool same(const T& left, const T& right)
{
    return fields(left) == fields(right);
}

template <typename... Types>
bool same(const std::variant<Types...>& left, const std::variant<Types...>& right)
{
    return left.index() == right.index() &&
        std::visit(
            [&right](const auto& left_value) {
                return same(left_value, std::get<std::decay_t<decltype(left_value)>>(right));
            },
            left);
}

bool same_actions(
    const std::vector<term::Parser_action>& left,
    const std::vector<term::Parser_action>& right)
{
    return std::equal(
        left.begin(), left.end(),
        right.begin(), right.end(),
        [](const term::Parser_action& l, const term::Parser_action& r) {
            return same(l.payload, r.payload);
        });
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

bool test_sixel_scaled_color_passes()
{
    bool ok = true;

    // Ch. 14: color passes overwrite set pixels, '$' stays in the same band,
    // and '-' closes it. Every vertically repeated row must contain the
    // final color, including gaps, partial passes and the band ending at ST.
    for (const int aspect : {1, 2, 5, 17}) {
        for (const bool background : {false, true}) {
            QByteArray data = '"' + QByteArray::number(aspect) + ";1";
            if (background) {
                data += ";6;" + QByteArray::number(12 * aspect + 1);
            }
            data += "#1;2;100;0;0!3~-#2;2;0;0;100!5t$#3;2;0;100;0?A$#1!2@#0;2;100;100;100";
            const QByteArray bytes = sixel_dcs(background ? "0;0" : "0;1", data);
            for (const qsizetype chunk_size : {qsizetype{1}, bytes.size()}) {
                const std::string label =
                    "scaled color passes " + std::to_string(aspect) +
                    (background ? " opaque" : " transparent") +
                    " chunk " + std::to_string(chunk_size);
                term::Terminal_byte_stream_parser parser;
                std::vector<term::Parser_action> actions;
                for (qsizetype offset = 0; offset < bytes.size(); offset += chunk_size) {
                    for (term::Parser_action& action : ingest_all(parser, bytes.sliced(offset, chunk_size))) {
                        actions.push_back(std::move(action));
                    }
                }

                const std::vector<term::Screen_sixel_image_mutation> images = images_in(actions);
                ok &= check(images.size() == 1U,              label + ": one image");
                ok &= check(diagnostics_in(actions).empty(), label + ": no diagnostic");
                if (images.size() != 1U) {
                    continue;
                }

                const auto& image  = images.front();
                const int   width  = background ? 6 : 5;
                const int   height = 12 * aspect + (background ? 1 : 0);
                const QRgb  clear  = background ? k_white : k_transparent;
                ok &= check_size(image, width, height, label);
                ok &= check(image.final_cursor_y == 6 * aspect, label + ": final band cursor");
                for (int y = 0; y < height; ++y) {
                    const int source_row = y / aspect;
                    for (int x = 0; x < width; ++x) {
                        QRgb expected = clear;
                        if (source_row < 6 && x < 3) {
                            expected = k_red;
                        }
                        else
                        if (source_row == 6 && x < 5) {
                            expected = x < 2 ? k_red : k_blue;
                        }
                        else
                        if (source_row == 7 && x == 1) {
                            expected = k_green;
                        }
                        else
                        if ((source_row == 8 || source_row == 10 || source_row == 11) && x < 5) {
                            expected = k_blue;
                        }
                        ok &= check_pixel(image, x, y, expected, label);
                    }
                }
            }
        }
    }

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
        ingest_all(parser, sixel_dcs("0;1", "\"1;1!20~-!20~-!20~"));
    ok &= check_over_cap(grown, 20U * 18U * 4U, k_limit, 20, 18, "grown past the cap");
    const std::vector<term::Screen_sixel_image_mutation> grown_images = images_in(grown);
    ok &= check(!grown_images.empty() && grown_images[0].final_cursor_y == 12,
        "grown past the cap: cursor still tracked");

    // A transparent image is only what it draws, whatever raster it declares.
    const std::vector<term::Parser_action> declared_large =
        ingest_all(parser, sixel_dcs("0;1", "\"1;1;100;100~"));
    ok &= check(diagnostics_in(declared_large).empty(), "large declared raster alone is not over");
    const std::vector<term::Screen_sixel_image_mutation> declared_images = images_in(declared_large);
    ok &= check(declared_images.size() == 1U && !declared_images[0].raster.isNull(),
        "large declared raster: the drawn image is kept");

    // A small declared raster does not bound what the data draws.
    ok &= check_over_cap(
        ingest_all(parser, sixel_dcs("0;1", "\"1;1;2;2!300~")),
        300U * 6U * 4U,
        k_limit,
        300,
        6,
        "data past a small declared raster");

    // A filled background is part of the image, drawn or not.
    ok &= check_over_cap(
        ingest_all(parser, sixel_dcs({}, "\"1;1;100;100")),
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
        const std::vector<term::Parser_action> actions =
            ingest_all(parser, sixel_dcs("0;1", data));
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

bool test_sixel_geometry_saturates()
{
    bool ok = true;

    // Repeats of blank sixels, graphics new lines and a large aspect ratio
    // move the image cursor far past anything an image holds: here the extent
    // reaches 2^30 x 2^34 pixels, whose product is 2^64. The geometry
    // saturates at the largest value the image carries, the cap trips, and
    // the image still ends with that geometry for placement.
    QByteArray data("\"16384;1");
    data.append(QByteArray(174762, '-'));
    for (int i = 0; i < 32769; ++i) {
        data.append("!32767?");
    }
    data.append('G');

    std::vector<term::Parser_action> actions;
    try {
        actions = parse(sixel_dcs("0;1", data));
    }
    catch (const std::exception& error) {
        return check(false, std::string("far geometry: decoding threw ") + error.what());
    }

    constexpr int           k_int_max        = std::numeric_limits<int>::max();
    constexpr std::uint64_t k_expected_bytes = (1ULL << 30U) * static_cast<std::uint64_t>(k_int_max) * 4ULL;
    ok &= check_over_cap(
        actions,
        static_cast<std::size_t>(k_expected_bytes),
        term::terminal_history_ring_max_record_bytes(
            term::k_terminal_default_retained_history_capacity_bytes),
        1 << 30,
        k_int_max,
        "far geometry");
    const std::vector<term::Screen_sixel_image_mutation> images = images_in(actions);
    ok &= check(!images.empty() && images[0].final_cursor_y == k_int_max,
        "far geometry: the cursor saturates");

    // With both extents saturated, four times their product passes INT64_MAX
    // and the decoded size is still reported exactly.
    QByteArray widest("\"32767;1");
    widest.append(QByteArray(10924, '-'));
    for (int i = 0; i < 65539; ++i) {
        widest.append("!32767?");
    }
    widest.append('G');

    std::vector<term::Parser_action> widest_actions;
    try {
        widest_actions = parse(sixel_dcs("0;1", widest));
    }
    catch (const std::exception& error) {
        return check(false, std::string("saturated geometry: decoding threw ") + error.what());
    }

    constexpr std::uint64_t k_saturated_bytes =
        static_cast<std::uint64_t>(k_int_max) * static_cast<std::uint64_t>(k_int_max) * 4ULL;
    static_assert(k_saturated_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
    ok &= check_over_cap(
        widest_actions,
        static_cast<std::size_t>(k_saturated_bytes),
        term::terminal_history_ring_max_record_bytes(
            term::k_terminal_default_retained_history_capacity_bytes),
        k_int_max,
        k_int_max,
        "saturated geometry");
    const std::vector<term::Screen_sixel_image_mutation> widest_images = images_in(widest_actions);
    ok &= check(!widest_images.empty() && widest_images[0].final_cursor_y == k_int_max,
        "saturated geometry: the cursor saturates");
    return ok;
}

bool test_sixel_cap_trips_inside_a_scaled_band()
{
    bool ok = true;

    // Above 1:1 a band's rows are expanded when the band closes. When the cap
    // trips inside a band, the raster and the band waiting to be expanded go
    // together; the band still closes, later data draws nothing, and the
    // geometry is tracked to the terminator.
    constexpr std::size_t k_limit = 4096U;
    term::Terminal_byte_stream_parser parser;
    parser.set_sixel_raster_limit_bytes(k_limit);

    std::vector<term::Parser_action> actions;
    try {
        actions = ingest_all(parser, sixel_dcs("2;1", "#1!20~$!400~-!10~"));
    }
    catch (const std::exception& error) {
        return check(false, std::string("cap inside a 5:1 band: decoding threw ") + error.what());
    }

    ok &= check_over_cap(actions, 400U * 30U * 4U, k_limit, 400, 60, "cap inside a 5:1 band");
    const std::vector<term::Screen_sixel_image_mutation> images = images_in(actions);
    ok &= check(!images.empty() && images[0].final_cursor_y == 30,
        "cap inside a 5:1 band: the cursor still moves a band down");
    return ok;
}

bool test_sixel_header_limit_is_chunk_independent()
{
    bool ok = true;

    // A DCS header counts against the DCS payload limit however the stream is
    // split: a header whose parameter bytes fit the limit makes a sixel image,
    // and one byte more makes an over-limit DCS.
    const std::size_t limit = term::k_dcs_payload_limit_bytes;
    for (const std::size_t header_size : {limit, limit + 1U}) {
        const QByteArray bytes =
            QByteArray("\x1bP") + QByteArray(static_cast<qsizetype>(header_size), '0') +
            QByteArray("q~\x1b\\");
        const qsizetype final_byte = 2 + static_cast<qsizetype>(header_size);
        const bool      fits       = header_size <= limit;
        for (const qsizetype split : {
            qsizetype{0},
            qsizetype{3},
            final_byte / 2,
            final_byte - 1,
            final_byte,
            final_byte + 1,
            bytes.size() - 1})
        {
            const std::string label =
                "header of " + std::to_string(header_size) + " bytes split at " + std::to_string(split);
            term::Terminal_byte_stream_parser parser;
            std::vector<term::Parser_action> actions = ingest_all(parser, bytes.first(split));
            for (term::Parser_action& action : ingest_all(parser, bytes.sliced(split))) {
                actions.push_back(std::move(action));
            }

            const std::vector<term::Parser_payload_diagnostic> diagnostics = diagnostics_in(actions);
            ok &= check(images_in(actions).size() == (fits ? 1U : 0U), label + ": image");
            ok &= check(diagnostics.size() == (fits ? 0U : 1U),        label + ": diagnostic");
            if (!fits && diagnostics.size() == 1U) {
                ok &= check(
                    diagnostics[0].code == term::Parser_diagnostic_code::PAYLOAD_LIMIT_EXCEEDED &&
                    diagnostics[0].family == term::Parser_sequence_family::DCS &&
                    diagnostics[0].raw_payload_size == limit + 1U,
                    label + ": over-limit DCS");
            }
        }
    }

    return ok;
}

bool test_parser_stops_after_each_image()
{
    bool ok = true;

    // A few bytes can describe an image as large as the cap, so a chunk that
    // describes several must not decode them all before any is applied: the
    // parser stops after each completed image, with the text around the
    // images in order, and resumes where it stopped.
    const QByteArray image = sixel_dcs({}, "\"1;1;4;4");
    const QByteArray bytes = QByteArray("A") + image + "B" + image + image + "C";
    const qsizetype  image_size = image.size();

    term::Terminal_byte_stream_parser parser;
    const struct
    {
        qsizetype   parsed;
        std::size_t images;
        const char* text;
    }
    expected_stops[] = {
        {1 + image_size,     1U, "A"},
        {2 + 2 * image_size, 1U, "B"},
        {2 + 3 * image_size, 1U, nullptr},
        {bytes.size(),       0U, "C"},
    };

    qsizetype parsed = 0;
    for (const auto& stop : expected_stops) {
        const std::string label = "stop at " + std::to_string(stop.parsed);
        const std::vector<term::Parser_action> actions = parser.ingest(bytes, parsed);
        ok &= check(parsed == stop.parsed,                    label + ": parsed up to the image end");
        ok &= check(images_in(actions).size() == stop.images, label + ": images");
        if (stop.text != nullptr) {
            ok &= check(has_printed_text(actions, QString::fromLatin1(stop.text)), label + ": text");
        }
    }

    return ok;
}

// One label per action, enough to tell the kinds and their order apart.
std::string action_label(const term::Parser_action& action)
{
    switch (term::parser_action_kind(action)) {
        case term::Parser_action_kind::SCREEN_MUTATION:
        {
            const term::Screen_mutation& mutation = std::get<term::Screen_mutation>(action.payload);
            if (const auto* print = std::get_if<term::Screen_print_text_mutation>(&mutation)) {
                return "print:" + print->text.toStdString();
            }
            return std::get_if<term::Screen_sixel_image_mutation>(&mutation) != nullptr
                ? "image"
                : "screen";
        }
        case term::Parser_action_kind::STYLE_MUTATION:
            return "style";
        case term::Parser_action_kind::CONTROL_SEQUENCE:
            return "control:" +
                std::get<term::Parser_control_sequence>(action.payload).final_bytes.toStdString();
        case term::Parser_action_kind::DIAGNOSTIC:
            return "diagnostic:" +
                std::get<term::Parser_payload_diagnostic>(action.payload).source_sequence.toStdString();
        case term::Parser_action_kind::TERMINAL_REPLY:
            return "reply:" + std::get<term::Terminal_reply>(action.payload).source_sequence.toStdString();
        default:
            return "other";
    }
}

bool test_parser_stops_after_each_image_across_chunks()
{
    bool ok = true;

    // The stops after images hold across chunks that split a UTF-8 scalar or
    // an escape, at an image that ends exactly at a chunk end, around empty
    // input, and next to a cancelled image, a recovered one and a query:
    // every call returns at most one image, as its last action, and the
    // actions keep the order of the stream.
    const QByteArray image = sixel_dcs({}, "~");
    const std::vector<QByteArray> chunks = {
        QByteArray("\xc3"),
        QByteArray("\xa9") + image,
        QByteArray("\x1b"),
        QByteArray("Pq~\x1b\\\x1b[c"),
        QByteArray(),
        QByteArray("\x1bPq~\x18") + QByteArray("\x1bPq~\x1b[0m") + image + "Z",
    };

    term::Terminal_byte_stream_parser parser;
    std::vector<std::string> labels;
    for (const QByteArray& chunk : chunks) {
        qsizetype parsed = 0;
        do {
            const std::vector<term::Parser_action> batch = parser.ingest(chunk, parsed);
            const std::size_t batch_images = images_in(batch).size();
            ok &= check(batch_images <= 1U, "a call returns at most one image");
            if (batch_images == 1U) {
                ok &= check(action_label(batch.back()) == "image", "the image ends its call");
            }
            for (const term::Parser_action& action : batch) {
                labels.push_back(action_label(action));
            }
        }
        while (parsed < chunk.size());
    }

    const std::vector<std::string> expected = {
        "print:\xc3\xa9",
        "image",
        "image",
        "control:c",
        "diagnostic:DCS recovery",
        "style",
        "image",
        "print:Z",
    };
    ok &= check(labels == expected, "the actions keep the order of the stream");

    // The model answers the query after placing the images before it.
    term::Terminal_screen_model_config config;
    config.grid_size       = {24, 80};
    config.cell_pixel_size = term::terminal_cell_pixel_size_t{10, 20};
    term::Terminal_screen_model model(config);
    std::vector<std::string> model_labels;
    for (const QByteArray& chunk : chunks) {
        for (const term::Parser_action& action : model.ingest(chunk).actions) {
            model_labels.push_back(action_label(action));
        }
    }
    const std::vector<std::string> ordered = {"image", "image", "control:c", "reply:DA1", "image"};
    auto next = model_labels.begin();
    for (const std::string& label : ordered) {
        next = std::find(next, model_labels.end(), label);
        ok &= check(next != model_labels.end(), "the model keeps " + label + " in stream order");
        if (next == model_labels.end()) {
            break;
        }
        ++next;
    }
    return ok;
}

// With a sixel work budget the parser stops after the byte whose work spends
// it, a draw, a graphics new line, a raster reservation or an image end, and
// the bytes after it are handed over in the next step. However small the
// budget, parsing a stream step by step gives exactly the actions and images
// of parsing it whole.
bool test_sixel_budget_steps_match_a_whole_parse()
{
    bool ok = true;

    const QByteArray overdraw =
        QByteArray("\x1bP0;1q\"1;1#1;2;100;0;0#2;2;0;100;0") +
        QByteArray("#1!300~$#2!300~-").repeated(6) + QByteArray("\x1b\\");
    const std::vector<std::pair<std::string, QByteArray>> streams = {
        {"images between text",
            "A" + sixel_dcs({}, "#1~~-~") + "B" + sixel_dcs("2;1", "#1!20~$!40~-!10~") + "C"},
        {"a background raster", sixel_dcs("0;0", "\"1;1;64;48#0;2;0;0;100#1!64~")},
        {"overdraw at 2:1", overdraw},
        {"8-bit controls", QByteArray("\x90q#1!9~-~\x9c", 10) + QByteArray("x")},
        {"a cancelled and a recovered image",
            QByteArray("\x1bPq!50~\x18") + QByteArray("\x1bPq!50~\x1b[0m") + sixel_dcs({}, "~~")},
        {"a query after an image", sixel_dcs({}, "!100~-!100~") + QByteArray("\x1b[c")},
    };

    for (const auto& [name, stream] : streams) {
        const std::vector<term::Parser_action> whole = parse(stream);
        for (const std::uint64_t units : {1ULL, 7ULL, 600ULL}) {
            const std::string label = name + " in steps of " + std::to_string(units) + " units";
            term::Terminal_byte_stream_parser parser;
            std::vector<term::Parser_action> stepped;
            bool      deferred = false;
            qsizetype offset   = 0;
            for (int step = 0; offset < stream.size() && step < 100000; ++step) {
                term::Sixel_work_budget budget(units);
                const std::vector<term::Parser_action> actions =
                    parser.ingest(stream, offset, &budget);
                deferred = deferred || parser.sixel_work_deferred();
                stepped.insert(stepped.end(), actions.begin(), actions.end());
            }
            ok &= check(offset == stream.size(), label + ": the whole stream is parsed");
            ok &= check(deferred || units == 600U, label + ": a small budget defers work");
            ok &= check(same_actions(stepped, whole), label + ": exactly the same actions");
        }
    }

    // For every split of a stream into two windows, including one inside each
    // terminator, which leaves its ESC pending in the parser, parsing each
    // window in steps of one unit gives exactly the actions of parsing the
    // windows whole. The stream has images ended by both ST forms, a
    // cancelled image, one a CSI abandons, an OSC ended by ST and a query.
    const QByteArray image = sixel_dcs({}, "~~-~");
    const QByteArray image_8bit_st("\x1bPq#1;2;100;0;0~~\x9c");
    const QByteArray mixed =
        "A" + image + image_8bit_st + QByteArray("\x1b]2;t\x1b\\") +
        QByteArray("\x1bPq~~\x18") + QByteArray("\x1bPq~~\x1b[1m") + image + "\x1b[cB";
    const auto parse_windows = [](
        const QByteArray&            bytes,
        qsizetype                    split,
        std::optional<std::uint64_t> units)
    {
        term::Terminal_byte_stream_parser parser;
        std::vector<term::Parser_action> actions;
        for (const QByteArrayView window : {
            QByteArrayView(bytes).first(split), QByteArrayView(bytes).sliced(split)})
        {
            qsizetype offset = 0;
            for (int step = 0; offset < window.size() && step < 10000; ++step) {
                std::optional<term::Sixel_work_budget> budget;
                if (units.has_value()) {
                    budget.emplace(*units);
                }
                const std::vector<term::Parser_action> step_actions = parser.ingest(
                    window,
                    offset,
                    budget.has_value() ? &*budget : nullptr);
                actions.insert(actions.end(), step_actions.begin(), step_actions.end());
            }
        }
        return actions;
    };
    bool same_as_whole = true;
    for (qsizetype split = 0; split <= mixed.size(); ++split) {
        const std::vector<term::Parser_action> whole   = parse_windows(mixed, split, std::nullopt);
        const std::vector<term::Parser_action> stepped = parse_windows(mixed, split, 1U);
        same_as_whole = same_as_whole && images_in(whole).size() == 3U &&
            same_actions(stepped, whole);
    }
    ok &= check(same_as_whole, "budget steps keep every action exactly for every split");
    return ok;
}

// A raster reservation is charged to the byte that causes it. Raster
// attributes that alternate between two shapes each replace a near-cap
// transparent raster, which no draw or graphics new line pays for; the byte
// that completes them is charged for its allocation, and decoding stops after
// it once that spends the budget. So a call makes at most one such allocation
// past its budget, at a unit budget and at a drain step's budget, and the
// steps parse exactly as a whole parse does.
bool test_sixel_raster_reservations_are_budgeted()
{
    bool ok = true;

    const QByteArray alternation("\"1;1;32000;64#0\"1;1;64;32000#0");
    const QByteArray cancelled = QByteArray("\x1bP0;1q") + alternation.repeated(8) + QByteArray("\x18");
    // Where each raster attribute command completes: at the byte after its
    // parameters.
    std::vector<qsizetype> completions;
    for (qsizetype index = cancelled.indexOf('"'); index >= 0; index = cancelled.indexOf('"', index + 1)) {
        completions.push_back(cancelled.indexOf('#', index));
    }

    for (const std::uint64_t units : {1ULL, 2000000ULL}) {
        const std::string label = "alternating rasters at " + std::to_string(units) + " units";
        term::Terminal_byte_stream_parser parser;
        qsizetype offset = 0;
        int most_per_call = 0;
        int calls = 0;
        for (; offset < cancelled.size() && calls < 1000; ++calls) {
            const qsizetype before = offset;
            term::Sixel_work_budget budget(units);
            (void)parser.ingest(cancelled, offset, &budget);
            const int completed = static_cast<int>(std::count_if(
                completions.begin(),
                completions.end(),
                [before, offset](qsizetype completion) {
                    return completion >= before && completion < offset;
                }));
            most_per_call = std::max(most_per_call, completed);
        }
        ok &= check(offset == cancelled.size(), label + ": the whole string is parsed");
        ok &= check(most_per_call == 1 && calls >= static_cast<int>(completions.size()),
            label + ": each call completes at most one reserving raster command");
    }

    // Cut into two windows around every completion, and stepped at a unit
    // budget, the string parses exactly as whole.
    const QByteArray ended = QByteArray("\x1bP0;1q") + alternation.repeated(3) +
        QByteArray("#1;2;100;0;0#1~~\x1b\\") + QByteArray("B");
    std::vector<qsizetype> splits;
    for (qsizetype index = ended.indexOf('"'); index >= 0; index = ended.indexOf('"', index + 1)) {
        const qsizetype completion = ended.indexOf('#', index);
        splits.insert(splits.end(), {completion - 1, completion, completion + 1});
    }
    const std::vector<term::Parser_action> whole = parse(ended);
    bool same_as_whole = images_in(whole).size() == 1U;
    for (const qsizetype split : splits) {
        term::Terminal_byte_stream_parser parser;
        std::vector<term::Parser_action> stepped;
        for (const QByteArrayView window : {
                QByteArrayView(ended).first(split), QByteArrayView(ended).sliced(split)})
        {
            qsizetype offset = 0;
            for (int step = 0; offset < window.size() && step < 1000; ++step) {
                term::Sixel_work_budget budget(1U);
                const std::vector<term::Parser_action> actions = parser.ingest(window, offset, &budget);
                stepped.insert(stepped.end(), actions.begin(), actions.end());
            }
        }
        same_as_whole = same_as_whole && same_actions(stepped, whole);
    }
    ok &= check(same_as_whole,
        "unit steps cut around every raster completion keep every action exactly");
    return ok;
}

// The two inputs that once stalled or misread a suspended string, at unit
// budgets: an ignored ESC after raster attributes whose completion spends the
// budget (it was retried at no progress, forever, inside one call; the test's
// TIMEOUT bounds that), and a UTF-8 lead byte completing them whose
// continuation is ST's C1 byte (it was read as ST). Both strings end at CAN
// without an image, whole and cut right after the ESC or between the two
// UTF-8 bytes, and every call makes progress.
bool test_suspension_takes_every_byte_once()
{
    bool ok = true;

    const QByteArray prefix("\x1bP0;1q\"1;1;1;1#0\"1;1;2;2");
    const std::vector<std::pair<std::string, QByteArray>> cases = {
        {"an ignored ESC", QByteArray("\x1bX\x18", 3)},
        {"a UTF-8 lead byte", QByteArray("\xc2\x9c\x18", 3)},
    };
    for (const auto& [name, tail] : cases) {
        const QByteArray stream = prefix + tail + QByteArray("after");
        const std::vector<term::Parser_action> whole = parse(stream);
        for (const std::optional<qsizetype> split :
            {std::optional<qsizetype>{}, std::optional<qsizetype>{prefix.size() + 1}})
        {
            const std::string label = name + (split.has_value() ? ", cut inside it" : ", whole");
            term::Terminal_byte_stream_parser parser;
            std::vector<term::Parser_action> stepped;
            bool progress = true;
            qsizetype parsed = 0;
            const std::vector<QByteArrayView> windows = split.has_value()
                ? std::vector<QByteArrayView>{
                    QByteArrayView(stream).first(*split), QByteArrayView(stream).sliced(*split)}
                : std::vector<QByteArrayView>{QByteArrayView(stream)};
            for (const QByteArrayView window : windows) {
                qsizetype offset = 0;
                for (int calls = 0; offset < window.size() && calls < 1000; ++calls) {
                    const qsizetype before = offset;
                    term::Sixel_work_budget budget(1U);
                    const std::vector<term::Parser_action> actions =
                        parser.ingest(window, offset, &budget);
                    progress = progress && (offset > before || !actions.empty());
                    stepped.insert(stepped.end(), actions.begin(), actions.end());
                }
                parsed += offset;
            }
            ok &= check(parsed == stream.size() && progress,
                label + ": every call takes a byte, and the whole stream is taken");
            ok &= check(images_in(stepped).empty() && same_actions(stepped, whole),
                label + ": the string ends at CAN without an image, as parsed whole");
        }
    }
    return ok;
}

// An image end is one step that always runs, whatever is left of the budget:
// it fills and expands the raster, charges that afterwards, and the draws of
// the next image wait for a later step.
bool test_sixel_image_end_is_one_step()
{
    bool ok = true;

    // The declared 64 x 64 raster and the draw cost 4104 units, and the end's
    // 64 x 64 background fill 4096 more.
    const QByteArray stream("\x1bPq\"1;1;64;64#1!8~\x1b\\\x1bPq!3~\x1b\\");
    term::Terminal_byte_stream_parser parser;
    term::Sixel_work_budget budget(5000U);
    qsizetype offset = 0;
    std::vector<term::Parser_action> actions = parser.ingest(stream, offset, &budget);
    ok &= check(images_in(actions).size() == 1U && !parser.sixel_work_deferred(),
        "the first image ends past what is left of the budget");

    const std::vector<term::Parser_action> next = parser.ingest(stream, offset, &budget);
    ok &= check(images_in(next).empty() && parser.sixel_work_deferred() &&
            offset < stream.size(),
        "with the budget spent, the next image's data waits for a later step");

    term::Sixel_work_budget fresh(5000U);
    const std::vector<term::Parser_action> rest = parser.ingest(stream, offset, &fresh);
    actions.insert(actions.end(), next.begin(), next.end());
    actions.insert(actions.end(), rest.begin(), rest.end());
    const std::vector<term::Screen_sixel_image_mutation> images = images_in(actions);
    const std::vector<term::Screen_sixel_image_mutation> whole  = images_in(parse(stream));
    ok &= check(offset == stream.size() && images.size() == 2U && whole.size() == 2U &&
            images[0].raster == whole[0].raster && images[1].raster == whole[1].raster,
        "a later step finishes the next image as a whole parse does");
    return ok;
}

bool test_model_supplies_the_cap()
{
    bool ok = true;

    // The cap is the retained history's largest record and follows the ring
    // capacity, including a change while an image streams. The model has a
    // cell pixel size, so the images are placed and the cap's diagnostic is
    // the only one.
    term::Terminal_screen_model_config config;
    config.grid_size       = {24, 80};
    config.cell_pixel_size = term::terminal_cell_pixel_size_t{10, 20};
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
    ok &= test_sixel_scaled_color_passes();
    ok &= test_sixel_colors();
    ok &= test_sixel_aspect_ratio();
    ok &= test_sixel_background();
    ok &= test_sixel_decoded_size_cap();
    ok &= test_sixel_growth_near_the_cap();
    ok &= test_sixel_geometry_saturates();
    ok &= test_sixel_cap_trips_inside_a_scaled_band();
    ok &= test_sixel_header_limit_is_chunk_independent();
    ok &= test_parser_stops_after_each_image();
    ok &= test_parser_stops_after_each_image_across_chunks();
    ok &= test_sixel_budget_steps_match_a_whole_parse();
    ok &= test_sixel_image_end_is_one_step();
    ok &= test_sixel_raster_reservations_are_budgeted();
    ok &= test_suspension_takes_every_byte_once();
    ok &= test_model_supplies_the_cap();
    return ok ? 0 : 1;
}
