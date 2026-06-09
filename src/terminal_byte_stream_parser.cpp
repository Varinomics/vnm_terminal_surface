#include "vnm_terminal/internal/terminal_byte_stream_parser.h"

#include "vnm_terminal/internal/csi_parameter_parsing.h"
#include "vnm_terminal/internal/parser_action.h"
#include "vnm_terminal/internal/terminal_style.h"
#include "vnm_terminal/internal/unicode_width.h"
#include "vnm_terminal/internal/utf8_scan.h"

#include <QByteArray>
#include <QChar>
#include <QLatin1StringView>
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace vnm_terminal::internal {

namespace {

constexpr char32_t k_replacement_codepoint = 0xfffdU;
constexpr int k_printable_ascii_scan_block_bytes = 32;

bool is_printable_ascii_byte(unsigned char byte)
{
    return byte >= k_printable_ascii_first && byte <= k_printable_ascii_last;
}

bool printable_ascii_block_is_all_printable(const char* block)
{
    // Unsigned underflow makes below-range bytes fail the span check without a branch.
    unsigned int outside_block = 0U;
    for (int i = 0; i < k_printable_ascii_scan_block_bytes; ++i) {
        const unsigned int byte = static_cast<unsigned char>(block[i]);
        outside_block |= static_cast<unsigned int>(
            byte - k_printable_ascii_first > k_printable_ascii_last - k_printable_ascii_first);
    }
    return outside_block == 0U;
}

qsizetype printable_ascii_run_end(QByteArrayView bytes, qsizetype offset)
{
    const char* data = bytes.data();
    qsizetype current = offset;
    while (current <= bytes.size() - k_printable_ascii_scan_block_bytes) {
        if (!printable_ascii_block_is_all_printable(data + current)) {
            break;
        }
        current += k_printable_ascii_scan_block_bytes;
    }

    while (current < bytes.size() &&
        is_printable_ascii_byte(static_cast<unsigned char>(data[current])))
    {
        ++current;
    }
    return current;
}

struct Sgr_parse_result
{
    bool                   ok = true;
    Terminal_sgr_sequence  sequence;
};

enum class Sgr_color_atom_layout
{
    SEMICOLON,
    COLON,
};

struct Csi_dispatch_parts
{
    bool       ok = true;
    QByteArray parameter_bytes;
    QByteArray private_marker;
    QByteArray intermediates;
    QByteArray final_bytes;
    QByteArray raw_bytes;
};

Parser_action make_screen_mutation_action(Screen_mutation mutation)
{
    return {std::move(mutation)};
}

Parser_action make_bell_notification_action();

bool append_c0_control_action(unsigned char byte, std::vector<Parser_action>& actions)
{
    switch (byte) {
        case '\r':
            actions.push_back(make_screen_mutation_action(
                Screen_mutation{Screen_carriage_return_mutation{}}));
            return true;
        case '\n':
            actions.push_back(make_screen_mutation_action(
                Screen_mutation{Screen_line_feed_mutation{}}));
            return true;
        case '\b':
            actions.push_back(make_screen_mutation_action(
                Screen_mutation{Screen_backspace_mutation{}}));
            return true;
        case '\t':
            actions.push_back(make_screen_mutation_action(
                Screen_mutation{Screen_horizontal_tab_mutation{}}));
            return true;
        case '\a':
            actions.push_back(make_screen_mutation_action(
                Screen_mutation{Screen_bell_mutation{}}));
            actions.push_back(make_bell_notification_action());
            return true;
        default:
            return false;
    }
}

std::optional<QByteArray> osc_command_body(QByteArrayView payload, QByteArrayView command)
{
    if (payload.size() < command.size() + 1 || !payload.startsWith(command)) {
        return std::nullopt;
    }

    const unsigned char separator =
        static_cast<unsigned char>(payload[static_cast<qsizetype>(command.size())]);
    if (separator != ';') {
        return std::nullopt;
    }

    return
        QByteArray(
            payload.data() + static_cast<qsizetype>(command.size()) + 1,
            payload.size() - static_cast<qsizetype>(command.size()) - 1);
}

std::optional<QByteArray> parse_osc8_id_parameter(QByteArrayView parameters)
{
    const QByteArray key    = QByteArrayLiteral("id=");
    qsizetype        offset = 0;
    while (offset <= parameters.size()) {
        const qsizetype end = parameters.indexOf(':', offset);
        const QByteArrayView part = end < 0
            ? parameters.sliced(offset)
            : parameters.sliced(offset, end - offset);

        if (part.startsWith(key)) {
            const QByteArrayView value = part.sliced(key.size());
            return QByteArray(value.data(), value.size());
        }

        if (end < 0) {
            break;
        }
        offset = end + 1;
    }

    return std::nullopt;
}

Parser_action make_title_action(QString title)
{
    return {
        Screen_mutation{Screen_set_title_mutation{std::move(title)}},
    };
}

Parser_action make_icon_name_action(QString icon_name)
{
    return {
        Screen_mutation{Screen_set_icon_name_mutation{std::move(icon_name)}},
    };
}

Parser_action make_hyperlink_action(QByteArray identity_key)
{
    return {
        Screen_mutation{Screen_set_hyperlink_mutation{std::move(identity_key)}},
    };
}

Parser_action make_bell_notification_action()
{
    return {
        Parser_notification{Parser_notification_kind::BELL_REQUESTED, {}},
    };
}

Parser_action make_title_notification_action(QString title)
{
    return {
        Parser_notification{Parser_notification_kind::TITLE_CHANGED, std::move(title)},
    };
}

Parser_action make_icon_name_notification_action(QString icon_name)
{
    return {
        Parser_notification{
            Parser_notification_kind::ICON_NAME_CHANGED,
            std::move(icon_name),
        },
    };
}

std::optional<QString> parse_osc_title_text(
    const QByteArray&              text_bytes,
    QString                        source_sequence,
    std::vector<Parser_action>&    actions)
{
    const Terminal_utf8_width_result width = measure_utf8_width(text_bytes);
    if (width.codepoints.size() > k_title_scalar_limit) {
        actions.push_back(make_title_limit_diagnostic(
            width.codepoints.size(),
            std::move(source_sequence)));
        return std::nullopt;
    }

    return QString::fromUtf8(text_bytes);
}

void append_title_actions(QString title, std::vector<Parser_action>& actions)
{
    actions.push_back(make_title_action(title));
    actions.push_back(make_title_notification_action(std::move(title)));
}

void append_icon_name_actions(QString icon_name, std::vector<Parser_action>& actions)
{
    actions.push_back(make_icon_name_action(icon_name));
    actions.push_back(make_icon_name_notification_action(std::move(icon_name)));
}

Parser_sequence_family esc_string_family(unsigned char final_byte)
{
    switch (final_byte) {
        case ']': return Parser_sequence_family::OSC;
        case 'P': return Parser_sequence_family::DCS;
        case '_': return Parser_sequence_family::APC;
        case '^': return Parser_sequence_family::PM;
        case 'X': return Parser_sequence_family::SOS;
        default:  return Parser_sequence_family::NONE;
    }
}

Parser_sequence_family c1_string_family(unsigned char byte)
{
    switch (byte) {
        case 0x9dU: return Parser_sequence_family::OSC;
        case 0x90U: return Parser_sequence_family::DCS;
        case 0x9fU: return Parser_sequence_family::APC;
        case 0x9eU: return Parser_sequence_family::PM;
        case 0x98U: return Parser_sequence_family::SOS;
        default:    return Parser_sequence_family::NONE;
    }
}

bool is_control_byte(unsigned char byte)
{
    return byte < 0x20U || byte == 0x7fU;
}

bool is_csi_final_byte(unsigned char byte)
{
    return byte >= 0x40U && byte <= 0x7eU;
}

bool is_csi_parameter_byte(unsigned char byte)
{
    return byte >= 0x30U && byte <= 0x3fU;
}

bool is_csi_private_marker_byte(unsigned char byte)
{
    return byte >= 0x3cU && byte <= 0x3fU;
}

bool is_csi_intermediate_byte(unsigned char byte)
{
    return byte >= 0x20U && byte <= 0x2fU;
}

bool string_terminator_is_valid_for_family(
    Parser_sequence_family     family,
    Parser_string_terminator   terminator)
{
    if (family == Parser_sequence_family::OSC) {
        return
            terminator == Parser_string_terminator::BEL     ||
            terminator == Parser_string_terminator::ST_7BIT ||
            terminator == Parser_string_terminator::ST_8BIT;
    }

    return
        terminator == Parser_string_terminator::ST_7BIT ||
        terminator == Parser_string_terminator::ST_8BIT;
}

bool color_component_is_valid(int value)
{
    return value >= 0 && value <= 255;
}

std::vector<int> flat_csi_parameters(QByteArrayView parameter_bytes)
{
    if (parameter_bytes.empty()) {
        return {};
    }

    std::vector<Sgr_parameter_group> groups;
    if (!parse_sgr_parameter_groups(parameter_bytes, groups)) {
        return {};
    }

    std::vector<int> parameters;
    parameters.reserve(groups.size());
    for (const Sgr_parameter_group& group : groups) {
        if (group.atoms.size() != 1U) {
            return {};
        }

        parameters.push_back(group.atoms.front().has_value ? group.atoms.front().value : 0);
    }

    return parameters;
}

bool parse_csi_dispatch_parts(
    QByteArrayView         bytes,
    qsizetype              csi_begin,
    qsizetype              final_offset,
    Csi_dispatch_parts&    parts)
{
    qsizetype raw_begin = csi_begin - 1;
    if (csi_begin                     >= 2     &&
        byte_at(bytes, csi_begin - 2) == 0x1bU &&
        byte_at(bytes, csi_begin - 1) == '[')
    {
        raw_begin = csi_begin - 2;
    }

    parts.raw_bytes   = QByteArray(
        bytes.data() + raw_begin,
        final_offset - raw_begin + 1);
    parts.final_bytes = QByteArray(1, static_cast<char>(byte_at(bytes, final_offset)));

    bool parsing_intermediates = false;
    for (qsizetype i = csi_begin; i < final_offset; ++i) {
        const unsigned char byte = byte_at(bytes, i);
        if (byte < 0x20U) {
            continue;
        }

        if (is_csi_parameter_byte(byte)) {
            if (parsing_intermediates) {
                parts.ok = false;
                return false;
            }

            if (is_csi_private_marker_byte(byte)) {
                if (!parts.private_marker.isEmpty() || !parts.parameter_bytes.isEmpty()) {
                    parts.ok = false;
                    return false;
                }

                parts.private_marker.append(static_cast<char>(byte));
                continue;
            }

            parts.parameter_bytes.append(static_cast<char>(byte));
            continue;
        }

        if (is_csi_intermediate_byte(byte)) {
            parsing_intermediates = true;
            parts.intermediates.append(static_cast<char>(byte));
            continue;
        }

        parts.ok = false;
        return false;
    }

    return true;
}

Parser_action make_csi_dispatch_action_from_parts(const Csi_dispatch_parts& parts)
{
    return {
        Parser_control_sequence{
            Parser_sequence_family::CSI,
            Parser_control_sequence_action::DISPATCH,
            flat_csi_parameters(parts.parameter_bytes),
            parts.private_marker,
            parts.intermediates,
            parts.final_bytes,
            parts.parameter_bytes,
            Parser_string_terminator::NONE,
            parts.raw_bytes,
        },
    };
}

int sgr_group_code(const Sgr_parameter_group& group)
{
    if (group.atoms.empty() || !group.atoms.front().has_value) {
        return 0;
    }

    return group.atoms.front().value;
}

bool strict_single_group_value(const Sgr_parameter_group& group, int& value)
{
    if (group.atoms.size() != 1U || !group.atoms.front().has_value) {
        return false;
    }

    value = group.atoms.front().value;
    return true;
}

void append_color_operation(
    Terminal_sgr_sequence&         sequence,
    Terminal_sgr_operation_kind    target,
    Terminal_color_ref             color)
{
    sequence.operations.push_back({target, 0U, color});
}

struct sgr_attribute_entry_t
{
    int                         code;
    Terminal_sgr_operation_kind kind;
    std::uint16_t               attributes;
};

constexpr std::uint16_t sgr_attr(Terminal_style_attribute attribute)
{
    return terminal_style_attribute_mask(attribute);
}

constexpr std::array<sgr_attribute_entry_t, 15> k_sgr_attribute_table = {{
    { 1, Terminal_sgr_operation_kind::SET_ATTRIBUTES, sgr_attr(Terminal_style_attribute::BOLD)      },
    { 2, Terminal_sgr_operation_kind::SET_ATTRIBUTES, sgr_attr(Terminal_style_attribute::FAINT)     },
    { 3, Terminal_sgr_operation_kind::SET_ATTRIBUTES, sgr_attr(Terminal_style_attribute::ITALIC)    },
    { 4, Terminal_sgr_operation_kind::SET_ATTRIBUTES, sgr_attr(Terminal_style_attribute::UNDERLINE) },
    { 5, Terminal_sgr_operation_kind::SET_ATTRIBUTES, sgr_attr(Terminal_style_attribute::BLINK)     },
    { 7, Terminal_sgr_operation_kind::SET_ATTRIBUTES, sgr_attr(Terminal_style_attribute::INVERSE)   },
    { 8, Terminal_sgr_operation_kind::SET_ATTRIBUTES, sgr_attr(Terminal_style_attribute::INVISIBLE) },
    { 9, Terminal_sgr_operation_kind::SET_ATTRIBUTES, sgr_attr(Terminal_style_attribute::STRIKE)    },
    // SGR 22 clears both bold and faint per ECMA-48 8.3.117.
    {22, Terminal_sgr_operation_kind::CLEAR_ATTRIBUTES,
        static_cast<std::uint16_t>(sgr_attr(Terminal_style_attribute::BOLD) |
            sgr_attr(Terminal_style_attribute::FAINT))},
    { 23, Terminal_sgr_operation_kind::CLEAR_ATTRIBUTES, sgr_attr(Terminal_style_attribute::ITALIC)    },
    { 24, Terminal_sgr_operation_kind::CLEAR_ATTRIBUTES, sgr_attr(Terminal_style_attribute::UNDERLINE) },
    { 25, Terminal_sgr_operation_kind::CLEAR_ATTRIBUTES, sgr_attr(Terminal_style_attribute::BLINK)     },
    { 27, Terminal_sgr_operation_kind::CLEAR_ATTRIBUTES, sgr_attr(Terminal_style_attribute::INVERSE)   },
    { 28, Terminal_sgr_operation_kind::CLEAR_ATTRIBUTES, sgr_attr(Terminal_style_attribute::INVISIBLE) },
    { 29, Terminal_sgr_operation_kind::CLEAR_ATTRIBUTES, sgr_attr(Terminal_style_attribute::STRIKE)    },
}};

const sgr_attribute_entry_t* find_sgr_attribute_entry(int code)
{
    for (const sgr_attribute_entry_t& entry : k_sgr_attribute_table) {
        if (entry.code == code) {
            return &entry;
        }
    }
    return nullptr;
}

std::optional<Terminal_color_ref> color_ref_from_sgr_color_atoms(
    std::span<const Sgr_parameter_atom>    atoms,
    Sgr_color_atom_layout                  layout)
{
    int mode = 0;
    if (atoms.empty() || !atoms.front().has_value) {
        return std::nullopt;
    }

    mode = atoms.front().value;
    if (mode == 5) {
        int palette_index = 0;
        if (layout == Sgr_color_atom_layout::SEMICOLON) {
            if (atoms.size() != 2U || !atoms[1U].has_value) {
                return std::nullopt;
            }

            palette_index = atoms[1U].value;
        }
        else {
            // Existing colon indexed-color behavior uses the last atom as the palette index.
            if (atoms.size() < 2U || !atoms.back().has_value) {
                return std::nullopt;
            }

            palette_index = atoms.back().value;
        }

        if (!palette_index_is_valid(palette_index)) {
            return std::nullopt;
        }

        return make_palette_terminal_color_ref(static_cast<std::uint16_t>(palette_index));
    }

    if (mode != 2) {
        return std::nullopt;
    }

    std::vector<int> values_after_mode;
    if (layout == Sgr_color_atom_layout::SEMICOLON) {
        if (atoms.size() != 4U) {
            return std::nullopt;
        }

        for (std::size_t i = 1U; i < atoms.size(); ++i) {
            if (!atoms[i].has_value) {
                return std::nullopt;
            }
            values_after_mode.push_back(atoms[i].value);
        }
    }
    else {
        bool seen_value_after_mode = false;
        for (std::size_t i = 1U; i < atoms.size(); ++i) {
            if (!atoms[i].has_value) {
                if (seen_value_after_mode) {
                    return std::nullopt;
                }
                continue;
            }

            seen_value_after_mode = true;
            values_after_mode.push_back(atoms[i].value);
        }
    }

    if (values_after_mode.size() < 3U || values_after_mode.size() > 4U) {
        return std::nullopt;
    }

    const std::size_t first_component = values_after_mode.size() - 3U;
    const int         red             = values_after_mode[first_component];
    const int         green           = values_after_mode[first_component + 1U];
    const int         blue            = values_after_mode[first_component + 2U];
    if (!color_component_is_valid(red) ||
        !color_component_is_valid(green) ||
        !color_component_is_valid(blue))
    {
        return std::nullopt;
    }

    return make_rgb_terminal_color_ref(rgba_from_components(red, green, blue));
}

bool append_color_operation_from_atoms(
    std::span<const Sgr_parameter_atom>        atoms,
    Sgr_color_atom_layout                      layout,
    Terminal_sgr_operation_kind                target,
    Terminal_sgr_sequence&                     sequence)
{
    const std::optional<Terminal_color_ref> color =
        color_ref_from_sgr_color_atoms(atoms, layout);
    if (!color.has_value()) {
        return false;
    }

    append_color_operation(sequence, target, *color);
    return true;
}

bool append_semicolon_color_operation(
    const std::vector<Sgr_parameter_group>&    groups,
    std::size_t&                               group_index,
    Terminal_sgr_operation_kind                target,
    Terminal_sgr_sequence&                     sequence)
{
    int mode = 0;
    if (group_index + 1U >= groups.size() ||
        !strict_single_group_value(groups[group_index + 1U], mode))
    {
        return false;
    }

    if (mode == 5) {
        int palette_index = 0;
        if (group_index + 2U >= groups.size() ||
            !strict_single_group_value(groups[group_index + 2U], palette_index))
        {
            return false;
        }

        const std::array color_atoms = {
            Sgr_parameter_atom{true, mode},
            Sgr_parameter_atom{true, palette_index},
        };
        const bool appended = append_color_operation_from_atoms(
            color_atoms,
            Sgr_color_atom_layout::SEMICOLON,
            target,
            sequence);
        if (appended) {
            group_index += 2U;
        }
        return appended;
    }

    if (mode == 2) {
        int red   = 0;
        int green = 0;
        int blue  = 0;
        if (group_index + 4U >= groups.size() ||
            !strict_single_group_value(groups[group_index + 2U], red) ||
            !strict_single_group_value(groups[group_index + 3U], green) ||
            !strict_single_group_value(groups[group_index + 4U], blue))
        {
            return false;
        }

        const std::array color_atoms = {
            Sgr_parameter_atom{true, mode},
            Sgr_parameter_atom{true, red},
            Sgr_parameter_atom{true, green},
            Sgr_parameter_atom{true, blue},
        };
        const bool appended = append_color_operation_from_atoms(
            color_atoms,
            Sgr_color_atom_layout::SEMICOLON,
            target,
            sequence);
        if (appended) {
            group_index += 4U;
        }
        return appended;
    }

    return false;
}

bool append_colon_color_operation(
    const Sgr_parameter_group&     group,
    Terminal_sgr_operation_kind    target,
    Terminal_sgr_sequence&         sequence)
{
    if (group.atoms.size() <= 1U) {
        return false;
    }

    return
        append_color_operation_from_atoms(
            std::span<const Sgr_parameter_atom>(group.atoms).subspan(1U),
            Sgr_color_atom_layout::COLON,
            target,
            sequence);
}

Sgr_parse_result parse_sgr_sequence(QByteArrayView parameter_bytes)
{
    Sgr_parse_result result;

    std::vector<Sgr_parameter_group> groups;
    if (!parse_sgr_parameter_groups(parameter_bytes, groups)) {
        result.ok = false;
        return result;
    }

    result.sequence.raw_parameters = QByteArray(parameter_bytes.data(), parameter_bytes.size());

    for (std::size_t i = 0; i < groups.size(); ++i) {
        const Sgr_parameter_group& group = groups[i];
        const int code = sgr_group_code(group);

        if (group.atoms.size() > 1U && code != 38 && code != 48) {
            result.ok = false;
            return result;
        }

        if (code == 0) {
            result.sequence.operations.push_back({Terminal_sgr_operation_kind::RESET_ALL, 0U, {}});
        }
        else
        if (const sgr_attribute_entry_t* entry = find_sgr_attribute_entry(code); entry != nullptr) {
            result.sequence.operations.push_back({entry->kind, entry->attributes, {}});
        }
        else
        if (code == 38 || code == 48) {
            const Terminal_sgr_operation_kind target = code == 38
                ? Terminal_sgr_operation_kind::SET_FOREGROUND
                : Terminal_sgr_operation_kind::SET_BACKGROUND;
            if (group.atoms.size() > 1U) {
                result.ok = append_colon_color_operation(group, target, result.sequence);
            }
            else {
                result.ok = append_semicolon_color_operation(groups, i, target, result.sequence);
            }
        }
        else
        if (code == 39) {
            append_color_operation(
                result.sequence,
                Terminal_sgr_operation_kind::SET_FOREGROUND,
                make_default_terminal_color_ref());
        }
        else
        if (code == 49) {
            append_color_operation(
                result.sequence,
                Terminal_sgr_operation_kind::SET_BACKGROUND,
                make_default_terminal_color_ref());
        }
        else
        if (code >= 30 && code <= 37) {
            append_color_operation(
                result.sequence,
                Terminal_sgr_operation_kind::SET_FOREGROUND,
                make_palette_terminal_color_ref(static_cast<std::uint16_t>(code - 30)));
        }
        else
        if (code >= 40 && code <= 47) {
            append_color_operation(
                result.sequence,
                Terminal_sgr_operation_kind::SET_BACKGROUND,
                make_palette_terminal_color_ref(static_cast<std::uint16_t>(code - 40)));
        }
        else
        if (code >= 90 && code <= 97) {
            append_color_operation(
                result.sequence,
                Terminal_sgr_operation_kind::SET_FOREGROUND,
                make_palette_terminal_color_ref(static_cast<std::uint16_t>(8 + code - 90)));
        }
        else
        if (code >= 100 && code <= 107) {
            append_color_operation(
                result.sequence,
                Terminal_sgr_operation_kind::SET_BACKGROUND,
                make_palette_terminal_color_ref(static_cast<std::uint16_t>(8 + code - 100)));
        }

        if (!result.ok) {
            return result;
        }
    }

    return result;
}

std::optional<int> parse_ascii_palette_index(QByteArrayView bytes)
{
    if (bytes.empty()) {
        return std::nullopt;
    }

    int value = 0;
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        const unsigned char byte = byte_at(bytes, i);
        if (byte < '0' || byte > '9') {
            return std::nullopt;
        }

        const int digit = byte - '0';
        if (value > (255 - digit) / 10) {
            return std::nullopt;
        }
        value = (value * 10) + digit;
    }

    return value;
}

Parser_action make_clipboard_diagnostic(
    Parser_diagnostic_code     code,
    QString                    source_sequence,
    std::size_t                raw_payload_size)
{
    return {
        Parser_payload_diagnostic{
            code,
            std::move(source_sequence),
            raw_payload_size,
            k_osc_payload_limit_bytes,
            Parser_sequence_family::OSC,
            Parser_recovery_strategy::DISCARD_STRING,
        },
    };
}

QByteArray csi_pending_prefix_without_c0(QByteArrayView bytes, qsizetype offset)
{
    QByteArray pending;
    pending.reserve(bytes.size() - offset);
    for (qsizetype i = offset; i < bytes.size(); ++i) {
        const unsigned char byte = byte_at(bytes, i);
        if (i == offset || byte >= 0x20U) {
            pending.append(static_cast<char>(byte));
        }
    }

    return pending;
}

}

