#include "vnm_terminal/internal/terminal_screen_model.h"

#include "vnm_terminal/internal/csi_parameter_parsing.h"
#include "vnm_terminal/internal/hierarchical_profiler.h"
#include "vnm_terminal/internal/terminal_color_scheme.h"
#include "vnm_terminal/internal/terminal_history_row_record_codec.h"
#include "vnm_terminal/internal/terminal_history_row_traversal.h"
#include "vnm_terminal/internal/terminal_repaint_recovery.h"
#include "vnm_terminal/internal/unicode_width.h"

#include <QByteArray>
#include <QChar>
#include <QDateTime>
#include <QStringList>
#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace vnm_terminal::internal {

namespace {

constexpr std::size_t k_printable_ascii_count =
    k_printable_ascii_last - k_printable_ascii_first + 1U;
constexpr int k_resize_repaint_clear_guard_action_budget = 64;
constexpr std::size_t k_retained_history_ring_capacity_bytes = 64U * 1024U * 1024U;

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

bool is_printable_ascii_text(QStringView text)
{
    for (QChar character : text) {
        if (!is_printable_ascii(character)) {
            return false;
        }
    }

    return true;
}

Terminal_render_cell_text_category render_cell_text_category(QStringView text)
{
    if (text.isEmpty()) {
        return Terminal_render_cell_text_category::EMPTY;
    }

    unsigned int outside_printable_ascii = 0U;
    unsigned int non_ascii               = 0U;
    const qsizetype text_size            = text.size();
    const QChar* characters              = text.data();
    for (qsizetype index = 0; index < text_size; ++index) {
        const unsigned int code_unit = characters[index].unicode();
        outside_printable_ascii |= static_cast<unsigned int>(
            code_unit - k_printable_ascii_first >
                k_printable_ascii_last - k_printable_ascii_first);
        non_ascii |= code_unit;
    }

    if (outside_printable_ascii == 0U) {
        return Terminal_render_cell_text_category::PRINTABLE_ASCII;
    }

    return (non_ascii & ~0x7fU) != 0U
        ? Terminal_render_cell_text_category::NON_ASCII
        : Terminal_render_cell_text_category::OTHER_ASCII;
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
    m_color_state(make_terminal_color_state(default_color_scheme())),
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

    result_change_overrides_t overrides;
    overrides.dirty_rows_have_stable_mutation_identity =
        publication.dirty_rows_have_stable_mutation_identity;
    adopt_publication_changes(std::move(publication));
    retain_referenced_active_hyperlink_ids();

    {
        VNM_TERMINAL_PROFILE_SCOPE("Terminal_screen_model::finalize_ingest_result");
        result = finalize_result(std::move(result), overrides);
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

    result_change_overrides_t overrides;
    overrides.dirty_rows_have_stable_mutation_identity =
        publication.dirty_rows_have_stable_mutation_identity;
    adopt_publication_changes(std::move(publication));
    retain_referenced_active_hyperlink_ids();

    return finalize_result(std::move(result), overrides);
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
                if (mutation.printable_ascii_only) {
#if VNM_TERMINAL_PROFILING_ENABLED
                    if (m_profile_stats.enabled) {
                        ++m_profile_stats.print_text_calls;
                    }
#endif
                    if (!mutation.text.isEmpty()) {
                        cancel_resize_repaint_clear_guard_before_visible_clear();
                    }
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

    const auto single_count = [&](int& value) {
        if (!single_parameter(1, value)) {
            return false;
        }
        if (value < 1) {
            value = 1;
        }
        return true;
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

    m_current_style_id = intern_style(m_current_style);
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
    if (m_primary_retained_lookup_cache.invalidated() &&
        m_alternate_retained_lookup_cache.invalidated()) {
        return;
    }

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
    // Every content mutation funnels through this generation advance, so this
    // is the single place that records when the line last changed for the
    // row-timestamp tooltip.
    row.retained_line_provenance.content_stamp_ms = QDateTime::currentMSecsSinceEpoch();
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
    result_change_overrides_t resize_overrides;
    resize_overrides.terminal_content_changed = resize_terminal_content_changed;
    resize_overrides.active_buffer_changed    = resize_active_buffer_changed;
    resize_overrides.grid_reflow_changed      = resize_grid_reflow_changed;
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

Terminal_screen_model_result Terminal_screen_model::set_color_state(Terminal_color_state state)
{
    Terminal_screen_model_result result;
    clear_backing_deltas();
    clear_recovery_proposals();
    clear_dirty();

    m_color_state = std::move(state);
    mark_all_dirty();
    mark_viewport_changed();
    if (m_modes.synchronized_output) {
        collect_synchronized_changes();
    }

    return finalize_result(std::move(result));
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
    if (cell.wide_continuation ||
        cell.display_width != 1)
    {
        clear_cell_at_content(row, column);
    }

    Cell& target_cell = row.cells[static_cast<std::size_t>(column)];
    target_cell.text              = printable_ascii_cell_text(text);
    target_cell.text_category     = Terminal_render_cell_text_category::PRINTABLE_ASCII;
    target_cell.display_width     = 1;
    target_cell.wide_continuation = false;
    target_cell.occupied          = true;
    target_cell.style_id          = m_current_style_id;
    target_cell.hyperlink_id      = m_current_hyperlink_id;
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
    cell.text_category     = render_cell_text_category(QStringView(cell.text));
    cell.display_width     = display_width;
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
    if (m_profile_stats.enabled && m_printable_text_profile_depth > 0) {
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

    finalize_row_cell_mutation(screen_row, before_cells);
}

void Terminal_screen_model::delete_cells(int count)
{
    mark_terminal_content_changed();
    count = std::clamp(count, 1, m_config.grid_size.columns - m_cursor.column);
    Terminal_screen_row& screen_row = active_grid_rows()[static_cast<std::size_t>(m_cursor.row)];
    std::vector<Cell>& row = screen_row.cells;
    const std::vector<Cell> before_cells = row;

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

    finalize_row_cell_mutation(screen_row, before_cells);
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
    mark_dirty_rows(m_cursor.row, m_scroll_bottom);
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

    mark_dirty_rows(top, bottom);
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

void Terminal_screen_model::mark_dirty_rows(int first, int last)
{
    for (int row = first; row <= last; ++row) {
        mark_dirty(row);
    }
}

void Terminal_screen_model::mark_terminal_content_changed()
{
    if (m_primary_repaint_recovery_candidate.active &&
        m_active_buffer_id == Terminal_buffer_id::PRIMARY)
    {
        m_primary_repaint_recovery_candidate.visible_row_identity_ambiguous = true;
    }

    invalidate_retained_lookup_caches();
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

void Terminal_screen_model::adopt_publication_changes(ingest_publication_t&& publication)
{
    m_dirty_rows                    = std::move(publication.dirty_rows);
    m_terminal_content_changed      = publication.terminal_content_changed;
    m_active_buffer_changed         = publication.active_buffer_changed;
    m_grid_reflow_changed           = publication.grid_reflow_changed;
    m_viewport_changed              = publication.viewport_changed;
    m_mode_state_changed            = publication.mode_state_changed;
    m_mouse_reporting_mode_changed  = publication.mouse_reporting_mode_changed;
    m_alternate_scroll_mode_changed = publication.alternate_scroll_mode_changed;
}

Terminal_screen_model_result Terminal_screen_model::finalize_result(
    Terminal_screen_model_result result,
    result_change_overrides_t overrides) const
{
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
            render_cell_text_category(QStringView(cell.text)),
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
