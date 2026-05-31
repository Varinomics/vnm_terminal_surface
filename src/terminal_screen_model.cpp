#include "vnm_terminal/internal/terminal_screen_model.h"

#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/terminal_history_row_record_codec.h"
#include "vnm_terminal/internal/terminal_history_row_traversal.h"
#include "vnm_terminal/internal/terminal_repaint_recovery.h"
#include "vnm_terminal/internal/unicode_width.h"

#include <QChar>
#include <QStringList>
#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace vnm_terminal::internal {

namespace {

constexpr char32_t k_replacement_codepoint = 0xfffdU;
constexpr ushort   k_printable_ascii_first = 0x20U;
constexpr ushort   k_printable_ascii_last  = 0x7eU;
constexpr std::size_t k_printable_ascii_count =
    k_printable_ascii_last - k_printable_ascii_first + 1U;
constexpr int k_resize_repaint_clear_guard_action_budget = 64;
constexpr std::size_t k_retained_history_ring_capacity_bytes = 64U * 1024U * 1024U;

template <typename T>
constexpr bool k_unhandled_screen_mutation = false;

constexpr std::array<quint32, 16> k_ansi_16_palette_rgba = {
    0xff000000U, 0xffcd0000U, 0xff00cd00U, 0xffcdcd00U,
    0xff0000eeU, 0xffcd00cdU, 0xff00cdcdU, 0xffe5e5e5U,
    0xff7f7f7fU, 0xffff0000U, 0xff00ff00U, 0xffffff00U,
    0xff5c5cffU, 0xffff00ffU, 0xff00ffffU, 0xffffffffU,
};

constexpr std::array<int, 6> k_256_cube_components = {
    0, 95, 135, 175, 215, 255,
};

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

std::unique_ptr<Terminal_history_ring> make_retained_history_ring()
{
    auto ring = std::make_unique<Terminal_history_ring>(
        terminal_history_ring_config_t{
            k_retained_history_ring_capacity_bytes,
            0U,
        });
    if (!ring->ok()) {
        throw std::runtime_error("terminal retained history ring initialization failed");
    }

    return ring;
}

std::unique_ptr<Terminal_history_row_traversal> make_retained_history_traversal(
    Terminal_history_ring& ring)
{
    return std::make_unique<Terminal_history_row_traversal>(ring);
}

[[noreturn]] void throw_retained_history_storage_failure()
{
    throw std::runtime_error("terminal retained history ring storage operation failed");
}

const QString& printable_ascii_cell_text(QChar character)
{
    static const std::array<QString, k_printable_ascii_count> strings = [] {
        std::array<QString, k_printable_ascii_count> result;
        for (std::size_t index = 0; index < result.size(); ++index) {
            result[index] = QString(QChar(
                static_cast<ushort>(k_printable_ascii_first + index)));
        }
        return result;
    }();

    return strings[static_cast<std::size_t>(
        character.unicode() - k_printable_ascii_first)];
}

struct Sgr_parameter_atom
{
    bool                   has_value          = false;
    int                    value              = 0;
};

struct Sgr_parameter_group
{
    std::vector<Sgr_parameter_atom> atoms;
};

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

QString scalar_to_string(char32_t codepoint)
{
    if (codepoint <= 0xffffU) {
        return QString(QChar(static_cast<char16_t>(codepoint)));
    }

    const char32_t text[] = {codepoint};
    return QString::fromUcs4(text, 1);
}

quint32 rgba_from_components(int red, int green, int blue)
{
    return
         0xff000000U                        |
        (static_cast<quint32>(red) << 16U)  |
        (static_cast<quint32>(green) << 8U) |
         static_cast<quint32>(blue);
}

quint32 terminal_palette_rgba(int index)
{
    if (index >= 0 && index < static_cast<int>(k_ansi_16_palette_rgba.size())) {
        return k_ansi_16_palette_rgba[static_cast<std::size_t>(index)];
    }

    if (index >= 16 && index <= 231) {
        const int cube_index = index - 16;
        const int red        = k_256_cube_components[static_cast<std::size_t>(cube_index / 36)];
        const int green      = k_256_cube_components[static_cast<std::size_t>((cube_index / 6) % 6)];
        const int blue       = k_256_cube_components[static_cast<std::size_t>(cube_index % 6)];
        return rgba_from_components(red, green, blue);
    }

    if (index >= 232 && index <= 255) {
        const int component = 8 + ((index - 232) * 10);
        return rgba_from_components(component, component, component);
    }

    return k_terminal_default_foreground_rgba;
}

Terminal_color_state make_default_terminal_color_state()
{
    Terminal_color_state state;
    for (std::size_t i = 0; i < state.palette_rgba.size(); ++i) {
        state.palette_rgba[i] = terminal_palette_rgba(static_cast<int>(i));
    }
    return state;
}

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

QByteArray uri_from_hyperlink_identity_key(const QByteArray& identity_key)
{
    constexpr qsizetype uri_prefix_size = 4;
    constexpr qsizetype id_prefix_size  = 3;

    if (identity_key.startsWith(QByteArrayLiteral("uri:"))) {
        return identity_key.sliced(uri_prefix_size);
    }

    if (identity_key.startsWith(QByteArrayLiteral("id:"))) {
        const qsizetype separator = identity_key.indexOf('\x1f', id_prefix_size);
        if (separator >= 0 && separator + 1 < identity_key.size()) {
            return identity_key.sliced(separator + 1);
        }
    }

    return {};
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

bool palette_index_is_valid(int value)
{
    return value >= 0 && value <= 255;
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

bool parse_simple_csi_parameter_groups(
    QByteArrayView                             parameter_bytes,
    std::vector<Sgr_parameter_group>&          groups)
{
    if (!parse_sgr_parameter_groups(parameter_bytes, groups)) {
        return false;
    }

    for (const Sgr_parameter_group& group : groups) {
        if (group.atoms.size() != 1U) {
            return false;
        }
    }

    return true;
}

bool csi_parameter_value(
    const std::vector<Sgr_parameter_group>&    groups,
    std::size_t                                index,
    int                                        default_value,
    int&                                       value)
{
    if (index >= groups.size()) {
        value = default_value;
        return true;
    }

    const Sgr_parameter_group& group = groups[index];
    if (group.atoms.size() != 1U) {
        return false;
    }

    const Sgr_parameter_atom& atom = group.atoms.front();
    value = atom.has_value ? atom.value : default_value;
    return true;
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

Parser_action make_unsupported_sequence_diagnostic(
    QString                    source_sequence,
    Parser_sequence_family     family,
    std::size_t                raw_payload_size,
    std::size_t                limit_bytes,
    Parser_recovery_strategy   recovery)
{
    return {
        Parser_payload_diagnostic{
            Parser_diagnostic_code::UNSUPPORTED_SEQUENCE,
            std::move(source_sequence),
            raw_payload_size,
            limit_bytes,
            family,
            recovery,
        },
    };
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

    const auto flush_print_text = [&]() {
        if (print_text.isEmpty()) {
            return;
        }

        actions.push_back(make_print_text_action(std::move(print_text), 0, 0));
        print_text.clear();
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
        if (byte == 0x1bU || byte == 0x9bU || c1_string_family(byte) != Parser_sequence_family::NONE) {
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

            print_text += scalar_to_string(k_replacement_codepoint);
            flush_print_text();
            actions.push_back(make_malformed_recovery_diagnostic(
                QStringLiteral("invalid UTF-8"),
                Parser_sequence_family::PRINTABLE,
                Parser_recovery_strategy::IGNORE_BYTE));
            ++offset;
            continue;
        }

        if (!is_control_byte(byte)) {
            print_text += scalar_to_string(step.codepoint);
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

Terminal_screen_model_config_status validate_terminal_screen_model_config(
    const Terminal_screen_model_config& config)
{
    if (!is_terminal_screen_model_grid_size_supported(config.grid_size)) {
        return Terminal_screen_model_config_status::INVALID_GRID_SIZE;
    }

    if (config.scrollback_limit < 0) {
        return Terminal_screen_model_config_status::INVALID_SCROLLBACK_LIMIT;
    }

    if (config.tab_width <= 0) {
        return Terminal_screen_model_config_status::INVALID_TAB_WIDTH;
    }

    return Terminal_screen_model_config_status::OK;
}

Terminal_screen_model::Terminal_screen_model(Terminal_screen_model_config config)
:
    m_config(config),
    m_color_state(make_default_terminal_color_state()),
    m_current_style(make_default_terminal_text_style()),
    m_styles{make_default_terminal_text_style()}
{
    if (validate_terminal_screen_model_config(m_config) !=
        Terminal_screen_model_config_status::OK)
    {
        throw std::invalid_argument("invalid Terminal_screen_model_config");
    }

    reset_grid();
}

Terminal_screen_model_result Terminal_screen_model::ingest(QByteArrayView bytes)
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_screen_model::ingest");

    Terminal_screen_model_result result;
    m_scrollback_evicted_rows = 0;
    clear_backing_deltas();
    clear_recovery_proposals();
    clear_dirty();
    std::vector<Parser_action> parser_actions;
    {
        VNM_TERMINAL_PROFILE_SCOPE("Terminal_screen_model::parser_ingest");
        parser_actions = m_parser.ingest(bytes);
    }

    ingest_publication_t publication;

    {
        VNM_TERMINAL_PROFILE_SCOPE("Terminal_screen_model::apply_parser_actions");
        if (m_config.retain_structural_actions) {
            result.actions.reserve(parser_actions.size());
        }

        for (const Parser_action& action : parser_actions) {
            if (m_config.retain_structural_actions || action_is_session_visible(action)) {
                result.actions.push_back(action);
            }
            clear_dirty();
            apply_action(action, result.actions, &publication);
            advance_resize_repaint_clear_guard();
            advance_primary_repaint_recovery_resize_guard();

            if (m_modes.synchronized_output) {
                collect_synchronized_changes();
            }
            else {
                publish_pending_changes(publication);
            }
            retain_referenced_active_hyperlink_ids();
        }
    }

    if (m_primary_repaint_recovery_candidate.active && m_modes.cursor_visible) {
        clear_dirty();
        finish_primary_repaint_recovery_candidate(false);
        if (m_modes.synchronized_output) {
            collect_synchronized_changes();
        }
        else {
            publish_pending_changes(publication);
        }
    }

    m_dirty_rows                    = std::move(publication.dirty_rows);
    m_terminal_content_changed      = publication.terminal_content_changed;
    m_active_buffer_changed         = publication.active_buffer_changed;
    m_grid_reflow_changed           = publication.grid_reflow_changed;
    m_viewport_changed              = publication.viewport_changed;
    m_mode_state_changed            = publication.mode_state_changed;
    m_mouse_reporting_mode_changed  = publication.mouse_reporting_mode_changed;
    m_alternate_scroll_mode_changed = publication.alternate_scroll_mode_changed;
    retain_referenced_active_hyperlink_ids();

    {
        VNM_TERMINAL_PROFILE_SCOPE("Terminal_screen_model::finalize_ingest_result");
        result.dirty_rows = dirty_rows();
        result.dirty_rows_have_stable_mutation_identity =
            publication.dirty_rows_have_stable_mutation_identity;
        result.terminal_content_changed      = m_terminal_content_changed;
        result.active_buffer_changed         = m_active_buffer_changed;
        result.grid_reflow_changed           = m_grid_reflow_changed;
        result.viewport_changed              = m_viewport_changed;
        result.mode_state_changed            = m_mode_state_changed;
        result.mouse_reporting_mode_changed  = m_mouse_reporting_mode_changed;
        result.alternate_scroll_mode_changed = m_alternate_scroll_mode_changed;
        result.scrollback_rows               = scrollback_size();
        result.backing_deltas                = m_backing_deltas;
        result.recovery_proposals            = m_recovery_proposals;
        result.evicted_scrollback_rows       = compatibility_evicted_scrollback_rows();
    }
    return result;
}

Terminal_screen_model_result Terminal_screen_model::force_release_synchronized_output()
{
    Terminal_screen_model_result result;
    m_scrollback_evicted_rows = 0;
    clear_backing_deltas();
    clear_recovery_proposals();
    clear_dirty();

    ingest_publication_t publication;
    set_synchronized_output_mode(false, &publication);

    m_dirty_rows                    = std::move(publication.dirty_rows);
    m_terminal_content_changed      = publication.terminal_content_changed;
    m_active_buffer_changed         = publication.active_buffer_changed;
    m_grid_reflow_changed           = publication.grid_reflow_changed;
    m_viewport_changed              = publication.viewport_changed;
    m_mode_state_changed            = publication.mode_state_changed;
    m_mouse_reporting_mode_changed  = publication.mouse_reporting_mode_changed;
    m_alternate_scroll_mode_changed = publication.alternate_scroll_mode_changed;
    retain_referenced_active_hyperlink_ids();

    result.dirty_rows = dirty_rows();
    result.dirty_rows_have_stable_mutation_identity =
        publication.dirty_rows_have_stable_mutation_identity;
    result.terminal_content_changed      = m_terminal_content_changed;
    result.active_buffer_changed         = m_active_buffer_changed;
    result.grid_reflow_changed           = m_grid_reflow_changed;
    result.viewport_changed              = m_viewport_changed;
    result.mode_state_changed            = m_mode_state_changed;
    result.mouse_reporting_mode_changed  = m_mouse_reporting_mode_changed;
    result.alternate_scroll_mode_changed = m_alternate_scroll_mode_changed;
    result.scrollback_rows               = scrollback_size();
    result.backing_deltas                = m_backing_deltas;
    result.recovery_proposals            = m_recovery_proposals;
    result.evicted_scrollback_rows       = compatibility_evicted_scrollback_rows();
    return result;
}

void Terminal_screen_model::apply_action(const Parser_action& action)
{
    std::vector<Parser_action> generated_actions;
    apply_action(action, generated_actions, nullptr);
    advance_resize_repaint_clear_guard();
    advance_primary_repaint_recovery_resize_guard();
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
        [this](const auto& mutation) {
            using mutation_t = std::decay_t<decltype(mutation)>;
            if constexpr (std::is_same_v<mutation_t, Screen_print_text_mutation>) {
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_screen_model::apply_action::print_text");
                put_text(mutation.text);
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

    std::vector<Sgr_parameter_group> groups;
    const auto parse_simple_parameters = [&]() {
        groups.clear();
        if (!parse_simple_csi_parameter_groups(sequence.payload, groups)) {
            malformed();
            return false;
        }
        return true;
    };

    const auto single_parameter = [&](int default_value, int& value) {
        if (!parse_simple_parameters()) {
            return false;
        }
        if (groups.size() > 1U) {
            malformed();
            return false;
        }
        return csi_parameter_value(groups, 0U, default_value, value);
    };

    const bool has_no_prefix =
        sequence.private_marker.isEmpty() && sequence.intermediates.isEmpty();
    const unsigned char final_byte = static_cast<unsigned char>(sequence.final_bytes.front());

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
                            if (!parse_simple_parameters()) {
                                return;
                            }
                            if (groups.size() > 1U) {
                                malformed();
                                return;
                            }
                            if (!csi_parameter_value(groups, 0U, 1, count)) {
                                malformed();
                                return;
                            }
                            if (count < 1) {
                                count = 1;
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
                            if (groups.size() > 2U) {
                                malformed();
                                return;
                            }

                            int row    = 1;
                            int column = 1;
                            if (!csi_parameter_value(groups, 0U, 1, row) ||
                                !csi_parameter_value(groups, 1U, 1, column))
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
                            if (!single_parameter(1, count)) { return;    }
                            if (count < 1)                   { count = 1; }

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
                            int count = 1;
                            if (!has_no_prefix) {
                                malformed();
                                return;
                            }
                            if (!parse_simple_parameters()) {
                                return;
                            }
                            if (groups.size() > 1U) {
                                if (final_byte == 'T') {
                                    unsupported();
                                }
                                else {
                                    malformed();
                                }
                                return;
                            }
                            if (!csi_parameter_value(groups, 0U, 1, count)) {
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
                            if (groups.size() > 2U) {
                                malformed();
                                return;
                            }

                            int top    = 1;
                            int bottom = m_config.grid_size.rows;
                            if (!csi_parameter_value(groups, 0U, 1, top) ||
                                !csi_parameter_value(groups, 1U, m_config.grid_size.rows, bottom))
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
                            if (groups.size() > 1U || !csi_parameter_value(groups, 0U, 0, mode)) {
                                malformed();
                                return;
                            }

                            if (sequence.intermediates.isEmpty() &&
                                sequence.private_marker.isEmpty() &&
                                mode == 0)
                            {
                                generated_actions.push_back(make_da1_reply_action(QByteArrayLiteral("\x1b[?1;2c")));
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
                            int mode = 0;
                            if (!has_no_prefix) {
                                unsupported();
                                return;
                            }
                            if (!parse_simple_parameters()) {
                                return;
                            }
                            if (!csi_parameter_value(groups, 0U, 0, mode)) {
                                malformed();
                                return;
                            }
                            if (mode == 8 && groups.size() == 3U) {
                                int rows    = 0;
                                int columns = 0;
                                if (!csi_parameter_value(groups, 1U, 0, rows) ||
                                    !csi_parameter_value(groups, 2U, 0, columns))
                                {
                                    malformed();
                                    return;
                                }
                                if (rows <= 0 || columns <= 0) {
                                    unsupported();
                                    return;
                                }
                                const terminal_grid_size_t grid_size{rows, columns};
                                if (!is_terminal_screen_model_grid_size_supported(grid_size)) {
                                    unsupported();
                                    return;
                                }
                                apply_grid_resize(grid_size, false);
                                generated_actions.push_back(
                                    make_text_area_resize_notification_action(rows, columns));
                                return;
                            }
                            // Other multi-parameter lowercase t forms are xterm window-operation
                            // subcommands; reject them as unsupported, not as malformed CSI syntax.
                            if (groups.size() != 1U) {
                                unsupported();
                                return;
                            }
                            if (mode == 18) {
                                generated_actions.push_back(make_text_area_size_reply_action(
                                    m_config.grid_size.rows,
                                    m_config.grid_size.columns));
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

                            for (std::size_t i = 0; i < groups.size(); ++i) {
                                int mode = 0;
                                if (!csi_parameter_value(groups, i, 0, mode)) {
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

                            if (!groups.empty()) {
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

void Terminal_screen_model::apply_sgr_sequence(const Terminal_sgr_sequence& sequence)
{
    for (const Terminal_sgr_operation& operation : sequence.operations) {
        apply_sgr_operation(operation);
    }
}

void Terminal_screen_model::apply_sgr_operation(const Terminal_sgr_operation& operation)
{
    switch (operation.kind) {
        case Terminal_sgr_operation_kind::RESET_ALL:
            m_current_style = make_default_terminal_text_style();
            break;
        case Terminal_sgr_operation_kind::SET_ATTRIBUTES:
            m_current_style.attributes |= operation.attributes;
            break;
        case Terminal_sgr_operation_kind::CLEAR_ATTRIBUTES:
            m_current_style.attributes &= static_cast<std::uint16_t>(~operation.attributes);
            break;
        case Terminal_sgr_operation_kind::SET_FOREGROUND:
            m_current_style.foreground = operation.color;
            break;
        case Terminal_sgr_operation_kind::SET_BACKGROUND:
            m_current_style.background = operation.color;
            break;
    }

    m_current_style_id = intern_style(m_current_style);
}

Terminal_style_id Terminal_screen_model::intern_style(const Terminal_text_style& style)
{
    for (std::size_t i = 0; i < m_styles.size(); ++i) {
        if (m_styles[i] == style) {
            return static_cast<Terminal_style_id>(i);
        }
    }

    m_styles.push_back(style);
    return static_cast<Terminal_style_id>(m_styles.size() - 1U);
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

Terminal_render_snapshot Terminal_screen_model::render_snapshot(std::uint64_t sequence) const
{
    Terminal_viewport_state viewport;
    viewport.active_buffer   = m_active_buffer_id;
    viewport.visible_rows    = m_config.grid_size.rows;
    viewport.scrollback_rows = m_active_buffer_id == Terminal_buffer_id::PRIMARY
        ? m_primary_backing.retained_history_size()
        : 0;

    Terminal_render_snapshot_request request;
    request.sequence                     = sequence;
    request.viewport                     = viewport;
    request.dirty_rows                   = dirty_rows();
    request.viewport_changed             = m_viewport_changed;
    request.mouse_reporting_mode_changed = m_mouse_reporting_mode_changed;
    return render_snapshot(request);
}

Terminal_render_snapshot Terminal_screen_model::render_snapshot(
    const Terminal_render_snapshot_request& request) const
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_screen_model::render_snapshot");

#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled) {
        ++m_profile_stats.render_snapshot_requests;
        m_profile_stats.render_snapshot_dirty_rows_requested +=
            static_cast<std::uint64_t>(request.dirty_rows.size());
        if (request.viewport_changed) {
            ++m_profile_stats.render_snapshot_full_repaint_fallbacks;
            ++m_profile_stats.render_snapshot_viewport_fallbacks;
        }
    }
#endif

    Terminal_viewport_state viewport = request.viewport;
    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_screen_model::render_snapshot::viewport");

        viewport.active_buffer = m_active_buffer_id;
        viewport.visible_rows  = m_config.grid_size.rows;
        if (viewport.active_buffer == Terminal_buffer_id::ALTERNATE) {
            viewport.scrollback_rows  = 0;
            viewport.offset_from_tail = 0;
            viewport.follow_tail      = true;
        }
        else {
            viewport.scrollback_rows = scrollback_size();
            viewport.offset_from_tail =
                std::clamp(viewport.offset_from_tail, 0, viewport.scrollback_rows);
            viewport.follow_tail = viewport.offset_from_tail == 0;
        }
    }

    Terminal_render_snapshot snapshot =
        make_empty_render_snapshot(m_config.grid_size, viewport, request.sequence);
    snapshot.basis                     = request.basis;
    snapshot.purpose                   = request.purpose;
    snapshot.public_scroll_diagnostics = request.public_scroll_diagnostics;
    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_screen_model::render_snapshot::metadata");

        snapshot.cells.reserve(
            static_cast<std::size_t>(m_config.grid_size.rows) *
            static_cast<std::size_t>(m_config.grid_size.columns));
        snapshot.color_state                       = m_color_state;
        snapshot.styles                            = m_styles;
        snapshot.cursor.position                   = m_cursor;
        snapshot.cursor.shape                      = request.cursor_shape;
        snapshot.cursor.visible                    = m_modes.cursor_visible;
        snapshot.cursor.blink_enabled              = request.cursor_blink_enabled;
        snapshot.ime_preedit                       = request.ime_preedit;
        snapshot.metadata.backend_geometry_in_sync = request.backend_geometry_in_sync;
        snapshot.metadata.row_origin_generation    = request.row_origin_generation;
        snapshot.metadata.visual_bell_active       = request.visual_bell_active;
        snapshot.metadata.mouse_reporting_mode_changed =
            request.mouse_reporting_mode_changed;
        snapshot.modes = m_modes;
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_screen_model::render_snapshot::dirty_rows");

        snapshot.dirty_row_ranges = compact_dirty_row_ranges(
            viewport_dirty_rows(viewport, request.dirty_rows),
            m_config.grid_size.rows,
            request.viewport_changed);
#if VNM_TERMINAL_PROFILING_ENABLED
        if (m_profile_stats.enabled) {
            std::uint64_t visible_dirty_rows = 0U;
            for (const Terminal_render_dirty_row_range& range : snapshot.dirty_row_ranges) {
                visible_dirty_rows += static_cast<std::uint64_t>(range.row_count);
            }
            m_profile_stats.render_snapshot_dirty_rows_visible += visible_dirty_rows;
            if (visible_dirty_rows == 0U) {
                ++m_profile_stats.render_snapshot_zero_dirty_publications;
            }
        }
#endif
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_screen_model::render_snapshot::append_rows");

#if VNM_TERMINAL_PROFILING_ENABLED
        const std::size_t snapshot_cells_before_append = snapshot.cells.size();
        std::uint64_t rows_visited                     = 0U;
        std::uint64_t rows_materialized                = 0U;
#endif
        {
            VNM_TERMINAL_PROFILE_SCOPE(
                "Terminal_screen_model::render_snapshot::append_rows::reserve");

            snapshot.visible_line_provenance.reserve(
                static_cast<std::size_t>(m_config.grid_size.rows));
        }
        for (int row = 0; row < m_config.grid_size.rows; ++row) {
#if VNM_TERMINAL_PROFILING_ENABLED
            ++rows_visited;
#endif
            const viewport_row_t viewport_row{row};
            const std::size_t first_row_cell = snapshot.cells.size();
            const snapshot_row_t snapshot_row = [&]() {
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_screen_model::render_snapshot::append_rows::snapshot_row");

                return snapshot_row_from_viewport(viewport_row);
            }();

            std::optional<std::vector<Cell>> row_cells;
            {
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_screen_model::render_snapshot::append_rows::viewport_row_cells");

                row_cells = viewport_row_cells(viewport, row);
            }
            if (row_cells.has_value()) {
#if VNM_TERMINAL_PROFILING_ENABLED
                ++rows_materialized;
#endif
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_screen_model::render_snapshot::append_rows::append_cells");

                append_snapshot_cells_from_row(snapshot, *row_cells, snapshot_row.value);
            }

            std::optional<std::map<std::uint64_t, QByteArray>>
                row_local_hyperlink_identity_keys;
            {
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_screen_model::render_snapshot::append_rows::hyperlink_metadata::lookup");

                row_local_hyperlink_identity_keys =
                    viewport_row_retained_hyperlink_identity_keys(viewport, row);
            }
            {
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_screen_model::render_snapshot::append_rows::hyperlink_metadata::append");

                append_hyperlink_metadata_for_cells(
                    snapshot.hyperlinks,
                    snapshot.cells,
                    first_row_cell,
                    row_local_hyperlink_identity_keys.has_value()
                        ? &*row_local_hyperlink_identity_keys
                        : nullptr);
            }

            std::optional<Terminal_retained_line_provenance> provenance;
            {
                VNM_TERMINAL_PROFILE_SCOPE(
                    "Terminal_screen_model::render_snapshot::append_rows::provenance::lookup");

                provenance = viewport_row_provenance(viewport, row);
            }
            if (provenance.has_value()) {
                std::optional<primary_backing_row_t> backing_row;
                {
                    VNM_TERMINAL_PROFILE_SCOPE(
                        "Terminal_screen_model::render_snapshot::append_rows::provenance::backing_row");

                    backing_row = primary_backing_row_from_viewport(viewport, viewport_row);
                }
                {
                    VNM_TERMINAL_PROFILE_SCOPE(
                        "Terminal_screen_model::render_snapshot::append_rows::provenance::append");

                    const int logical_row =
                        backing_row.has_value() ? backing_row->value : row;
                    snapshot.visible_line_provenance.push_back({
                        static_cast<std::int64_t>(logical_row),
                        provenance->retained_line_id,
                        provenance->content_generation,
                    });
                }
            }
        }
#if VNM_TERMINAL_PROFILING_ENABLED
        if (m_profile_stats.enabled) {
            const std::uint64_t cells_emitted = static_cast<std::uint64_t>(
                snapshot.cells.size() - snapshot_cells_before_append);
            ++m_profile_stats.render_snapshots_constructed;
            m_profile_stats.render_snapshot_rows_visited      += rows_visited;
            m_profile_stats.render_snapshot_rows_materialized += rows_materialized;
            m_profile_stats.render_snapshot_cells_scanned     +=
                rows_materialized *
                static_cast<std::uint64_t>(m_config.grid_size.columns);
            m_profile_stats.render_snapshot_cells_emitted     += cells_emitted;
            m_profile_stats.max_render_snapshot_rows_visited =
                std::max(
                    m_profile_stats.max_render_snapshot_rows_visited,
                    rows_visited);
            m_profile_stats.max_render_snapshot_cells_emitted =
                std::max(
                    m_profile_stats.max_render_snapshot_cells_emitted,
                    cells_emitted);
        }
#endif
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_screen_model::render_snapshot::primary_cursor");

        if (viewport.active_buffer == Terminal_buffer_id::PRIMARY) {
            const primary_backing_row_t cursor_backing_row =
                primary_backing_row_from_active(active_grid_row_t{m_cursor.row});
            const viewport_row_t cursor_viewport_row =
                viewport_row_from_primary_backing_unbounded(viewport, cursor_backing_row);
            snapshot.cursor.position.row = cursor_viewport_row.value;
            snapshot.cursor.visible =
                snapshot.cursor.visible                                &&
                snapshot.cursor.position.row >= 0                      &&
                snapshot.cursor.position.row < m_config.grid_size.rows;
        }
    }

    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_screen_model::render_snapshot::selections");

        if (render_snapshot_visible_line_provenance_is_valid(snapshot)) {
            for (const Terminal_render_selection_request& selection_request : request.selections) {
                std::vector<int> selected_logical_rows;
                if (!render_selection_request_logical_rows(
                        selection_request,
                        viewport.active_buffer,
                        selected_logical_rows))
                {
                    continue;
                }

                const Terminal_selection_range& selection = selection_request.range;
                const terminal_grid_position_t start = normalized_selection_start(selection);
                const terminal_grid_position_t end   = normalized_selection_end(selection);

                for (std::size_t index = 0U; index < selected_logical_rows.size(); ++index) {
                    const int logical_row = selected_logical_rows[index];
                    int row = logical_row;
                    if (viewport.active_buffer == Terminal_buffer_id::PRIMARY) {
                        const std::optional<viewport_row_t> converted_row =
                            viewport_row_from_primary_backing(
                                viewport,
                                primary_backing_row_t{logical_row});
                        if (!converted_row.has_value()) {
                            continue;
                        }

                        row = converted_row->value;
                    }
                    if (row < 0 || row >= m_config.grid_size.rows) {
                        continue;
                    }

                    const int first_column = index == 0U ? start.column : 0;
                    const int end_column   =
                        index + 1U == selected_logical_rows.size()
                            ? end.column
                            : m_config.grid_size.columns;
                    if (first_column >= 0                          &&
                        first_column <= m_config.grid_size.columns &&
                        end_column   >= 0                          &&
                        end_column   <= m_config.grid_size.columns &&
                        end_column   >  first_column)
                    {
                        snapshot.selection_spans.push_back({
                            selection,
                            row,
                            first_column,
                            end_column - first_column,
                        });
                    }
                }
            }
        }
    }

    return snapshot;
}

std::optional<Terminal_retained_line_provenance>
Terminal_screen_model::viewport_row_provenance(
    const Terminal_viewport_state& viewport,
    int                            viewport_row) const
{
    const viewport_row_t row{viewport_row};
    if (!viewport_row_is_valid(row)) {
        return std::nullopt;
    }

    if (viewport.active_buffer == Terminal_buffer_id::ALTERNATE) {
        const Terminal_screen_row* active_row =
            alternate_active_row(active_grid_row_t{row.value});
        return active_row != nullptr
            ? std::optional<Terminal_retained_line_provenance>(
                active_row->retained_line_provenance)
            : std::nullopt;
    }

    const std::optional<primary_backing_row_t> backing_row =
        primary_backing_row_from_viewport(viewport, row);
    if (!backing_row.has_value()) {
        return std::nullopt;
    }

    const std::optional<Terminal_screen_row> backing = primary_backing_row(*backing_row);
    return backing.has_value()
        ? std::optional<Terminal_retained_line_provenance>(
            backing->retained_line_provenance)
        : std::nullopt;
}

std::optional<std::map<std::uint64_t, QByteArray>>
Terminal_screen_model::viewport_row_retained_hyperlink_identity_keys(
    const Terminal_viewport_state& viewport,
    int                            viewport_row) const
{
    const viewport_row_t row{viewport_row};
    if (viewport.active_buffer != Terminal_buffer_id::PRIMARY ||
        !viewport_row_is_valid(row))
    {
        return std::nullopt;
    }

    const std::optional<primary_backing_row_t> backing_row =
        primary_backing_row_from_viewport(viewport, row);
    if (!backing_row.has_value() || backing_row->value >= scrollback_size()) {
        return std::nullopt;
    }

    const std::optional<retained_row_record_t> retained_record =
        m_primary_backing.materialize_retained_history_record(
            static_cast<std::size_t>(backing_row->value));
    if (!retained_record.has_value()) {
        return std::nullopt;
    }

    return retained_record->hyperlink_identity_keys;
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

    if (!terminal_history_handle_has_identity(history_handle)) {
        return {};
    }

    Terminal_retained_line_lookup_result result;
    if (history_handle.epoch != k_terminal_history_retained_identity_epoch) {
        result.resolution_status = Terminal_history_resolution_status::STALE_EPOCH;
        return result;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        result = {};
        result.resolution_status = Terminal_history_resolution_status::STALE_ROW_SEQUENCE;

        const retained_lookup_cache_t& cache = retained_lookup_cache(buffer_id);
        bool invalid_cache_hit = false;

        const auto remember_neighbor =
            [&](const retained_lookup_cache_entry_t& entry, bool successor) {
                const Terminal_history_resolution_status entry_status =
                    retained_lookup_cache_entry_status(buffer_id, entry);
                if (entry_status != Terminal_history_resolution_status::OK) {
                    invalid_cache_hit = true;
                    return;
                }

                if (successor) {
                    result.nearest_successor = true;
                    result.nearest_successor_logical_row = entry.logical_row;
                }
                else {
                    result.nearest_predecessor = true;
                    result.nearest_predecessor_logical_row = entry.logical_row;
                }
            };

        auto equal_or_successor =
            cache.by_row_sequence.lower_bound(history_handle.row_sequence);
        if (equal_or_successor != cache.by_row_sequence.end() &&
            equal_or_successor->first == history_handle.row_sequence)
        {
            const retained_lookup_cache_entry_t& entry = equal_or_successor->second;
            const Terminal_history_resolution_status entry_status =
                retained_lookup_cache_entry_status(buffer_id, entry);
            if (entry_status != Terminal_history_resolution_status::OK) {
                invalid_cache_hit = true;
            }
            else {
                result.retained_line_id_found = true;
                result.retained_line_id_match_count = entry.row_sequence_match_count;
                result.resolution_status =
                    retained_history_handle_match_status(entry.history_handle, history_handle);
                if (result.resolution_status == Terminal_history_resolution_status::OK) {
                    result.exact_match       = true;
                    result.exact_logical_row = entry.logical_row;
                }
                else
                if (result.resolution_status ==
                    Terminal_history_resolution_status::CONTENT_GENERATION_MISMATCH)
                {
                    result.retained_line_content_generation_mismatch = true;
                }
            }

            ++equal_or_successor;
        }

        if (equal_or_successor != cache.by_row_sequence.end()) {
            remember_neighbor(equal_or_successor->second, true);
        }

        auto predecessor = cache.by_row_sequence.lower_bound(history_handle.row_sequence);
        if (predecessor != cache.by_row_sequence.begin()) {
            --predecessor;
            remember_neighbor(predecessor->second, false);
        }

        if (!invalid_cache_hit) {
            return result;
        }

        invalidate_retained_lookup_caches();
    }

    return result;
}

std::optional<terminal_history_handle_t>
Terminal_screen_model::retained_history_handle_at_logical_row(
    Terminal_buffer_id buffer_id,
    int                logical_row) const
{
    return retained_lookup_cache_handle_at_logical_row(buffer_id, logical_row);
}

void Terminal_screen_model::discard_retained_lookup_cache_for_testing() const
{
    invalidate_retained_lookup_caches();
}

void Terminal_screen_model::invalidate_retained_lookup_caches() const
{
    m_primary_retained_lookup_cache.valid = false;
    m_primary_retained_lookup_cache.by_row_sequence.clear();
    m_primary_retained_lookup_cache.by_logical_row.clear();
    m_alternate_retained_lookup_cache.valid = false;
    m_alternate_retained_lookup_cache.by_row_sequence.clear();
    m_alternate_retained_lookup_cache.by_logical_row.clear();
}

void Terminal_screen_model::rebuild_retained_lookup_cache(
    Terminal_buffer_id buffer_id) const
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_screen_model::rebuild_retained_lookup_cache");

    retained_lookup_cache_t& cache = mutable_retained_lookup_cache(buffer_id);
    cache.by_row_sequence.clear();

    const int row_count = buffer_id == Terminal_buffer_id::PRIMARY
        ? primary_backing_row_count()
        : active_grid_row_count();
    cache.by_logical_row.assign(static_cast<std::size_t>(row_count), {});

    for (int logical_row = 0; logical_row < row_count; ++logical_row) {
        const std::optional<terminal_history_handle_t> handle =
            retained_lookup_cache_live_handle(buffer_id, logical_row);
        if (!handle.has_value() || !terminal_history_handle_has_identity(*handle)) {
            continue;
        }

        cache.by_logical_row[static_cast<std::size_t>(logical_row)] = *handle;
        auto insertion = cache.by_row_sequence.emplace(
            handle->row_sequence,
            retained_lookup_cache_entry_t{
                logical_row,
                *handle,
                1,
            });
        if (!insertion.second) {
            ++insertion.first->second.row_sequence_match_count;
        }
    }

    cache.valid = true;
}

const Terminal_screen_model::retained_lookup_cache_t&
Terminal_screen_model::retained_lookup_cache(Terminal_buffer_id buffer_id) const
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_screen_model::retained_lookup_cache");

    retained_lookup_cache_t& cache = mutable_retained_lookup_cache(buffer_id);
    if (!cache.valid) {
        rebuild_retained_lookup_cache(buffer_id);
    }
    return cache;
}

Terminal_screen_model::retained_lookup_cache_t&
Terminal_screen_model::mutable_retained_lookup_cache(Terminal_buffer_id buffer_id) const
{
    return buffer_id == Terminal_buffer_id::PRIMARY
        ? m_primary_retained_lookup_cache
        : m_alternate_retained_lookup_cache;
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
            return m_primary_backing.retained_history_handle(
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

Terminal_history_resolution_status
Terminal_screen_model::retained_lookup_cache_entry_status(
    Terminal_buffer_id                     buffer_id,
    const retained_lookup_cache_entry_t&   entry) const
{
    const std::optional<terminal_history_handle_t> live_handle =
        retained_lookup_cache_live_handle(buffer_id, entry.logical_row);
    if (!live_handle.has_value()) {
        return Terminal_history_resolution_status::STALE_ROW_SEQUENCE;
    }

    if (!terminal_history_handle_has_identity(*live_handle)) {
        return Terminal_history_resolution_status::STALE_ROW_SEQUENCE;
    }

    return retained_history_handle_match_status(*live_handle, entry.history_handle);
}

std::optional<terminal_history_handle_t>
Terminal_screen_model::retained_lookup_cache_handle_at_logical_row(
    Terminal_buffer_id buffer_id,
    int                logical_row) const
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_screen_model::retained_lookup_cache_handle_at_logical_row");

    if (logical_row < 0) {
        return std::nullopt;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        const retained_lookup_cache_t& cache = retained_lookup_cache(buffer_id);
        if (logical_row >= static_cast<int>(cache.by_logical_row.size())) {
            return std::nullopt;
        }

        const terminal_history_handle_t cached_handle =
            cache.by_logical_row[static_cast<std::size_t>(logical_row)];
        if (!terminal_history_handle_has_identity(cached_handle)) {
            return std::nullopt;
        }

        const std::optional<terminal_history_handle_t> live_handle =
            retained_lookup_cache_live_handle(buffer_id, logical_row);
        if (!live_handle.has_value()) {
            invalidate_retained_lookup_caches();
            continue;
        }

        if (retained_history_handle_match_status(*live_handle, cached_handle) ==
            Terminal_history_resolution_status::OK)
        {
            return cached_handle;
        }

        invalidate_retained_lookup_caches();
    }

    return std::nullopt;
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
            static_cast<std::size_t>(logical_row));
    return retained_record.has_value()
        ? std::optional<terminal_retained_row_record_metadata_t>(retained_record->metadata)
        : std::nullopt;
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

Terminal_screen_model::Retained_history_storage::Retained_history_storage()
:
    ring(make_retained_history_ring()),
    traversal(make_retained_history_traversal(*ring))
{}

Terminal_screen_model::Retained_history_storage::~Retained_history_storage() = default;

Terminal_screen_model::Retained_history_storage::Retained_history_storage(
    const Retained_history_storage& other)
:
    Retained_history_storage()
{
    for (terminal_history_handle_t source_handle : other.logical_rows) {
        const Terminal_history_ring_read_scope read =
            other.ring->read_record(source_handle.byte_sequence);
        if (!read.ok()) {
            throw_retained_history_storage_failure();
        }

        Terminal_history_row_record_decode_result decoded =
            decode_terminal_history_row_record(read, source_handle);
        if (decoded.status != Terminal_history_row_record_codec_status::OK) {
            throw_retained_history_storage_failure();
        }

        terminal_history_row_record_identity_t identity;
        identity.epoch        = decoded.history_handle.epoch;
        identity.row_sequence = decoded.history_handle.row_sequence;
        if (terminal_history_handle_has_identity(latest_history_handle)) {
            identity.previous_row_byte_sequence = latest_history_handle.byte_sequence;
            identity.previous_row_sequence      = latest_history_handle.row_sequence;
        }

        Terminal_history_row_record_append_result append =
            encode_terminal_history_row_record_to_ring(
                *ring,
                decoded.record,
                identity);
        if (append.status != Terminal_history_row_record_codec_status::OK) {
            throw_retained_history_storage_failure();
        }

        latest_history_handle = append.history_handle;
        logical_rows.push_back(append.history_handle);
    }
}

Terminal_screen_model::Retained_history_storage&
Terminal_screen_model::Retained_history_storage::operator=(
    const Retained_history_storage& other)
{
    if (this != &other) {
        Retained_history_storage replacement(other);
        ring                  = std::move(replacement.ring);
        traversal             = std::move(replacement.traversal);
        logical_rows          = std::move(replacement.logical_rows);
        latest_history_handle = replacement.latest_history_handle;
    }

    return *this;
}

void Terminal_screen_model::Retained_history_storage::reset()
{
    ring = make_retained_history_ring();
    traversal = make_retained_history_traversal(*ring);
    logical_rows.clear();
    latest_history_handle = {};
}

Terminal_screen_model::Primary_backing_buffer::Primary_backing_buffer() = default;
Terminal_screen_model::Primary_backing_buffer::~Primary_backing_buffer() = default;

Terminal_screen_model::Primary_backing_buffer::Primary_backing_buffer(
    const Primary_backing_buffer& other)
:
    active_grid(other.active_grid),
    retained_history(other.retained_history)
{}

Terminal_screen_model::Primary_backing_buffer&
Terminal_screen_model::Primary_backing_buffer::operator=(
    const Primary_backing_buffer& other)
{
    if (this != &other) {
        active_grid       = other.active_grid;
        retained_history  = other.retained_history;
    }

    return *this;
}

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
    return retained_history.logical_rows.empty();
}

int Terminal_screen_model::Primary_backing_buffer::retained_history_size() const
{
    if (retained_history.logical_rows.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        throw std::overflow_error("terminal retained history row count exceeds int range");
    }

    return static_cast<int>(retained_history.logical_rows.size());
}

std::optional<Terminal_screen_model::retained_row_record_t>
Terminal_screen_model::Primary_backing_buffer::materialize_retained_history_record(
    std::size_t index) const
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_screen_model::Primary_backing_buffer::materialize_retained_history_record");

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (index >= retained_history.logical_rows.size()) {
            return std::nullopt;
        }

        Terminal_history_row_traversal_result row =
            retained_history.traversal->resolve_cached_row(
                retained_history.logical_rows[index]);
        if (row.status == Terminal_history_row_traversal_status::OK) {
            return Terminal_screen_model::retained_row_record_from_history_row_record(
                row.row.record);
        }

        if (row.status == Terminal_history_row_traversal_status::CACHE_ENTRY_INVALID) {
            rebuild_retained_history_rows();
            continue;
        }

        throw_retained_history_storage_failure();
    }

    return std::nullopt;
}

std::optional<terminal_history_handle_t>
Terminal_screen_model::Primary_backing_buffer::retained_history_handle(
    std::size_t index) const
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_screen_model::Primary_backing_buffer::retained_history_handle");

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (index >= retained_history.logical_rows.size()) {
            return std::nullopt;
        }

        Terminal_history_row_traversal_result row =
            retained_history.traversal->resolve_cached_row(
                retained_history.logical_rows[index]);
        if (row.status == Terminal_history_row_traversal_status::OK) {
            return row.row.history_handle;
        }

        if (row.status == Terminal_history_row_traversal_status::CACHE_ENTRY_INVALID) {
            rebuild_retained_history_rows();
            continue;
        }

        throw_retained_history_storage_failure();
    }

    return std::nullopt;
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
    if (terminal_history_handle_has_identity(retained_history.latest_history_handle)) {
        identity.previous_row_byte_sequence =
            retained_history.latest_history_handle.byte_sequence;
        identity.previous_row_sequence =
            retained_history.latest_history_handle.row_sequence;
    }

    Terminal_history_row_record_append_result append =
        encode_terminal_history_row_record_to_ring(
            *retained_history.ring,
            history_record,
            identity);
    if (append.status != Terminal_history_row_record_codec_status::OK) {
        throw_retained_history_storage_failure();
    }

    retained_history.latest_history_handle = append.history_handle;
    retained_history.logical_rows.push_back(append.history_handle);
    retained_history.traversal->discard_directory_cache();

    retained_history_append_result_t result;
    result.history_handle = append.history_handle;
    if (append.commit.tail_advanced) {
        result.evicted_rows = prune_retained_history_rows_outside_live_window();
    }
    return result;
}

int Terminal_screen_model::Primary_backing_buffer::discard_oldest_retained_history_records(
    int row_count)
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_screen_model::Primary_backing_buffer::discard_oldest_retained_history_records");

    if (row_count <= 0 || retained_history.logical_rows.empty()) {
        return 0;
    }

    const std::size_t rows_to_discard = std::min(
        static_cast<std::size_t>(row_count),
        retained_history.logical_rows.size());
    const terminal_history_ring_discard_result_t discard =
        retained_history.ring->discard_oldest_records(rows_to_discard);
    if (discard.status != Terminal_history_ring_status::OK) {
        throw_retained_history_storage_failure();
    }

    retained_history.logical_rows.erase(
        retained_history.logical_rows.begin(),
        retained_history.logical_rows.begin() +
            static_cast<std::ptrdiff_t>(discard.discarded_records));
    retained_history.latest_history_handle =
        retained_history.logical_rows.empty()
            ? terminal_history_handle_t{}
            : retained_history.logical_rows.back();
    retained_history.traversal->discard_directory_cache();

    if (discard.discarded_records >
        static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        throw std::overflow_error("terminal retained history discard exceeds int range");
    }

    return static_cast<int>(discard.discarded_records);
}

void Terminal_screen_model::Primary_backing_buffer::clear_retained_history()
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_screen_model::Primary_backing_buffer::clear_retained_history");

    retained_history.reset();
}

void Terminal_screen_model::Primary_backing_buffer::rebuild_retained_history_rows() const
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_screen_model::Primary_backing_buffer::rebuild_retained_history_rows");

    retained_history.logical_rows.clear();

    Terminal_history_row_traversal_result current =
        retained_history.traversal->oldest_live_row();
    if (current.status == Terminal_history_row_traversal_status::EMPTY) {
        retained_history.latest_history_handle = {};
        return;
    }

    while (current.status == Terminal_history_row_traversal_status::OK) {
        retained_history.logical_rows.push_back(current.row.history_handle);
        retained_history.latest_history_handle = current.row.history_handle;
        current = retained_history.traversal->next_row_after(current.row.history_handle);
    }

    if (current.status != Terminal_history_row_traversal_status::NOT_FOUND) {
        throw_retained_history_storage_failure();
    }

    if (retained_history.logical_rows.empty()) {
        retained_history.latest_history_handle = {};
    }
}

int Terminal_screen_model::Primary_backing_buffer::
    prune_retained_history_rows_outside_live_window() const
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_screen_model::Primary_backing_buffer::prune_retained_history_rows_outside_live_window");

    const std::uint64_t oldest_live =
        retained_history.ring->oldest_live_byte_sequence();

    auto first_live = retained_history.logical_rows.begin();
    while (first_live != retained_history.logical_rows.end() &&
        first_live->byte_sequence < oldest_live)
    {
        ++first_live;
    }

    const std::size_t pruned_rows = static_cast<std::size_t>(
        first_live - retained_history.logical_rows.begin());
    retained_history.logical_rows.erase(
        retained_history.logical_rows.begin(),
        first_live);
    retained_history.latest_history_handle =
        retained_history.logical_rows.empty()
            ? terminal_history_handle_t{}
            : retained_history.logical_rows.back();
    retained_history.traversal->discard_directory_cache();

    if (pruned_rows > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::overflow_error("terminal retained history prune exceeds int range");
    }

    return static_cast<int>(pruned_rows);
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

Terminal_screen_model::screen_buffer_state_t
Terminal_screen_model::capture_current_buffer_state() const
{
    return {
        active_grid_rows(),
        m_saved_cursor,
        m_cursor,
        m_scroll_top,
        m_scroll_bottom,
        m_origin_mode,
        m_pending_wrap,
    };
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
    active_buffer_state() = capture_current_buffer_state();
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

void Terminal_screen_model::resize_buffer_state(
    screen_buffer_state_t&             state,
    terminal_grid_size_t               grid_size)
{
    resize_rows(state.rows, grid_size);
    state.cursor.row    = std::clamp(state.cursor.row,    0, grid_size.rows - 1);
    state.cursor.column = std::clamp(state.cursor.column, 0, grid_size.columns - 1);
    if (state.saved_cursor.valid) {
        state.saved_cursor.position.row =
            std::clamp(state.saved_cursor.position.row, 0, grid_size.rows - 1);
        state.saved_cursor.position.column =
            std::clamp(state.saved_cursor.position.column, 0, grid_size.columns - 1);
        state.saved_cursor.pending_wrap = false;
        state.saved_cursor.origin_mode  = false;
    }
    state.scroll_top    = 0;
    state.scroll_bottom = grid_size.rows - 1;
    state.origin_mode   = false;
    state.pending_wrap  = false;
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
    invalidate_retained_lookup_caches();
    row.retained_line_provenance = {
        .retained_line_id   = next_retained_line_id(),
        .content_generation = 0U,
        .source             = source,
    };
}

void Terminal_screen_model::replace_visible_retained_line_ids()
{
    for (Terminal_screen_row& row : active_grid_rows()) {
        replace_retained_line_id(row);
    }
}

void Terminal_screen_model::replace_row_with_erased_retained_line(Terminal_screen_row& row)
{
    fill_row_with_erased_cells(row.cells);
    replace_retained_line_id(row);
}

bool Terminal_screen_model::cells_have_same_selection_content(
    const Cell&    left,
    const Cell&    right) const
{
    return left.text           == right.text              &&
        left.display_width     == right.display_width     &&
        left.wide_continuation == right.wide_continuation &&
        left.occupied          == right.occupied;
}

bool Terminal_screen_model::rows_have_same_selection_content(
    const std::vector<Cell>&   left,
    const std::vector<Cell>&   right) const
{
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!cells_have_same_selection_content(left[index], right[index])) {
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

    Cell intended_cell;
    intended_cell.text              = printable_ascii_cell_text(text);
    intended_cell.display_width     = 1;
    intended_cell.wide_continuation = false;
    intended_cell.occupied          = true;

    return !cells_have_same_selection_content(
        row.cells[static_cast<std::size_t>(column)],
        intended_cell);
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

bool Terminal_screen_model::scalar_span_changes_selection_content(
    const Terminal_screen_row& row,
    terminal_grid_position_t   position,
    QStringView                text,
    int                        display_width) const
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
            intended_cell.wide_continuation = false;
            intended_cell.occupied          = true;
        }
        else
        if (column > position.column && column < new_span_end_column) {
            intended_cell.text              = {};
            intended_cell.display_width     = 0;
            intended_cell.wide_continuation = true;
            intended_cell.occupied          = true;
        }

        if (!cells_have_same_selection_content(
                row.cells[static_cast<std::size_t>(column)],
                intended_cell))
        {
            return true;
        }
    }

    return false;
}

bool Terminal_screen_model::scalar_span_clear_changes_selection_content(
    const Terminal_screen_row& row,
    terminal_grid_position_t   position) const
{
    const terminal_grid_position_t base_position = cell_base_position(position);
    const Cell&                    base_cell =
        row.cells[static_cast<std::size_t>(base_position.column)];
    const int clear_width = std::max(1, base_cell.display_width);

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
            return true;
        }
    }