bool parse_sgr_parameter_groups(
    QByteArrayView                     bytes,
    std::vector<Sgr_parameter_group>&  groups)
{
    Sgr_parameter_group current_group;
    bool        atom_has_value = false;
    int         atom_value     = 0;
    std::size_t digit_count    = 0U;
    std::size_t atom_count     = 0U;

    const auto reset_atom = [&]() {
        atom_has_value = false;
        atom_value     = 0;
        digit_count    = 0U;
    };

    const auto finish_atom = [&]() -> bool {
        if (atom_count >= k_csi_parameter_atom_limit) {
            return false;
        }

        current_group.atoms.push_back({atom_has_value, atom_value});
        ++atom_count;
        reset_atom();
        return true;
    };

    const auto finish_group = [&]() -> bool {
        if (!finish_atom()) {
            return false;
        }

        if (groups.size() >= k_csi_parameter_group_limit) {
            return false;
        }

        groups.push_back(std::move(current_group));
        current_group = {};
        return true;
    };

    for (qsizetype i = 0; i < bytes.size(); ++i) {
        const unsigned char byte = byte_at(bytes, i);
        if (byte >= '0' && byte <= '9') {
            const int digit = byte - '0';
            if (digit_count >= k_csi_parameter_digit_limit ||
                atom_value  >  (std::numeric_limits<int>::max() - digit) / 10)
            {
                return false;
            }

            atom_has_value = true;
            atom_value     = (atom_value * 10) + digit;
            ++digit_count;
            continue;
        }

        if (byte == ':') {
            if (!finish_atom()) {
                return false;
            }
            continue;
        }

        if (byte == ';') {
            if (!finish_group()) {
                return false;
            }
            continue;
        }

        return false;
    }

    return finish_group();
}

std::vector<Parser_action> Terminal_byte_stream_parser::ingest(QByteArrayView bytes)
{
    if (m_pending_prefix.isEmpty()) {
        return ingest_buffer(bytes);
    }

    QByteArray prefixed = std::move(m_pending_prefix);
    m_pending_prefix.clear();
    prefixed.append(bytes.data(), bytes.size());
    return ingest_buffer(prefixed);
}

std::vector<Parser_action> Terminal_byte_stream_parser::ingest_buffer(QByteArrayView bytes)
{
    std::vector<Parser_action> actions;
    QString print_text;
    bool    print_text_printable_ascii_only = true;

    const auto flush_print_text = [&]() {
        if (print_text.isEmpty()) {
            return;
        }

        actions.push_back(make_print_text_action(
            std::move(print_text),
            0,
            0,
            print_text_printable_ascii_only));
        print_text.clear();
        print_text_printable_ascii_only = true;
    };

    for (qsizetype offset = 0; offset < bytes.size();) {
        if (m_discarding_csi) {
            flush_print_text();
            continue_discarded_csi(bytes, offset);
            continue;
        }

        if (is_string_family(m_string_family)) {
            flush_print_text();
            continue_string(bytes, offset, actions);
            continue;
        }

        const unsigned char byte = byte_at(bytes, offset);
        if (is_printable_ascii_byte(byte)) {
            const qsizetype ascii_begin = offset;
            offset = printable_ascii_run_end(bytes, offset);

            print_text.append(QLatin1StringView(
                bytes.data() + ascii_begin,
                offset - ascii_begin));
            continue;
        }

        if (byte == 0x1bU ||
            byte == 0x9bU ||
            c1_string_family(byte) != Parser_sequence_family::NONE)
        {
            flush_print_text();
        }

        if (try_start_string(bytes, offset, actions)          == String_state_result::CONSUMED ||
            try_consume_escape_or_csi(bytes, offset, actions) == String_state_result::CONSUMED)
        {
            continue;
        }

        if (is_control_byte(byte)) {
            flush_print_text();
        }
        if (append_c0_control_action(byte, actions)) {
            ++offset;
            continue;
        }

        const Terminal_utf8_decode_step step = decode_utf8_scalar(bytes, offset);
        if (!step.ok) {
            if (should_buffer_incomplete_utf8(bytes, offset)) {
                flush_print_text();
                m_pending_prefix = QByteArray(bytes.data() + offset, bytes.size() - offset);
                break;
            }

            print_text.append(QChar::fromUcs4(k_replacement_codepoint));
            print_text_printable_ascii_only = false;
            flush_print_text();
            actions.push_back(make_malformed_recovery_diagnostic(
                QStringLiteral("invalid UTF-8"),
                Parser_sequence_family::PRINTABLE,
                Parser_recovery_strategy::IGNORE_BYTE));
            ++offset;
            continue;
        }

        if (!is_control_byte(byte)) {
            print_text.append(QChar::fromUcs4(step.codepoint));
            print_text_printable_ascii_only = false;
        }
        offset += step.bytes_consumed;
    }

    flush_print_text();
    return actions;
}