    return false;
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

    invalidate_retained_lookup_caches();
    if (row.retained_line_provenance.content_generation ==
        std::numeric_limits<std::uint64_t>::max())
    {
        throw std::overflow_error("terminal retained line content generation space exhausted");
    }

    ++row.retained_line_provenance.content_generation;
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
    bool                 guard_scrollback_clear)
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
    resize_buffer_state(m_primary_backing.active_grid_state(), grid_size);
    resize_buffer_state(m_alternate_grid.active_grid_state(),  grid_size);
    m_tab_stops = default_tab_stops(grid_size.columns);
    m_config.grid_size = grid_size;
    restore_buffer_state(active_buffer_state());
    mark_grid_reflow_changed();
    if (guard_scrollback_clear) {
        arm_resize_repaint_clear_guard();
        arm_primary_repaint_recovery_resize_guard();
    }
    else {
        cancel_resize_repaint_clear_guard();
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

    return true;
}

Terminal_screen_model_result Terminal_screen_model::resize(terminal_grid_size_t grid_size)
{
    Terminal_screen_model_result result;
    m_scrollback_evicted_rows = 0;
    clear_backing_deltas();
    clear_recovery_proposals();
    clear_dirty();

    const bool grid_resized = apply_grid_resize(grid_size, true);
    const bool resize_terminal_content_changed = m_terminal_content_changed;
    const bool resize_active_buffer_changed    = m_active_buffer_changed;
    const bool resize_grid_reflow_changed      = m_grid_reflow_changed;
    if (!grid_resized) {
        result.dirty_rows                    = dirty_rows();
        result.terminal_content_changed      = resize_terminal_content_changed;
        result.active_buffer_changed         = resize_active_buffer_changed;
        result.grid_reflow_changed           = resize_grid_reflow_changed;
        result.viewport_changed              = m_viewport_changed;
        result.mode_state_changed            = m_mode_state_changed;
        result.mouse_reporting_mode_changed  = m_mouse_reporting_mode_changed;
        result.alternate_scroll_mode_changed = m_alternate_scroll_mode_changed;
        result.scrollback_rows               = scrollback_size();
        result.backing_deltas                = m_backing_deltas;
        result.recovery_proposals            = m_recovery_proposals;
        result.evicted_scrollback_rows       = compatibility_evicted_scrollback_rows();
        return result;
    }

    if (m_modes.synchronized_output) {
        collect_synchronized_changes();
        if (resize_grid_reflow_changed) {
            m_synchronized_grid_reflow_changed = false;
        }
    }
    retain_referenced_active_hyperlink_ids();

    result.dirty_rows                    = dirty_rows();
    result.terminal_content_changed      = resize_terminal_content_changed;
    result.active_buffer_changed         = resize_active_buffer_changed;
    result.grid_reflow_changed           = resize_grid_reflow_changed;
    result.viewport_changed              = m_viewport_changed;
    result.mode_state_changed            = m_mode_state_changed;
    result.mouse_reporting_mode_changed  = m_mouse_reporting_mode_changed;
    result.alternate_scroll_mode_changed = m_alternate_scroll_mode_changed;
    result.scrollback_rows               = scrollback_size();
    result.backing_deltas                = m_backing_deltas;
    result.recovery_proposals            = m_recovery_proposals;
    result.evicted_scrollback_rows       = compatibility_evicted_scrollback_rows();
    return result;
}