Terminal_byte_stream_parser::String_state_result Terminal_byte_stream_parser::try_start_string(
    QByteArrayView                 bytes,
    qsizetype&                     offset,
    std::vector<Parser_action>&    actions)
{
    Parser_sequence_family family        = c1_string_family(byte_at(bytes, offset));
    qsizetype              payload_begin = offset + 1;

    if (family == Parser_sequence_family::NONE && byte_at(bytes, offset) == 0x1bU) {
        if (offset + 1 >= bytes.size()) {
            m_pending_prefix = QByteArray(bytes.data() + offset, bytes.size() - offset);
            offset = bytes.size();
            return String_state_result::CONSUMED;
        }

        family = esc_string_family(byte_at(bytes, offset + 1));
        payload_begin = offset + 2;
    }

    if (!is_string_family(family)) {
        return String_state_result::NOT_STRING;
    }

    start_string(family, bytes, payload_begin, offset, actions);
    return String_state_result::CONSUMED;
}

Terminal_byte_stream_parser::String_state_result
Terminal_byte_stream_parser::try_consume_escape_or_csi(
    QByteArrayView                 bytes,
    qsizetype&                     offset,
    std::vector<Parser_action>&    actions)
{
    const unsigned char byte = byte_at(bytes, offset);
    if (byte == 0x9bU || (byte == 0x1bU && offset + 1 < bytes.size() &&
        byte_at(bytes, offset + 1) == '['))
    {
        const qsizetype csi_begin = byte == 0x9bU ? offset + 1 : offset + 2;
        for (qsizetype i = csi_begin; i < bytes.size(); ++i) {
            const unsigned char current_byte = byte_at(bytes, i);
            const qsizetype     pending_size = i - offset + 1;
            if (pending_size > static_cast<qsizetype>(k_control_sequence_pending_limit_bytes)) {
                actions.push_back(make_unsupported_sequence_diagnostic(
                    QStringLiteral("CSI"),
                    Parser_sequence_family::CSI,
                    static_cast<std::size_t>(pending_size),
                    k_control_sequence_pending_limit_bytes,
                    Parser_recovery_strategy::DISCARD_SEQUENCE));
                m_discarding_csi = true;
                offset = i;
                return String_state_result::CONSUMED;
            }

            if (current_byte == 0x1bU || current_byte == 0x9bU) {
                emit_unsupported_control(
                    QStringLiteral("CSI"), Parser_sequence_family::CSI, actions);
                offset = i;
                return String_state_result::CONSUMED;
            }

            if (current_byte < 0x20U) {
                append_c0_control_action(current_byte, actions);
                continue;
            }

            if (is_csi_final_byte(current_byte)) {
                finish_csi_sequence(bytes, csi_begin, i, actions);
                offset = i + 1;
                return String_state_result::CONSUMED;
            }
        }

        const qsizetype pending_size = bytes.size() - offset;
        if (pending_size > static_cast<qsizetype>(k_control_sequence_pending_limit_bytes)) {
            actions.push_back(make_unsupported_sequence_diagnostic(
                QStringLiteral("CSI"),
                Parser_sequence_family::CSI,
                static_cast<std::size_t>(pending_size),
                k_control_sequence_pending_limit_bytes,
                Parser_recovery_strategy::DISCARD_SEQUENCE));
            m_discarding_csi = true;
            offset = bytes.size();
            return String_state_result::CONSUMED;
        }

        m_pending_prefix = csi_pending_prefix_without_c0(bytes, offset);
        offset = bytes.size();
        return String_state_result::CONSUMED;
    }

    if (byte == 0x1bU) {
        if (offset + 1 >= bytes.size()) {
            m_pending_prefix = QByteArray(bytes.data() + offset, bytes.size() - offset);
            offset = bytes.size();
            return String_state_result::CONSUMED;
        }

        const unsigned char final_byte = byte_at(bytes, offset + 1);
        if (final_byte == 'D' || final_byte == 'E' ||
            final_byte == 'M' || final_byte == '7' ||
            final_byte == '8' || final_byte == '=' ||
            final_byte == '>')
        {
            actions.push_back(make_escape_dispatch_action(
                QByteArray(1, static_cast<char>(final_byte)),
                QByteArray(bytes.data() + offset, 2)));
            offset += 2;
            return String_state_result::CONSUMED;
        }

        emit_unsupported_control(QStringLiteral("ESC"), Parser_sequence_family::ESC, actions);
        offset += 2;
        return String_state_result::CONSUMED;
    }

    return String_state_result::NOT_STRING;
}

void Terminal_byte_stream_parser::continue_string(
    QByteArrayView                 bytes,
    qsizetype&                     offset,
    std::vector<Parser_action>&    actions)
{
    Parser_string_terminator terminator = Parser_string_terminator::END_OF_INPUT;
    const qsizetype terminator_offset =
        find_string_terminator(bytes, m_string_family, offset, terminator);
    qsizetype payload_end =
        terminator_offset >= 0 ? terminator_offset : bytes.size();

    if (terminator_offset               <  0      &&
        payload_end                     >  offset &&
        byte_at(bytes, payload_end - 1) == 0x1bU)
    {
        --payload_end;
        m_pending_prefix = QByteArray(bytes.data() + payload_end, 1);
    }

    append_string_payload(m_string_family, bytes.sliced(offset, payload_end - offset), actions);

    if (terminator_offset < 0) {
        offset = bytes.size();
        return;
    }

    finish_string(m_string_family, terminator, actions);
    if (terminator == Parser_string_terminator::RECOVERY) {
        offset = terminator_offset;
        return;
    }

    if (terminator == Parser_string_terminator::ST_7BIT) {
        offset = terminator_offset + 2;
    }
    else {
        offset = terminator_offset + 1;
    }
}

void Terminal_byte_stream_parser::start_string(
    Parser_sequence_family         family,
    QByteArrayView                 bytes,
    qsizetype                      payload_begin,
    qsizetype&                     offset,
    std::vector<Parser_action>&    actions)
{
    m_string_family = family;
    m_string_payload.clear();
    m_string_over_limit = false;
    reset_utf8_scan_state(m_string_utf8_scan_state);
    offset = payload_begin;
    continue_string(bytes, offset, actions);
}