Terminal_screen_model_result Terminal_screen_model::set_scrollback_limit(int limit)
{
    Terminal_screen_model_result result;
    m_scrollback_evicted_rows = 0;
    clear_backing_deltas();
    clear_recovery_proposals();
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
        result.dirty_rows                    = dirty_rows();
        result.terminal_content_changed      = m_terminal_content_changed;
        result.active_buffer_changed         = m_active_buffer_changed;
        result.grid_reflow_changed           = m_grid_reflow_changed;
        result.viewport_changed              = m_viewport_changed;
        result.mode_state_changed            = m_mode_state_changed;
        result.mouse_reporting_mode_changed  = m_mouse_reporting_mode_changed;
        result.alternate_scroll_mode_changed = m_alternate_scroll_mode_changed;
        result.scrollback_rows               = scrollback_size();
        result.backing_deltas                = m_backing_deltas;
        result.recovery_proposals            = m_recovery_proposals;
        result.evicted_scrollback_rows       = compatibility_evicted_scrollback_rows();
        return result;
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

    result.dirty_rows                    = dirty_rows();
    result.terminal_content_changed      = m_terminal_content_changed;
    result.active_buffer_changed         = m_active_buffer_changed;
    result.grid_reflow_changed           = m_grid_reflow_changed;
    result.viewport_changed              = m_viewport_changed;
    result.mode_state_changed            = m_mode_state_changed;
    result.mouse_reporting_mode_changed  = m_mouse_reporting_mode_changed;
    result.alternate_scroll_mode_changed = m_alternate_scroll_mode_changed;
    result.scrollback_rows               = scrollback_size();
    result.backing_deltas                = m_backing_deltas;
    result.recovery_proposals            = m_recovery_proposals;
    result.evicted_scrollback_rows       = compatibility_evicted_scrollback_rows();
    return result;
}