qsizetype Terminal_byte_stream_parser::find_string_terminator(
    QByteArrayView                 bytes,
    Parser_sequence_family         family,
    qsizetype                      payload_begin,
    Parser_string_terminator&      terminator)
{
    Terminal_utf8_scan_state utf8_scan_state = m_string_utf8_scan_state;

    for (qsizetype i = payload_begin; i < bytes.size(); ++i) {
        const unsigned char byte = byte_at(bytes, i);
        // C1 controls such as ST (0x9c) and CSI (0x9b) overlap with UTF-8
        // continuation bytes. ASCII terminators such as BEL and ESC do not,
        // but keeping one scanner here makes the terminator checks below safe
        // across chunk boundaries for the whole string-control family.
        if (utf8_scan_consumes_byte(byte, utf8_scan_state)) {
            continue;
        }

        if (family == Parser_sequence_family::OSC && byte == 0x07U) {
            reset_utf8_scan_state(m_string_utf8_scan_state);
            terminator = Parser_string_terminator::BEL;
            return i;
        }
        if (byte == 0x9cU) {
            reset_utf8_scan_state(m_string_utf8_scan_state);
            terminator = Parser_string_terminator::ST_8BIT;
            return i;
        }
        if (byte == 0x1bU && i + 1 < bytes.size() && byte_at(bytes, i + 1) == '\\') {
            reset_utf8_scan_state(m_string_utf8_scan_state);
            terminator = Parser_string_terminator::ST_7BIT;
            return i;
        }
        if ((byte == 0x1bU && i + 1 < bytes.size() && byte_at(bytes, i + 1) == '[') ||
            byte == 0x9bU)
        {
            reset_utf8_scan_state(m_string_utf8_scan_state);
            terminator = Parser_string_terminator::RECOVERY;
            return i;
        }
    }

    m_string_utf8_scan_state = utf8_scan_state;
    terminator = Parser_string_terminator::END_OF_INPUT;
    return -1;
}

bool Terminal_byte_stream_parser::append_string_payload(
    Parser_sequence_family         family,
    QByteArrayView                 payload,
    std::vector<Parser_action>&    actions)
{
    if (payload.empty() || m_string_over_limit) {
        return true;
    }

    const std::size_t limit   = payload_limit_for_family(family);
    const std::size_t current = static_cast<std::size_t>(m_string_payload.size());
    const std::size_t added   = static_cast<std::size_t>(payload.size());

    if (limit > 0U && (added > limit || current > limit - added)) {
        actions.push_back(make_string_family_payload_limit_diagnostic(
            family,
            current + added));
        m_string_payload.clear();
        m_string_over_limit = true;
        return false;
    }

    m_string_payload.append(payload.data(), payload.size());
    return true;
}

void Terminal_byte_stream_parser::finish_string(
    Parser_sequence_family         family,
    Parser_string_terminator       terminator,
    std::vector<Parser_action>&    actions)
{
    const bool was_over_limit = m_string_over_limit;
    QByteArray payload        = std::move(m_string_payload);
    m_string_payload.clear();
    m_string_over_limit = false;
    reset_utf8_scan_state(m_string_utf8_scan_state);
    m_string_family = Parser_sequence_family::NONE;

    if (was_over_limit) {
        return;
    }

    if (terminator == Parser_string_terminator::RECOVERY) {
        actions.push_back(make_malformed_recovery_diagnostic(
            source_name_for_family(family) + QStringLiteral(" recovery"),
            family,
            Parser_recovery_strategy::RESET_TO_GROUND));
        return;
    }

    if (!string_terminator_is_valid_for_family(family, terminator)) {
        return;
    }

    if (family == Parser_sequence_family::OSC) {
        handle_osc_payload(std::move(payload), actions);
        return;
    }

    actions.push_back(make_unsupported_sequence_diagnostic(
        source_name_for_family(family),
        family,
        static_cast<std::size_t>(payload.size()),
        payload_limit_for_family(family),
        Parser_recovery_strategy::DISCARD_STRING));
}

void Terminal_byte_stream_parser::finish_csi_sequence(
    QByteArrayView                 bytes,
    qsizetype                      csi_begin,
    qsizetype                      final_offset,
    std::vector<Parser_action>&    actions)
{
    Csi_dispatch_parts parts;
    if (!parse_csi_dispatch_parts(bytes, csi_begin, final_offset, parts)) {
        actions.push_back(make_malformed_recovery_diagnostic(
            QStringLiteral("malformed CSI"),
            Parser_sequence_family::CSI,
            Parser_recovery_strategy::DISCARD_SEQUENCE));
        return;
    }

    const unsigned char final_byte = byte_at(bytes, final_offset);
    if (final_byte != 'm') {
        actions.push_back(make_csi_dispatch_action_from_parts(parts));
        return;
    }

    if (!parts.private_marker.isEmpty() || !parts.intermediates.isEmpty()) {
        actions.push_back(make_malformed_recovery_diagnostic(
            QStringLiteral("SGR"),
            Parser_sequence_family::CSI,
            Parser_recovery_strategy::DISCARD_SEQUENCE));
        return;
    }

    Sgr_parse_result result = parse_sgr_sequence(parts.parameter_bytes);
    if (!result.ok) {
        actions.push_back(make_malformed_recovery_diagnostic(
            QStringLiteral("SGR"),
            Parser_sequence_family::CSI,
            Parser_recovery_strategy::DISCARD_SEQUENCE));
        return;
    }

    if (!result.sequence.operations.empty()) {
        actions.push_back(make_sgr_action(std::move(result.sequence)));
    }
}