void Terminal_screen_model::set_primary_repaint_recovery_enabled(bool enabled)
{
    if (m_config.recover_scrollback_from_primary_repaints == enabled) {
        return;
    }

    m_config.recover_scrollback_from_primary_repaints = enabled;
    if (!enabled) {
        cancel_resize_repaint_clear_guard();
        cancel_primary_repaint_recovery_resize_guard();
        cancel_primary_repaint_recovery_candidate();
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
    if (!text.isEmpty()) {
        cancel_resize_repaint_clear_guard_before_visible_clear();
    }

    for (qsizetype i = 0; i < text.size();) {
        const qsizetype ascii_begin = i;
        while (i < text.size() && is_printable_ascii(text[i])) {
            ++i;
        }
        if (i > ascii_begin) {
            put_printable_ascii_text(QStringView(text).sliced(ascii_begin, i - ascii_begin));
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

    ++m_printable_text_profile_depth;
    const profile_depth_guard_t profile_depth_guard{m_printable_text_profile_depth};
#endif
    qsizetype offset = 0;
    while (offset < text.size()) {
        if (m_modes.autowrap && m_pending_wrap) {
#if VNM_TERMINAL_PROFILING_ENABLED
            if (m_profile_stats.enabled) {
                ++m_profile_stats.line_wraps_from_text_writes;
            }
#endif
            carriage_return();
            line_feed();
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
            bool selection_content_changed = false;
            if (available_columns > 1) {
                selection_content_changed =
                    printable_ascii_span_changes_selection_content(
                        screen_row,
                        m_cursor.column,
                        text.sliced(offset, available_columns - 1));
                write_printable_ascii_span_content(
                    m_cursor.row,
                    m_cursor.column,
                    text.sliced(offset, available_columns - 1));
            }
            selection_content_changed =
                selection_content_changed ||
                printable_ascii_cell_changes_selection_content(
                    screen_row,
                    m_config.grid_size.columns - 1,
                    text[text.size() - 1]);
            write_printable_ascii_cell_content(
                { m_cursor.row, m_config.grid_size.columns - 1 },
                text[text.size() - 1]);
            advance_row_content_generation_with_change_flag(
                screen_row,
                selection_content_changed);
            mark_dirty(m_cursor.row);
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
    write_printable_ascii_span_content(row, first_column, text);
    advance_row_content_generation_with_change_flag(screen_row, selection_content_changed);
    mark_dirty(row);
}

void Terminal_screen_model::write_printable_ascii_span_content(
    int                        row,
    int                        first_column,
    QStringView                text)
{
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled) {
        m_profile_stats.printable_ascii_cells_written +=
            static_cast<std::uint64_t>(text.size());
    }
#endif
    for (qsizetype offset = 0; offset < text.size(); ++offset) {
        write_printable_ascii_cell_content(
            { row, first_column + static_cast<int>(offset) },
            text[offset]);
    }
}

void Terminal_screen_model::write_printable_ascii_cell(
    terminal_grid_position_t   position,
    QChar                      text)
{
    Terminal_screen_row& screen_row =
        active_grid_rows()[static_cast<std::size_t>(position.row)];
    const bool selection_content_changed =
        printable_ascii_cell_changes_selection_content(screen_row, position.column, text);

    write_printable_ascii_cell_content(position, text);
    advance_row_content_generation_with_change_flag(screen_row, selection_content_changed);
}

void Terminal_screen_model::write_printable_ascii_cell_content(
    terminal_grid_position_t   position,
    QChar                      text)
{
    mark_terminal_content_changed();
    clear_cell_at(position);

    Cell& cell = active_grid_rows()[position.row].cells[position.column];
    cell.text              = printable_ascii_cell_text(text);
    cell.display_width     = 1;
    cell.wide_continuation = false;
    cell.occupied          = true;
    cell.style_id          = m_current_style_id;
    cell.hyperlink_id      = m_current_hyperlink_id;
}

void Terminal_screen_model::put_spacing_scalar(QString text, int display_width)
{
    if (display_width > m_config.grid_size.columns) {
        display_width = 1;
    }

    if (m_modes.autowrap && m_pending_wrap) {
        carriage_return();
        line_feed();
        m_pending_wrap = false;
    }

    if (display_width > m_config.grid_size.columns - m_cursor.column) {
        if (m_modes.autowrap) {
            carriage_return();
            line_feed();
        }
        else {
            return;
        }
    }

    place_cell_text(m_cursor, std::move(text), display_width);
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
    int     display_width = measure_utf8_width(combined_text.toUtf8()).cells;
    if (display_width <= 0)                          { display_width = 1; }
    if (display_width >  m_config.grid_size.columns) { display_width = 1; }
    if (display_width > m_config.grid_size.columns - target.column) {
        if (!m_modes.autowrap) {
            display_width = 1;
        }
        else {
            const Terminal_style_id style_id     = cell.style_id;
            const std::uint64_t     hyperlink_id = cell.hyperlink_id;
            Terminal_screen_row& screen_row =
                active_grid_rows()[static_cast<std::size_t>(target.row)];
            const bool selection_content_changed =
                scalar_span_clear_changes_selection_content(screen_row, target);
            clear_cell_at(target);
            advance_row_content_generation_with_change_flag(
                screen_row,
                selection_content_changed);
            mark_dirty(target.row);
            m_cursor = target;
            carriage_return();
            line_feed();
            install_cell_span(
                m_cursor,
                std::move(combined_text),
                display_width,
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
        cell.style_id,
        cell.hyperlink_id);
    set_cursor_after_cell(target, display_width);
}

void Terminal_screen_model::install_cell_span(
    terminal_grid_position_t   position,
    QString                    text,
    int                        display_width,
    Terminal_style_id          style_id,
    std::uint64_t              hyperlink_id)
{
    mark_terminal_content_changed();
    Terminal_screen_row& screen_row =
        active_grid_rows()[static_cast<std::size_t>(position.row)];
    const bool selection_content_changed =
        scalar_span_changes_selection_content(
            screen_row,
            position,
            QStringView(text),
            display_width);
    clear_cell_at(position);

    Cell& cell = screen_row.cells[position.column];
    cell.text              = std::move(text);
    cell.display_width     = display_width;
    cell.wide_continuation = false;
    cell.occupied          = true;
    cell.style_id          = style_id;
    cell.hyperlink_id      = hyperlink_id;

    for (int width_offset = 1; width_offset < display_width; ++width_offset) {
        clear_cell_at({position.row, position.column + width_offset});
        Cell& continuation = active_grid_rows()[position.row].cells[position.column + width_offset];
        continuation.text              = {};
        continuation.display_width     = 0;
        continuation.wide_continuation = true;
        continuation.occupied          = true;
        continuation.style_id          = cell.style_id;
        continuation.hyperlink_id      = cell.hyperlink_id;
    }

    advance_row_content_generation_with_change_flag(screen_row, selection_content_changed);
    mark_dirty(position.row);
}

void Terminal_screen_model::place_cell_text(
    terminal_grid_position_t   position,
    QString                    text,
    int                        display_width)
{
    install_cell_span(
        position,
        std::move(text),
        display_width,
        m_current_style_id,
        m_current_hyperlink_id);
}

void Terminal_screen_model::clear_cell_span(terminal_grid_position_t position)
{
    mark_terminal_content_changed();
    Cell&     cell          = active_grid_rows()[position.row].cells[position.column];
    const int display_width = cell.display_width;
    cell = Cell{};

    for (int width_offset = 1; width_offset < display_width; ++width_offset) {
        active_grid_rows()[position.row].cells[position.column + width_offset] = Cell{};
    }
}

void Terminal_screen_model::clear_cell_at(terminal_grid_position_t position)
{
    const terminal_grid_position_t base_position = cell_base_position(position);
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled && m_printable_text_profile_depth > 0) {
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
    if (row < 0 || row >= m_config.grid_size.rows) {
        return;
    }

    first_column = std::clamp(first_column, 0, m_config.grid_size.columns - 1);
    last_column  = std::clamp(last_column,  0, m_config.grid_size.columns - 1);
    if (first_column > last_column) {
        return;
    }

    Terminal_screen_row& screen_row = active_grid_rows()[static_cast<std::size_t>(row)];
    const std::vector<Cell> before_cells = screen_row.cells;
    for (int column = first_column; column <= last_column; ++column) {
        erase_cell_at({row, column});
    }

    advance_row_content_generation_if_changed(screen_row, before_cells);
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
    note_resize_repaint_visible_clear();
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
            if (consume_resize_repaint_scrollback_clear_guard()) {
                break;
            }
            if (m_active_buffer_id == Terminal_buffer_id::PRIMARY &&
                m_primary_backing.retained_history_empty())
            {
                record_primary_history_delta(
                    Terminal_backing_delta_kind::BACKING_UNCHANGED,
                    scrollback_size(),
                    scrollback_size(),
                    0,
                    0,
                    0);
            }
            if (m_active_buffer_id == Terminal_buffer_id::PRIMARY &&
                !m_primary_backing.retained_history_empty())
            {
                const int scrollback_rows_before = scrollback_size();
                m_scrollback_evicted_rows       += scrollback_rows_before;
                m_primary_backing.clear_retained_history();
                invalidate_retained_lookup_caches();
                record_primary_history_delta(
                    Terminal_backing_delta_kind::PRIMARY_HISTORY_CLEARED,
                    scrollback_rows_before,
                    0,
                    0,
                    scrollback_rows_before,
                    0);
                mark_terminal_content_changed();
                mark_viewport_changed();
            }
            break;
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

void Terminal_screen_model::insert_cells(int count)
{
    mark_terminal_content_changed();
    count = std::clamp(count, 1, m_config.grid_size.columns - m_cursor.column);
    Terminal_screen_row& screen_row = active_grid_rows()[static_cast<std::size_t>(m_cursor.row)];
    std::vector<Cell>& row = screen_row.cells;
    const std::vector<Cell> before_cells = row;

    const auto clear_wide_boundary = [&](int column) {
        if (column >= 0                          &&
            column <  m_config.grid_size.columns &&
            row[static_cast<std::size_t>(column)].wide_continuation)
        {
            erase_cell_at({m_cursor.row, column});
        }
    };
    clear_wide_boundary(m_cursor.column);
    clear_wide_boundary(m_config.grid_size.columns - count);

    std::move_backward(
        row.begin() + m_cursor.column,
        row.begin() + m_config.grid_size.columns - count,
        row.end());

    const Cell replacement = erased_cell();
    for (int column = m_cursor.column; column < m_cursor.column + count; ++column) {
        row[static_cast<std::size_t>(column)] = replacement;
    }
    repair_wide_spans_in_row(row, m_config.grid_size.columns);

    m_pending_wrap = false;
    advance_row_content_generation_if_changed(screen_row, before_cells);
    mark_dirty(m_cursor.row);
}

void Terminal_screen_model::delete_cells(int count)
{
    mark_terminal_content_changed();
    count = std::clamp(count, 1, m_config.grid_size.columns - m_cursor.column);
    Terminal_screen_row& screen_row = active_grid_rows()[static_cast<std::size_t>(m_cursor.row)];
    std::vector<Cell>& row = screen_row.cells;
    const std::vector<Cell> before_cells = row;

    const auto clear_wide_boundary = [&](int column) {
        if (column >= 0                          &&
            column <  m_config.grid_size.columns &&
            row[static_cast<std::size_t>(column)].wide_continuation)
        {
            erase_cell_at({m_cursor.row, column});
        }
    };
    for (int column = m_cursor.column; column < m_cursor.column + count; ++column) {
        erase_cell_at({m_cursor.row, column});
    }
    clear_wide_boundary(m_cursor.column + count);

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
    repair_wide_spans_in_row(row, m_config.grid_size.columns);

    m_pending_wrap = false;
    advance_row_content_generation_if_changed(screen_row, before_cells);
    mark_dirty(m_cursor.row);
}

void Terminal_screen_model::insert_lines(int count)
{
    if (m_cursor.row < m_scroll_top || m_cursor.row > m_scroll_bottom) {
        return;
    }

    mark_terminal_content_changed();
    count = std::clamp(count, 1, m_scroll_bottom - m_cursor.row + 1);
    std::move_backward(
        active_grid_rows().begin() + m_cursor.row,
        active_grid_rows().begin() + m_scroll_bottom - count + 1,
        active_grid_rows().begin() + m_scroll_bottom + 1);

    for (int row = m_cursor.row; row < m_cursor.row + count; ++row) {
        replace_row_with_erased_retained_line(active_grid_rows()[static_cast<std::size_t>(row)]);
    }

    m_pending_wrap = false;
    for (int row = m_cursor.row; row <= m_scroll_bottom; ++row) {
        mark_dirty(row);
    }
}

void Terminal_screen_model::delete_lines(int count)
{
    if (m_cursor.row < m_scroll_top || m_cursor.row > m_scroll_bottom) {
        return;
    }

    mark_terminal_content_changed();
    count = std::clamp(count, 1, m_scroll_bottom - m_cursor.row + 1);
    std::move(
        active_grid_rows().begin() + m_cursor.row + count,
        active_grid_rows().begin() + m_scroll_bottom + 1,
        active_grid_rows().begin() + m_cursor.row);

    for (int row = m_scroll_bottom - count + 1; row <= m_scroll_bottom; ++row) {
        replace_row_with_erased_retained_line(active_grid_rows()[static_cast<std::size_t>(row)]);
    }

    m_pending_wrap = false;
    for (int row = m_cursor.row; row <= m_scroll_bottom; ++row) {
        mark_dirty(row);
    }
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
    retain_referenced_active_hyperlink_ids();
    m_current_hyperlink_id = identity_key.isEmpty()
        ? 0U
        : active_hyperlink_id_for_identity(identity_key);
}

void Terminal_screen_model::set_synchronized_output_mode(
    bool                           enabled,
    ingest_publication_t*          publication)
{
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

void Terminal_screen_model::append_scrollback_row(
    const Terminal_screen_row&               row,
    Terminal_retained_line_provenance_source source,
    const std::map<std::uint64_t, QByteArray>* hyperlink_identity_keys)
{
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_profile_stats.enabled && m_printable_text_profile_depth > 0) {
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
                seal_retained_row_record(row, source, hyperlink_identity_keys));
        }
        m_scrollback_evicted_rows += append.evicted_rows;
        record_primary_history_delta(
            Terminal_backing_delta_kind::PRIMARY_HISTORY_APPENDED,
            scrollback_rows_before,
            scrollback_size(),
            1,
            append.evicted_rows,
            0);
        while (scrollback_size() > m_config.scrollback_limit) {
            evict_oldest_scrollback_rows(
                scrollback_size() - m_config.scrollback_limit);
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
}

void Terminal_screen_model::scroll_up_region(
    int    top,
    int    bottom,
    bool   append_scrollback,
    int    count)
{
    mark_terminal_content_changed();
    count = std::clamp(count, 1, bottom - top + 1);
    for (int step = 0; step < count; ++step) {
        if (append_scrollback) {
            append_scrollback_row(active_grid_rows()[static_cast<std::size_t>(top)]);
        }

        for (int row = top + 1; row <= bottom; ++row) {
            active_grid_rows()[static_cast<std::size_t>(row - 1)] =
                std::move(active_grid_rows()[static_cast<std::size_t>(row)]);
        }
        replace_row_with_erased_retained_line(active_grid_rows()[static_cast<std::size_t>(bottom)]);
    }

    for (int row = top; row <= bottom; ++row) {
        mark_dirty(row);
    }
}

void Terminal_screen_model::scroll_down_region(int top, int bottom, int count)
{
    mark_terminal_content_changed();
    count = std::clamp(count, 1, bottom - top + 1);
    for (int step = 0; step < count; ++step) {
        for (int row = bottom - 1; row >= top; --row) {
            active_grid_rows()[static_cast<std::size_t>(row + 1)] =
                std::move(active_grid_rows()[static_cast<std::size_t>(row)]);
        }
        replace_row_with_erased_retained_line(active_grid_rows()[static_cast<std::size_t>(top)]);
    }

    for (int row = top; row <= bottom; ++row) {
        mark_dirty(row);
    }
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

void Terminal_screen_model::arm_resize_repaint_clear_guard()
{
    if (!m_config.recover_scrollback_from_primary_repaints ||
        m_active_buffer_id != Terminal_buffer_id::PRIMARY   ||
        m_primary_backing.retained_history_empty())
    {
        cancel_resize_repaint_clear_guard();
        return;
    }

    m_resize_repaint_clear_guard_remaining         =
        k_resize_repaint_clear_guard_action_budget;
    m_resize_repaint_clear_guard_saw_visible_clear = false;
}

void Terminal_screen_model::cancel_resize_repaint_clear_guard()
{
    m_resize_repaint_clear_guard_remaining         = 0;
    m_resize_repaint_clear_guard_saw_visible_clear = false;
}

void Terminal_screen_model::cancel_resize_repaint_clear_guard_before_visible_clear()
{
    if (m_resize_repaint_clear_guard_saw_visible_clear) {
        return;
    }

    cancel_resize_repaint_clear_guard();
}

void Terminal_screen_model::advance_resize_repaint_clear_guard()
{
    if (m_resize_repaint_clear_guard_remaining <= 0) {
        return;
    }

    --m_resize_repaint_clear_guard_remaining;
    if (m_resize_repaint_clear_guard_remaining <= 0) {
        cancel_resize_repaint_clear_guard();
    }
}

void Terminal_screen_model::note_resize_repaint_visible_clear()
{
    if (m_resize_repaint_clear_guard_remaining <= 0 ||
        m_cursor.row                               != 0 ||
        m_cursor.column                            != 0)
    {
        return;
    }

    m_resize_repaint_clear_guard_saw_visible_clear = true;
}

bool Terminal_screen_model::consume_resize_repaint_scrollback_clear_guard()
{
    if (m_resize_repaint_clear_guard_remaining <= 0 ||
        !m_resize_repaint_clear_guard_saw_visible_clear)
    {
        return false;
    }

    cancel_resize_repaint_clear_guard();
    return true;
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
        k_resize_repaint_clear_guard_action_budget;
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

    bool has_visible_row = false;
    for (const Terminal_screen_row& row : active_grid_rows()) {
        has_visible_row = has_visible_row || row_has_visible_text(row);
    }
    if (!has_visible_row) {
        cancel_primary_repaint_recovery_candidate();
        return;
    }

    m_primary_repaint_recovery_candidate.rows                         = active_grid_rows();
    m_primary_repaint_recovery_candidate.hyperlink_identity_keys.clear();
    for (const Terminal_screen_row& row : m_primary_repaint_recovery_candidate.rows) {
        retained_row_record_t retained_record;
        retained_record.row = row;
        materialize_retained_row_hyperlinks(retained_record);
        m_primary_repaint_recovery_candidate.hyperlink_identity_keys.insert(
            retained_record.hyperlink_identity_keys.begin(),
            retained_record.hyperlink_identity_keys.end());
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
    if (!m_primary_repaint_recovery_candidate.active) {
        return;
    }

    primary_repaint_recovery_candidate_t candidate =
        std::move(m_primary_repaint_recovery_candidate);
    m_primary_repaint_recovery_candidate = {};

    if (candidate.visible_row_identity_ambiguous) {
        for (Terminal_screen_row& row : candidate.rows) {
            replace_retained_line_id(row);
        }
    }

    std::optional<primary_repaint_recovery_proposal_t> proposal =
        primary_repaint_recovery_proposal(candidate);
    if (!proposal.has_value()) {
        if (candidate.visible_row_identity_ambiguous) {
            replace_visible_retained_line_ids();
            mark_all_dirty();
            candidate.visible_row_identity_ambiguous = false;
        }
        if (!discard_if_no_match && candidate.unmatched_finish_budget > 0) {
            --candidate.unmatched_finish_budget;
            m_primary_repaint_recovery_candidate = std::move(candidate);
        }
        return;
    }

    accept_primary_repaint_recovery_proposal(*proposal);
    for (Terminal_screen_row& row : active_grid_rows()) {
        replace_retained_line_id(row);
    }
    mark_all_dirty();
}

void Terminal_screen_model::cancel_primary_repaint_recovery_candidate()
{
    if (m_primary_repaint_recovery_candidate.active &&
        m_primary_repaint_recovery_candidate.visible_row_identity_ambiguous)
    {
        replace_visible_retained_line_ids();
        mark_all_dirty();
    }
    m_primary_repaint_recovery_candidate = {};
}

void Terminal_screen_model::accept_primary_repaint_recovery_proposal(
    const primary_repaint_recovery_proposal_t& proposal)
{
    for (Terminal_screen_row recovered_row : proposal.rows) {
        append_scrollback_row(
            recovered_row,
            proposal.metadata.provenance_source,
            &proposal.hyperlink_identity_keys);
    }
    m_recovery_proposals.push_back(proposal.metadata);
}

std::optional<Terminal_screen_model::primary_repaint_recovery_proposal_t>
Terminal_screen_model::primary_repaint_recovery_proposal(
    const primary_repaint_recovery_candidate_t& candidate) const
{
    const int scrolled_rows = primary_repaint_recovery_shift_rows(candidate);
    if (scrolled_rows <= 0) {
        return std::nullopt;
    }

    primary_repaint_recovery_proposal_t proposal;
    proposal.rows.reserve(static_cast<std::size_t>(scrolled_rows));
    for (int row = 0; row < scrolled_rows; ++row) {
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
    proposal.metadata.recovered_row_count = scrolled_rows;
    proposal.metadata.visible_row_identity_ambiguous =
        candidate.visible_row_identity_ambiguous;
    return proposal;
}

int Terminal_screen_model::primary_repaint_recovery_shift_rows(
    const primary_repaint_recovery_candidate_t& candidate) const
{
    terminal_repaint_recovery_shift_input_t input;
    input.candidate_active                  = candidate.active;
    input.primary_buffer_active             = m_active_buffer_id == Terminal_buffer_id::PRIMARY;
    input.scrollback_rows_unchanged         = candidate.scrollback_rows == scrollback_size();
    input.line_start_clear_before_text      = candidate.line_start_clear_before_text;
    input.explicit_non_home_repaint_address =
        candidate.explicit_non_home_repaint_address;

    input.candidate_rows.reserve(candidate.rows.size());
    for (const Terminal_screen_row& row : candidate.rows) {
        input.candidate_rows.push_back(
            row_text_from_cells(row.cells, 0, m_config.grid_size.columns));
    }

    input.current_rows.reserve(active_grid_rows().size());
    for (const Terminal_screen_row& row : active_grid_rows()) {
        input.current_rows.push_back(
            row_text_from_cells(row.cells, 0, m_config.grid_size.columns));
    }

    return internal::primary_repaint_recovery_shift_rows(input);
}

bool Terminal_screen_model::row_has_visible_text(const Terminal_screen_row& row) const
{
    return !row_text_from_cells(row.cells, 0, m_config.grid_size.columns).isEmpty();
}

void Terminal_screen_model::carriage_return()
{
    cancel_resize_repaint_clear_guard_before_visible_clear();
    mark_cursor_dirty();
    m_cursor.column = 0;
    m_pending_wrap  = false;
    mark_cursor_dirty();
}

void Terminal_screen_model::line_feed()
{
    cancel_resize_repaint_clear_guard_before_visible_clear();
    mark_cursor_dirty();
    m_pending_wrap = false;

    if (m_cursor.row == m_scroll_bottom) {
        scroll_up_region(
            m_scroll_top,
            m_scroll_bottom,
            m_active_buffer_id == Terminal_buffer_id::PRIMARY &&
                m_scroll_top == 0);
        mark_cursor_dirty();
        return;
    }

    if (m_cursor.row < m_config.grid_size.rows - 1) {
        ++m_cursor.row;
        mark_cursor_dirty();
        return;
    }
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
    if (m_profile_stats.enabled && m_printable_text_profile_depth > 0) {
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

    if (row == m_last_dirty_row) {
#if VNM_TERMINAL_PROFILING_ENABLED
        if (m_dirty_row_stats.enabled) {
            ++m_dirty_row_stats.duplicate_mark_requests;
            ++dirty_row_stats_bucket().duplicate_mark_requests;
        }
#endif
        return;
    }

    const bool inserted = m_dirty_rows.insert(row).second;
    m_last_dirty_row = row;
#if VNM_TERMINAL_PROFILING_ENABLED
    if (m_dirty_row_stats.enabled) {
        Terminal_screen_model_dirty_row_bucket_stats& bucket = dirty_row_stats_bucket();
        if (inserted) {
            ++m_dirty_row_stats.unique_pending_row_marks;
            ++bucket.unique_pending_row_marks;
            update_pending_dirty_row_stats_watermark();
        }
        else {
            ++m_dirty_row_stats.duplicate_mark_requests;
            ++bucket.duplicate_mark_requests;
        }
    }
#else
    Q_UNUSED(inserted);
#endif
}

void Terminal_screen_model::mark_terminal_content_changed()
{
    invalidate_retained_lookup_caches();
    if (m_primary_repaint_recovery_candidate.active &&
        m_active_buffer_id == Terminal_buffer_id::PRIMARY)
    {
        m_primary_repaint_recovery_candidate.visible_row_identity_ambiguous = true;
    }
    m_terminal_content_changed = true;
}

void Terminal_screen_model::mark_active_buffer_changed()
{
    invalidate_retained_lookup_caches();
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

    const int evicted_rows =
        m_primary_backing.discard_oldest_retained_history_records(rows_to_evict);
    if (evicted_rows <= 0) {
        return;
    }

    invalidate_retained_lookup_caches();
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

    return {m_dirty_rows.begin(), m_dirty_rows.end()};
}

void Terminal_screen_model::collect_synchronized_changes()
{
    VNM_TERMINAL_PROFILE_SCOPE("Terminal_screen_model::collect_synchronized_changes");

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
    m_synchronized_dirty_rows.clear();
    m_synchronized_viewport_changed              = false;
    m_synchronized_terminal_content_changed      = false;
    m_synchronized_active_buffer_changed         = false;
    m_synchronized_grid_reflow_changed           = false;
    m_synchronized_mode_state_changed            = false;
    m_synchronized_mouse_reporting_mode_changed  = false;
    m_synchronized_alternate_scroll_mode_changed = false;
}

std::uint64_t Terminal_screen_model::next_hyperlink_id()
{
    const std::uint64_t id = m_next_hyperlink_id++;
    if (m_next_hyperlink_id == 0U) {
        m_next_hyperlink_id = 1U;
    }
    return id == 0U ? next_hyperlink_id() : id;
}

std::uint64_t Terminal_screen_model::active_hyperlink_id_for_identity(
    const QByteArray& identity_key)
{
    const auto found = m_active_hyperlink_ids.find(identity_key);
    if (found != m_active_hyperlink_ids.end()) {
        return found->second;
    }

    const std::uint64_t hyperlink_id = next_hyperlink_id();
    m_active_hyperlink_ids.emplace(identity_key, hyperlink_id);
    return hyperlink_id;
}

const QByteArray* Terminal_screen_model::active_hyperlink_identity_key(
    std::uint64_t hyperlink_id) const
{
    const auto found = std::find_if(
        m_active_hyperlink_ids.begin(),
        m_active_hyperlink_ids.end(),
        [hyperlink_id](const auto& entry) {
            return entry.second == hyperlink_id;
        });
    return found != m_active_hyperlink_ids.end() ? &found->first : nullptr;
}

void Terminal_screen_model::retain_referenced_active_hyperlink_ids()
{
    if (m_current_hyperlink_id == 0U && m_active_hyperlink_ids.empty()) {
        return;
    }

    std::set<std::uint64_t> live_ids;

    const auto collect_cells = [&](const std::vector<Terminal_screen_row>& rows) {
        for (const Terminal_screen_row& row : rows) {
            for (const Cell& cell : row.cells) {
                if (cell.hyperlink_id != 0U) {
                    live_ids.insert(cell.hyperlink_id);
                }
            }
        }
    };

    if (m_current_hyperlink_id != 0U) {
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
}

Terminal_screen_model::retained_row_record_t Terminal_screen_model::seal_retained_row_record(
    const Terminal_screen_row&               screen_row,
    Terminal_retained_line_provenance_source source,
    const std::map<std::uint64_t, QByteArray>* hyperlink_identity_keys)
{
    retained_row_record_t record;
    record.row                   = screen_row;
    record.metadata.source_width = m_config.grid_size.columns;
    if (source != Terminal_retained_line_provenance_source::TERMINAL_STORAGE) {
        replace_retained_line_id(record.row, source);
    }
    materialize_retained_row_hyperlinks(record, hyperlink_identity_keys);
    return record;
}

Terminal_history_row_record Terminal_screen_model::history_row_record_from_retained_record(
    const retained_row_record_t& retained_record)
{
    Terminal_history_row_record history_record;
    history_record.provenance = retained_record.row.retained_line_provenance;
    history_record.hyperlink_identity_keys = retained_record.hyperlink_identity_keys;
    history_record.metadata = retained_record.metadata;
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
    retained_record.hyperlink_identity_keys = history_record.hyperlink_identity_keys;
    retained_record.metadata = history_record.metadata;
    retained_record.row.cells.reserve(history_record.cells.size());

    for (const Terminal_history_row_cell& cell : history_record.cells) {
        retained_record.row.cells.push_back({
            cell.text,
            cell.display_width,
            cell.wide_continuation,
            cell.occupied,
            cell.style_id,
            cell.hyperlink_id,
        });
    }

    return retained_record;
}

void Terminal_screen_model::materialize_retained_row_hyperlinks(
    retained_row_record_t& row,
    const std::map<std::uint64_t, QByteArray>* preserved_identity_keys) const
{
    std::map<std::uint64_t, QByteArray> old_identity_keys =
        std::move(row.hyperlink_identity_keys);
    row.hyperlink_identity_keys.clear();

    for (const Cell& cell : row.row.cells) {
        if (cell.hyperlink_id == 0U) {
            continue;
        }

        auto old_found = old_identity_keys.find(cell.hyperlink_id);
        if (old_found != old_identity_keys.end()) {
            row.hyperlink_identity_keys[cell.hyperlink_id] = old_found->second;
            continue;
        }

        if (preserved_identity_keys != nullptr) {
            auto preserved_found =
                preserved_identity_keys->find(cell.hyperlink_id);
            if (preserved_found != preserved_identity_keys->end()) {
                row.hyperlink_identity_keys[cell.hyperlink_id] =
                    preserved_found->second;
                continue;
            }
        }

        const QByteArray* active_identity_key =
            active_hyperlink_identity_key(cell.hyperlink_id);
        if (active_identity_key != nullptr) {
            row.hyperlink_identity_keys[cell.hyperlink_id] = *active_identity_key;
        }
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

Terminal_screen_model::snapshot_row_t
Terminal_screen_model::snapshot_row_from_viewport(viewport_row_t row) const
{
    return {row.value};
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
                static_cast<std::size_t>(row.value));
        }
        return retained_record.has_value()
            ? std::optional<Terminal_screen_row>(retained_record->row)
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

std::vector<int> Terminal_screen_model::viewport_dirty_rows(
    const Terminal_viewport_state& viewport,
    const std::vector<int>&        dirty_rows) const
{
    if (viewport.active_buffer == Terminal_buffer_id::ALTERNATE) {
        return dirty_rows;
    }

    std::vector<int> viewport_rows;
    for (int dirty_row : dirty_rows) {
        const primary_backing_row_t backing_row =
            primary_backing_row_from_active(active_grid_row_t{dirty_row});
        const std::optional<viewport_row_t> viewport_row =
            viewport_row_from_primary_backing(viewport, backing_row);
        if (viewport_row.has_value()) {
            viewport_rows.push_back(viewport_row->value);
        }
    }

    return viewport_rows;
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

void Terminal_screen_model::append_snapshot_cells_from_row(
    Terminal_render_snapshot&  snapshot,
    const std::vector<Cell>&   row,
    int                        snapshot_row) const
{
    VNM_TERMINAL_PROFILE_SCOPE(
        "Terminal_screen_model::append_snapshot_cells_from_row");

    std::vector<Cell> visual_projection;
    const std::vector<Cell>& visual_row = [&]() -> const std::vector<Cell>& {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_screen_model::append_snapshot_cells_from_row::geometry_projection");

        return row_cells_for_current_geometry(row, visual_projection);
    }();

    const int column_count = m_config.grid_size.columns;
    {
        VNM_TERMINAL_PROFILE_SCOPE(
            "Terminal_screen_model::append_snapshot_cells_from_row::scan_cells");

        for (int column = 0; column < column_count; ++column) {
            const Cell& cell = visual_row[static_cast<std::size_t>(column)];
            if (!cell.occupied) {
                continue;
            }

            snapshot.cells.push_back({
                { snapshot_row, column },
                cell.text,
                cell.hyperlink_id,
                cell.display_width,
                cell.wide_continuation,
                cell.style_id,
            });
        }
    }
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

std::optional<std::vector<Terminal_screen_model::Cell>>
Terminal_screen_model::viewport_row_cells(
    const Terminal_viewport_state& viewport,
    int                            viewport_row) const
{
    const viewport_row_t row{viewport_row};
    if (!viewport_row_is_valid(row)) {
        return std::nullopt;
    }

    if (viewport.active_buffer == Terminal_buffer_id::ALTERNATE) {
        const Terminal_screen_row* active_row =
            alternate_active_row(active_grid_row_t{row.value});
        return active_row != nullptr
            ? std::optional<std::vector<Cell>>(active_row->cells)
            : std::nullopt;
    }

    const std::optional<primary_backing_row_t> backing_row =
        primary_backing_row_from_viewport(viewport, row);
    if (!backing_row.has_value()) {
        return std::nullopt;
    }

    const std::optional<Terminal_screen_row> backing = primary_backing_row(*backing_row);
    return backing.has_value()
        ? std::optional<std::vector<Cell>>(backing->cells)
        : std::nullopt;
}

void Terminal_screen_model::append_hyperlink_metadata_for_cells(
    std::vector<Terminal_render_hyperlink_metadata>& metadata,
    const std::vector<Terminal_render_cell>&         cells,
    std::size_t                                      first_cell,
    const std::map<std::uint64_t, QByteArray>*       row_local_identity_keys) const
{
    if (first_cell >= cells.size()) {
        return;
    }

    std::set<std::uint64_t> referenced_ids;
    for (std::size_t index = first_cell; index < cells.size(); ++index) {
        const Terminal_render_cell& cell = cells[index];
        if (cell.hyperlink_id != 0U) {
            referenced_ids.insert(cell.hyperlink_id);
        }
    }

    for (std::uint64_t hyperlink_id : referenced_ids) {
        const bool already_materialized = std::any_of(
            metadata.begin(),
            metadata.end(),
            [hyperlink_id](const Terminal_render_hyperlink_metadata& candidate) {
                return candidate.hyperlink_id == hyperlink_id;
            });
        if (already_materialized) {
            continue;
        }

        const QByteArray* identity_key = nullptr;
        if (row_local_identity_keys != nullptr) {
            const auto row_local_found = row_local_identity_keys->find(hyperlink_id);
            if (row_local_found != row_local_identity_keys->end()) {
                identity_key = &row_local_found->second;
            }
        }
        else {
            identity_key = active_hyperlink_identity_key(hyperlink_id);
        }

        if (identity_key != nullptr) {
            metadata.push_back({
                hyperlink_id,
                *identity_key,
                uri_from_hyperlink_identity_key(*identity_key),
            });
        }
    }
}

}