void Terminal_byte_stream_parser::handle_osc_payload(
    QByteArray                     payload,
    std::vector<Parser_action>&    actions)
{
    if (const std::optional<QByteArray> title =
        osc_command_body(payload, QByteArrayLiteral("0"));
        title.has_value())
    {
        const std::optional<QString> title_text =
            parse_osc_title_text(*title, QStringLiteral("OSC 0 title"), actions);
        if (!title_text.has_value()) {
            return;
        }

        append_icon_name_actions(*title_text, actions);
        append_title_actions(*title_text, actions);
        return;
    }

    if (const std::optional<QByteArray> icon_name =
        osc_command_body(payload, QByteArrayLiteral("1"));
        icon_name.has_value())
    {
        const std::optional<QString> icon_name_text = parse_osc_title_text(
            *icon_name,
            QStringLiteral("OSC 1 icon name"),
            actions);
        if (!icon_name_text.has_value()) {
            return;
        }

        append_icon_name_actions(*icon_name_text, actions);
        return;
    }

    if (const std::optional<QByteArray> title = osc_command_body(payload, QByteArrayLiteral("2"));
        title.has_value())
    {
        const std::optional<QString> title_text =
            parse_osc_title_text(*title, QStringLiteral("OSC 2 title"), actions);
        if (!title_text.has_value()) {
            return;
        }

        append_title_actions(*title_text, actions);
        return;
    }

    if (const std::optional<QByteArray> hyperlink =
        osc_command_body(payload, QByteArrayLiteral("8"));
        hyperlink.has_value())
    {
        const qsizetype separator = hyperlink->indexOf(';');
        if (separator < 0) {
            actions.push_back(make_malformed_recovery_diagnostic(
                QStringLiteral("OSC 8"),
                Parser_sequence_family::OSC,
                Parser_recovery_strategy::DISCARD_STRING));
            return;
        }

        const QByteArrayView parameters(hyperlink->constData(), separator);
        const QByteArrayView uri(
            hyperlink->constData() + separator + 1,
            hyperlink->size() - separator - 1);

        if (uri.isEmpty()) {
            actions.push_back(make_hyperlink_action({}));
            return;
        }

        QByteArray identity_key = QByteArrayLiteral("uri:");
        if (const std::optional<QByteArray> id = parse_osc8_id_parameter(parameters);
            id.has_value())
        {
            identity_key = QByteArrayLiteral("id:");
            identity_key.append(*id);
            identity_key.append('\x1f');
            identity_key.append(uri.data(), uri.size());
        }
        else {
            identity_key.append(uri.data(), uri.size());
        }

        actions.push_back(make_hyperlink_action(std::move(identity_key)));
        return;
    }

    if (payload == QByteArrayLiteral("10;?")) {
        actions.push_back(make_color_query_action(
            Terminal_color_query_kind::DEFAULT_FOREGROUND,
            0,
            QStringLiteral("OSC 10")));
        return;
    }

    if (payload == QByteArrayLiteral("11;?")) {
        actions.push_back(make_color_query_action(
            Terminal_color_query_kind::DEFAULT_BACKGROUND,
            0,
            QStringLiteral("OSC 11")));
        return;
    }

    if (payload == QByteArrayLiteral("12;?")) {
        actions.push_back(make_color_query_action(
            Terminal_color_query_kind::CURSOR,
            0,
            QStringLiteral("OSC 12")));
        return;
    }

    if (payload.startsWith("4;") && payload.endsWith(";?")) {
        const QByteArray         index_bytes   = payload.mid(2, payload.size() - 4);
        const std::optional<int> palette_index = parse_ascii_palette_index(index_bytes);
        if (!palette_index.has_value()) {
            actions.push_back(make_unsupported_sequence_diagnostic(
                QStringLiteral("OSC color query"),
                Parser_sequence_family::OSC,
                static_cast<std::size_t>(payload.size()),
                k_osc_payload_limit_bytes,
                Parser_recovery_strategy::DISCARD_STRING));
            return;
        }

        actions.push_back(make_color_query_action(
            Terminal_color_query_kind::PALETTE_INDEX,
            *palette_index,
            QStringLiteral("OSC 4")));
        return;
    }

    if (payload.startsWith("52;")) {
        const QByteArray body      = payload.mid(3);
        const qsizetype  separator = body.indexOf(';');
        if (separator < 0) {
            actions.push_back(make_clipboard_diagnostic(
                Parser_diagnostic_code::CLIPBOARD_WRITE_DENIED,
                QStringLiteral("OSC 52"),
                static_cast<std::size_t>(payload.size())));
            return;
        }

        const QByteArray target_selection_bytes = body.left(separator);
        const QByteArray data_bytes             = body.mid(separator + 1);
        if (data_bytes == QByteArrayLiteral("?")) {
            actions.push_back(make_clipboard_diagnostic(
                Parser_diagnostic_code::CLIPBOARD_READ_DENIED,
                QStringLiteral("OSC 52 read"),
                static_cast<std::size_t>(payload.size())));
            return;
        }

        const QByteArray::FromBase64Result decoded_payload = QByteArray::fromBase64Encoding(
            data_bytes,
            QByteArray::AbortOnBase64DecodingErrors);
        if (decoded_payload.decodingStatus != QByteArray::Base64DecodingStatus::Ok) {
            actions.push_back(make_clipboard_diagnostic(
                Parser_diagnostic_code::CLIPBOARD_WRITE_DENIED,
                QStringLiteral("OSC 52 write"),
                static_cast<std::size_t>(payload.size())));
            return;
        }

        actions.push_back(make_osc52_write_request_action(
            m_next_host_request_id++,
            QString::fromUtf8(target_selection_bytes),
            decoded_payload.decoded,
            static_cast<std::size_t>(payload.size()),
            QStringLiteral("OSC 52 write")));
        actions.push_back(make_clipboard_diagnostic(
            Parser_diagnostic_code::CLIPBOARD_WRITE_DENIED,
            QStringLiteral("OSC 52 write"),
            static_cast<std::size_t>(payload.size())));
        return;
    }

    actions.push_back(make_unsupported_sequence_diagnostic(
        QStringLiteral("OSC"),
        Parser_sequence_family::OSC,
        static_cast<std::size_t>(payload.size()),
        k_osc_payload_limit_bytes,
        Parser_recovery_strategy::DISCARD_STRING));
}

bool Terminal_byte_stream_parser::should_buffer_incomplete_utf8(
    QByteArrayView bytes,
    qsizetype      offset) const
{
    const unsigned char first     = byte_at(bytes, offset);
    const qsizetype     remaining = bytes.size() - offset;

    if (first >= 0xc2U && first <= 0xdfU) {
        return remaining < 2;
    }

    if (first >= 0xe0U && first <= 0xefU) {
        if (remaining == 1) {
            return true;
        }
        const unsigned char second = byte_at(bytes, offset + 1);
        if (!is_utf8_continuation_byte(second) ||
            (first == 0xe0U && second < 0xa0U) ||
            (first == 0xedU && second >= 0xa0U))
        {
            return false;
        }
        return remaining < 3;
    }

    if (first >= 0xf0U && first <= 0xf4U) {
        if (remaining == 1) {
            return true;
        }
        const unsigned char second = byte_at(bytes, offset + 1);
        if (!is_utf8_continuation_byte(second) ||
            (first == 0xf0U && second < 0x90U) ||
            (first == 0xf4U && second > 0x8fU))
        {
            return false;
        }
        if (remaining == 2)                                         { return true;  }
        if (!is_utf8_continuation_byte(byte_at(bytes, offset + 2))) { return false; }
        return remaining < 4;
    }

    return false;
}

void Terminal_byte_stream_parser::emit_unsupported_control(
    QString                        source_sequence,
    Parser_sequence_family         family,
    std::vector<Parser_action>&    actions)
{
    actions.push_back(make_unsupported_sequence_diagnostic(
        std::move(source_sequence),
        family,
        0U,
        0U,
        Parser_recovery_strategy::DISCARD_SEQUENCE));
}

void Terminal_byte_stream_parser::continue_discarded_csi(
    QByteArrayView bytes,
    qsizetype&     offset)
{
    for (; offset < bytes.size(); ++offset) {
        const unsigned char byte = byte_at(bytes, offset);
        if (byte == 0x1bU || byte == 0x9bU) {
            m_discarding_csi = false;
            return;
        }

        if (is_csi_final_byte(byte)) {
            ++offset;
            m_discarding_csi = false;
            return;
        }
    }
}

}
