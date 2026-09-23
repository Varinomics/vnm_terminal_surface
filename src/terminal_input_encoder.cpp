#include "vnm_terminal/internal/terminal_input_encoder.h"

#include "vnm_terminal/internal/hierarchical_profiler.h"
#include <QKeyEvent>
#include <Qt>
#include <QtGlobal>
#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace vnm_terminal::internal {

namespace {

constexpr Qt::KeyboardModifiers k_terminal_modifier_mask =
    Qt::ShiftModifier | Qt::AltModifier | Qt::ControlModifier;

constexpr ushort k_c0_control_max = 0x001fU;
constexpr ushort k_del_control    = 0x007fU;
constexpr ushort k_c1_control_min = 0x0080U;
constexpr ushort k_c1_control_max = 0x009fU;

// Bracketed-paste delimiters, and what they cost. The budget the encoder is
// given covers the framed result, so the delimiters have to be charged before
// the body is sanitized rather than added on afterwards.
constexpr char      k_bracketed_paste_begin[]      = "\x1b[200~";
constexpr char      k_bracketed_paste_end[]        = "\x1b[201~";
constexpr qsizetype k_bracketed_paste_begin_bytes  =
    static_cast<qsizetype>(sizeof(k_bracketed_paste_begin) - 1U);
constexpr qsizetype k_bracketed_paste_end_bytes    =
    static_cast<qsizetype>(sizeof(k_bracketed_paste_end) - 1U);
constexpr qsizetype k_bracketed_paste_framing_bytes =
    k_bracketed_paste_begin_bytes + k_bracketed_paste_end_bytes;

#if defined(Q_OS_WIN)
constexpr int k_win32_right_alt_pressed    = 0x0001;
constexpr int k_win32_left_alt_pressed     = 0x0002;
constexpr int k_win32_right_ctrl_pressed   = 0x0004;
constexpr int k_win32_left_ctrl_pressed    = 0x0008;
constexpr int k_win32_shift_pressed       = 0x0010;
constexpr int k_win32_numlock_on          = 0x0020;
constexpr int k_win32_scrolllock_on       = 0x0040;
constexpr int k_win32_capslock_on         = 0x0080;
constexpr int k_win32_enhanced_key        = 0x0100;
constexpr int k_win32_lock_state_mask     =
    k_win32_numlock_on | k_win32_scrolllock_on | k_win32_capslock_on;

// QWindowsKeyMapper's nativeModifiers layout in Qt 6.11. Keep this mapping
// local to events that actually carry native metadata; synthetic events use
// their Qt modifier flags and never sample global keyboard state.
constexpr quint32 k_qt_win_shift_left      = 0x00000001U;
constexpr quint32 k_qt_win_control_left    = 0x00000002U;
constexpr quint32 k_qt_win_alt_left        = 0x00000004U;
constexpr quint32 k_qt_win_shift_right     = 0x00000010U;
constexpr quint32 k_qt_win_control_right   = 0x00000020U;
constexpr quint32 k_qt_win_alt_right       = 0x00000040U;
constexpr quint32 k_qt_win_caps_lock       = 0x00000100U;
constexpr quint32 k_qt_win_num_lock        = 0x00000200U;
constexpr quint32 k_qt_win_scroll_lock     = 0x00000400U;
constexpr quint32 k_qt_win_extended_key    = 0x01000000U;
#endif

// Stops once the sanitized text is longer than `stop_beyond_units`, so a caller
// that is only going to refuse an over-budget paste does not pay for copying all
// of it first. One unit past the budget is enough to keep the answer the same:
// the encoded form is never shorter than the code-unit count it comes from, so
// a byte check against that same budget still refuses the truncated result.
// This is a budget for the paste body alone; encode_terminal_paste_text() has
// already deducted whatever framing it is going to add, which is what makes the
// truncated result over the caller's own budget rather than merely over this
// one.
QString sanitize_paste_text(QString text, qsizetype stop_beyond_units)
{
    QString sanitized;
    sanitized.reserve(std::min(text.size(), stop_beyond_units));

    for (qsizetype i = 0; i < text.size() && sanitized.size() <= stop_beyond_units; ++i) {
        const QChar  ch   = text.at(i);
        const ushort code = ch.unicode();
        // Every line break becomes one carriage return, which is what a
        // terminal sends for Enter. A line feed is not a weaker spelling of
        // it: the Windows console input parser resolves an unrecognised C0
        // byte through VkKeyScanW(), and VkKeyScanW(0x0a) answers VK_RETURN
        // with the control modifier, so a pasted line feed arrives at the
        // child as Ctrl+Enter. PSReadLine binds Ctrl+Enter to InsertLineAbove,
        // which put each pasted line above the one before it and ran the
        // pasted sequence backwards; cmd.exe has no line-editor binding for it
        // and merged the whole paste into a single command line. Shift+Enter
        // taught the same lesson for a single key, so Windows uses a native
        // Shift+Return operation while preserving the established VT byte.
        if (code == u'\r' || code == u'\n') {
            sanitized.append(QChar(u'\r'));
            if (code == u'\r' &&
                i + 1 < text.size() &&
                text.at(i + 1) == QChar(u'\n'))
            {
                ++i;
            }
            continue;
        }

        if (code == u'\t') {
            sanitized.append(ch);
            continue;
        }

        if (code <= k_c0_control_max ||
            code == k_del_control    ||
            (code >= k_c1_control_min && code <= k_c1_control_max))
        {
            continue;
        }

        sanitized.append(ch);
    }

    return sanitized;
}

bool should_frame_paste(
    Terminal_input_mode_state      modes,
    Terminal_paste_framing_policy  framing_policy)
{
    switch (framing_policy) {
        case Terminal_paste_framing_policy::DISABLED:               return false;
        case Terminal_paste_framing_policy::APPLICATION_CONTROLLED: return modes.bracketed_paste;
        case Terminal_paste_framing_policy::ENABLED:                return true;
    }

    Q_UNREACHABLE();
    return false;
}

QByteArray ss3(char final_byte)
{
    QByteArray bytes = QByteArrayLiteral("\x1bO");
    bytes.append(final_byte);
    return bytes;
}

QByteArray csi_final(int first_parameter, int modifier_parameter, char final_byte)
{
    QByteArray bytes = QByteArrayLiteral("\x1b[");
    bytes.append(QByteArray::number(first_parameter));
    bytes.append(';');
    bytes.append(QByteArray::number(modifier_parameter));
    bytes.append(final_byte);
    return bytes;
}

QByteArray csi_tilde(int first_parameter, int modifier_parameter = 0)
{
    QByteArray bytes = QByteArrayLiteral("\x1b[");
    bytes.append(QByteArray::number(first_parameter));
    if (modifier_parameter > 0) {
        bytes.append(';');
        bytes.append(QByteArray::number(modifier_parameter));
    }
    bytes.append('~');
    return bytes;
}

enum class Windows_native_input_operation
{
    WINDOWS_OP_NONE,
    WINDOWS_OP_NATIVE_KEY_STROKES,
    WINDOWS_OP_ESCAPE_EQUIVALENT,
    WINDOWS_OP_SHIFT_RETURN,
    WINDOWS_OP_COMMITTED_TEXT,
    WINDOWS_OP_ALT_TEXT,
    WINDOWS_OP_NATIVE_ALT_KEY_TEXT,
    WINDOWS_OP_VT_SEQUENCE,
};

struct Encoded_key_event
{
    QByteArray                       bytes;
    Windows_native_input_operation  windows_operation =
        Windows_native_input_operation::WINDOWS_OP_NONE;
};

#if defined(Q_OS_WIN)
QByteArray win32_input_key_event_bytes(
    int  virtual_key,
    int  scan_code,
    int  unicode_character,
    int  key_down,
    int  control_key_state,
    int  repeat_count)
{
    QByteArray bytes = QByteArrayLiteral("\x1b[");
    bytes.append(QByteArray::number(virtual_key));
    bytes.append(';');
    bytes.append(QByteArray::number(scan_code));
    bytes.append(';');
    bytes.append(QByteArray::number(unicode_character));
    bytes.append(';');
    bytes.append(QByteArray::number(key_down));
    bytes.append(';');
    bytes.append(QByteArray::number(control_key_state));
    bytes.append(';');
    bytes.append(QByteArray::number(repeat_count));
    bytes.append('_');
    return bytes;
}

QByteArray win32_key_stroke_bytes(
    int virtual_key, int scan_code, int unicode_character, int control_key_state)
{
    return win32_input_key_event_bytes(
            virtual_key, scan_code, unicode_character, 1, control_key_state, 1) +
        win32_input_key_event_bytes(
            virtual_key, scan_code, 0, 0, control_key_state, 1);
}

int windows_key_scan_code(const QKeyEvent& event, int virtual_key)
{
    // Qt's Windows mapper stores the E0 prefix in the high byte. Console
    // records carry the scan byte and ENHANCED_KEY separately.
    if (event.nativeScanCode() != 0U) {
        return static_cast<int>(event.nativeScanCode() & 0xffU);
    }
    if (virtual_key == VK_PACKET) {
        return 0;
    }
    return static_cast<int>(MapVirtualKeyW(
        static_cast<UINT>(virtual_key), MAPVK_VK_TO_VSC) & 0xffU);
}

bool windows_key_uses_enhanced_flag(const QKeyEvent& event, int virtual_key)
{
    const bool native_extended =
        (event.nativeModifiers() & k_qt_win_extended_key) != 0U;
    const bool scan_extended = event.nativeScanCode() != 0U &&
        (event.nativeScanCode() & 0xff00U) == 0xe000U;
    if (event.nativeModifiers() != 0U || event.nativeScanCode() != 0U) {
        return native_extended || scan_extended;
    }

    const bool keypad = (event.modifiers() & Qt::KeypadModifier) != Qt::NoModifier;
    switch (virtual_key) {
        case VK_RETURN:
            return event.key() == Qt::Key_Enter || keypad;
        case VK_DIVIDE:
            return true;
        case VK_PRIOR:
        case VK_NEXT:
        case VK_END:
        case VK_HOME:
        case VK_LEFT:
        case VK_UP:
        case VK_RIGHT:
        case VK_DOWN:
        case VK_INSERT:
        case VK_DELETE:
            return !keypad;
        default:
            return false;
    }
}

int windows_control_key_state(const QKeyEvent& event, int virtual_key)
{
    int control_key_state = 0;
    const quint32 native_modifiers = event.nativeModifiers();
    if (native_modifiers != 0U) {
        if ((native_modifiers & (k_qt_win_shift_left | k_qt_win_shift_right)) != 0U) {
            control_key_state |= k_win32_shift_pressed;
        }
        if ((native_modifiers & k_qt_win_control_left) != 0U) {
            control_key_state |= k_win32_left_ctrl_pressed;
        }
        if ((native_modifiers & k_qt_win_control_right) != 0U) {
            control_key_state |= k_win32_right_ctrl_pressed;
        }
        if ((native_modifiers & k_qt_win_alt_left) != 0U) {
            control_key_state |= k_win32_left_alt_pressed;
        }
        if ((native_modifiers & k_qt_win_alt_right) != 0U) {
            control_key_state |= k_win32_right_alt_pressed;
        }
        if ((native_modifiers & k_qt_win_caps_lock) != 0U) {
            control_key_state |= k_win32_capslock_on;
        }
        if ((native_modifiers & k_qt_win_num_lock) != 0U) {
            control_key_state |= k_win32_numlock_on;
        }
        if ((native_modifiers & k_qt_win_scroll_lock) != 0U) {
            control_key_state |= k_win32_scrolllock_on;
        }
    }
    else {
        const Qt::KeyboardModifiers modifiers = event.modifiers();
        if ((modifiers & Qt::ShiftModifier) != Qt::NoModifier) {
            control_key_state |= k_win32_shift_pressed;
        }
        if ((modifiers & Qt::AltModifier) != Qt::NoModifier) {
            control_key_state |= k_win32_left_alt_pressed;
        }
        if ((modifiers & Qt::GroupSwitchModifier) != Qt::NoModifier) {
            control_key_state |= k_win32_right_alt_pressed;
        }
        if ((modifiers & Qt::ControlModifier) != Qt::NoModifier) {
            control_key_state |= k_win32_left_ctrl_pressed;
        }
    }

    if (event.key() == Qt::Key_Backtab) {
        control_key_state |= k_win32_shift_pressed;
    }
    if (windows_key_uses_enhanced_flag(event, virtual_key)) {
        control_key_state |= k_win32_enhanced_key;
    }
    return control_key_state;
}

QByteArray win32_escape_strokes(const QKeyEvent& event)
{
    const QByteArray escape = win32_key_stroke_bytes(VK_ESCAPE, 1, VK_ESCAPE, 0);
    if ((event.modifiers() & Qt::AltModifier) != Qt::NoModifier) {
        return escape + escape;
    }
    return escape;
}

bool windows_alt_chord_uses_vt_sequence_input(const QKeyEvent& event)
{
#if defined(Q_OS_WIN)
    const Qt::KeyboardModifiers modifiers = event.modifiers();
    return (modifiers & Qt::GroupSwitchModifier) != Qt::NoModifier ||
        ((modifiers & Qt::AltModifier) != Qt::NoModifier &&
            (event.nativeModifiers() & k_qt_win_alt_right) != 0U);
#else
    static_cast<void>(event);
    return false;
#endif
}

int windows_virtual_key(const QKeyEvent& event)
{
    if (event.nativeVirtualKey() != 0U) {
        return static_cast<int>(event.nativeVirtualKey());
    }

    const int key = event.key();
    if ((event.modifiers() & Qt::KeypadModifier) != Qt::NoModifier) {
        if (key >= Qt::Key_0 && key <= Qt::Key_9) {
            return VK_NUMPAD0 + key - Qt::Key_0;
        }
        switch (key) {
            case Qt::Key_Period:  return VK_DECIMAL;
            case Qt::Key_Minus:   return VK_SUBTRACT;
            case Qt::Key_Comma:   return VK_SEPARATOR;
            case Qt::Key_Plus:    return VK_ADD;
            case Qt::Key_Asterisk: return VK_MULTIPLY;
            case Qt::Key_Slash:   return VK_DIVIDE;
            case Qt::Key_Equal:   return VK_OEM_NEC_EQUAL;
            default:              break;
        }
    }

    if ((key >= Qt::Key_A && key <= Qt::Key_Z) ||
        (key >= Qt::Key_0 && key <= Qt::Key_9))
    {
        return key;
    }

    switch (key) {
        case Qt::Key_Backspace:    return VK_BACK;
        case Qt::Key_Tab:
        case Qt::Key_Backtab:      return VK_TAB;
        case Qt::Key_Return:
        case Qt::Key_Enter:        return VK_RETURN;
        case Qt::Key_Escape:       return VK_ESCAPE;
        case Qt::Key_Space:        return VK_SPACE;
        case Qt::Key_PageUp:       return VK_PRIOR;
        case Qt::Key_PageDown:     return VK_NEXT;
        case Qt::Key_End:          return VK_END;
        case Qt::Key_Home:         return VK_HOME;
        case Qt::Key_Left:         return VK_LEFT;
        case Qt::Key_Up:           return VK_UP;
        case Qt::Key_Right:        return VK_RIGHT;
        case Qt::Key_Down:         return VK_DOWN;
        case Qt::Key_Insert:       return VK_INSERT;
        case Qt::Key_Delete:       return VK_DELETE;
        case Qt::Key_BracketLeft:  return VK_OEM_4;
        case Qt::Key_Backslash:    return VK_OEM_5;
        case Qt::Key_BracketRight: return VK_OEM_6;
        case Qt::Key_Semicolon:    return VK_OEM_1;
        case Qt::Key_Apostrophe:   return VK_OEM_7;
        case Qt::Key_Comma:        return VK_OEM_COMMA;
        case Qt::Key_Period:       return VK_OEM_PERIOD;
        case Qt::Key_Slash:        return VK_OEM_2;
        case Qt::Key_QuoteLeft:    return VK_OEM_3;
        case Qt::Key_Minus:        return VK_OEM_MINUS;
        case Qt::Key_Underscore:   return VK_OEM_MINUS;
        case Qt::Key_Equal:        return VK_OEM_PLUS;
        default:
            if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
                return VK_F1 + key - Qt::Key_F1;
            }
            return event.text().isEmpty() ? 0 : VK_PACKET;
    }
}

QByteArray win32_key_event_strokes(const QKeyEvent& event)
{
    const int virtual_key = windows_virtual_key(event);
    if (virtual_key == 0) {
        return {};
    }

    int control_key_state = windows_control_key_state(event, virtual_key);
    QString text = event.text();
    if (text.isEmpty()) {
        if (virtual_key == VK_BACK) {
            text.append((control_key_state &
                (k_win32_left_ctrl_pressed | k_win32_right_ctrl_pressed)) != 0
                    ? QChar(u'\b') : QChar(u'\x7f'));
        }
        else if (virtual_key == VK_TAB) {
            text.append(QChar(u'\t'));
        }
        else if (virtual_key == VK_RETURN) {
            text.append(QChar(u'\r'));
        }
        else if (virtual_key == VK_ESCAPE) {
            text.append(QChar(u'\x1b'));
        }
    }

    if (text.isEmpty()) {
        return win32_key_stroke_bytes(
            virtual_key,
            windows_key_scan_code(event, virtual_key),
            0,
            control_key_state);
    }

    QByteArray bytes;
    const bool packet_payload = text.size() > 1 || virtual_key == VK_PACKET;
    for (qsizetype index = 0; index < text.size(); ++index) {
        const int unit_virtual_key = packet_payload ? VK_PACKET : virtual_key;
        const int unit_scan_code = packet_payload
            ? 0
            : windows_key_scan_code(event, virtual_key);
        bytes += win32_key_stroke_bytes(
            unit_virtual_key,
            unit_scan_code,
            text.at(index).unicode(),
            control_key_state);
    }
    return bytes;
}

QByteArray win32_committed_text_strokes(const QKeyEvent& event)
{
    const QString text = event.text();
    if (text.isEmpty()) {
        return {};
    }

    const int control_key_state =
        windows_control_key_state(event, VK_PACKET) & k_win32_lock_state_mask;
    QByteArray bytes;
    for (const QChar character : text) {
        bytes += win32_key_stroke_bytes(
            VK_PACKET,
            0,
            character.unicode(),
            control_key_state);
    }
    return bytes;
}

QByteArray win32_alt_text_strokes(const QKeyEvent& event)
{
    const QByteArray committed_text = win32_committed_text_strokes(event);
    if (committed_text.isEmpty()) {
        return {};
    }

    // ConPTY emits VK_PACKET Unicode before applying its Alt-prefix handling.
    // Send one explicit Escape before committed text, regardless of its length.
    return win32_key_stroke_bytes(VK_ESCAPE, 1, VK_ESCAPE, 0) + committed_text;
}

QByteArray win32_native_alt_key_text_strokes(const QKeyEvent& event)
{
    const QByteArray native_key = win32_key_event_strokes(event);
    if (native_key.isEmpty()) {
        return {};
    }

    // ConPTY does not apply the VT Alt prefix to a native right-Alt-only
    // record. An explicit Escape restores VT semantics while keeping that
    // record's native identity intact. When left Alt is also held, ConPTY
    // supplies the prefix itself, so avoid sending a duplicate Escape.
    // Classic readers observe the explicit Escape as a separate key; the
    // right-Alt-only policy deliberately prioritizes VT ESC+text semantics.
    if ((event.nativeModifiers() & k_qt_win_alt_left) != 0U) {
        return native_key;
    }
    return win32_key_stroke_bytes(VK_ESCAPE, 1, VK_ESCAPE, 0) + native_key;
}

QByteArray win32_vt_sequence_strokes(const QByteArray& bytes)
{
    // Packet strokes preserve the computed VT bytes without exposing an ESC
    // prefix to ConPTY's native-input parser as a partial Win32 frame.
    QByteArray strokes;
    for (qsizetype index = 0; index < bytes.size(); ++index) {
        strokes += win32_key_stroke_bytes(
            VK_PACKET, 0, static_cast<unsigned char>(bytes.at(index)), 0);
    }
    return strokes;
}
#endif

int mouse_modifier_bits(Qt::KeyboardModifiers modifiers)
{
    int bits = 0;
    if ((modifiers & Qt::ShiftModifier)   != Qt::NoModifier) { bits += 4;  }
    if ((modifiers & Qt::AltModifier)     != Qt::NoModifier) { bits += 8;  }
    if ((modifiers & Qt::ControlModifier) != Qt::NoModifier) { bits += 16; }
    return bits;
}

int mouse_button_code(Terminal_mouse_button button)
{
    switch (button) {
        case Terminal_mouse_button::LEFT:       return 0;
        case Terminal_mouse_button::MIDDLE:     return 1;
        case Terminal_mouse_button::RIGHT:      return 2;
        case Terminal_mouse_button::NONE:       return 3;
        case Terminal_mouse_button::WHEEL_UP:   return 64;
        case Terminal_mouse_button::WHEEL_DOWN: return 65;
    }

    Q_UNREACHABLE();
    return 3;
}

bool is_reportable_button(Terminal_mouse_button button)
{
    return
        button == Terminal_mouse_button::LEFT   ||
        button == Terminal_mouse_button::MIDDLE ||
        button == Terminal_mouse_button::RIGHT;
}

QByteArray sgr_mouse_report(const Terminal_mouse_event& event, int button_code, char final_byte)
{
    QByteArray bytes = QByteArrayLiteral("\x1b[<");
    bytes.append(QByteArray::number(button_code + mouse_modifier_bits(event.modifiers)));
    bytes.append(';');
    bytes.append(QByteArray::number(event.column + 1));
    bytes.append(';');
    bytes.append(QByteArray::number(event.row + 1));
    bytes.append(final_byte);
    return bytes;
}

Qt::KeyboardModifiers terminal_modifiers(const QKeyEvent& event)
{
    return event.modifiers() & k_terminal_modifier_mask;
}

bool has_terminal_modifiers(const QKeyEvent& event)
{
    return terminal_modifiers(event) != Qt::NoModifier;
}

int modifier_parameter(const QKeyEvent& event)
{
    int parameter = 1;
    const Qt::KeyboardModifiers modifiers = event.modifiers();
    if (event.key() == Qt::Key_Backtab ||
        (modifiers & Qt::ShiftModifier) != Qt::NoModifier)
    {
        parameter += 1;
    }
    if ((modifiers & Qt::AltModifier)     != Qt::NoModifier) { parameter += 2; }
    if ((modifiers & Qt::ControlModifier) != Qt::NoModifier) { parameter += 4; }
    return parameter;
}

QByteArray alt_prefixed(QByteArray bytes, const QKeyEvent& event)
{
    if (!bytes.isEmpty() && (event.modifiers() & Qt::AltModifier) != Qt::NoModifier) {
        bytes.prepend('\x1b');
    }
    return bytes;
}

QByteArray control_key_bytes(const QKeyEvent& event)
{
    if ((event.modifiers() & Qt::ControlModifier) == Qt::NoModifier) {
        return {};
    }

    const int key = event.key();
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return QByteArray(1, static_cast<char>(key - Qt::Key_A + 1));
    }

    switch (key) {
        case Qt::Key_At:
        case Qt::Key_2:
        case Qt::Key_QuoteLeft:
        case Qt::Key_Space:        return QByteArray(1, '\0');
        case Qt::Key_3:
        case Qt::Key_BracketLeft:  return QByteArray(1, '\x1b');
        case Qt::Key_4:
        case Qt::Key_Backslash:    return QByteArray(1, '\x1c');
        case Qt::Key_5:
        case Qt::Key_BracketRight: return QByteArray(1, '\x1d');
        case Qt::Key_6:
        case Qt::Key_AsciiCircum:  return QByteArray(1, '\x1e');
        case Qt::Key_7:
        case Qt::Key_Minus:
        case Qt::Key_Slash:
        case Qt::Key_Underscore:   return QByteArray(1, '\x1f');
        case Qt::Key_8:
        case Qt::Key_Question:     return QByteArray(1, '\x7f');
        default:                   return {};
    }
}

QByteArray special_key_bytes(const QKeyEvent& event)
{
    switch (event.key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (terminal_modifiers(event) == Qt::ShiftModifier) {
#if defined(Q_OS_WIN)
                // The native operation below carries Shift while preserving
                // the established Windows VT-input carriage-return byte.
                return QByteArray(1, '\r');
#else
                return QByteArray(1, '\n');
#endif
            }
#if defined(Q_OS_WIN)
            if (terminal_modifiers(event) ==
                (Qt::ControlModifier | Qt::AltModifier))
            {
                // The native VK_RETURN path preserves the key record, and this
                // selector fallback matches ConPTY's measured ESC+LF result.
                return alt_prefixed(QByteArray(1, '\n'), event);
            }
#endif
            return alt_prefixed(QByteArray(1, '\r'), event);
        case Qt::Key_Tab:
            if ((event.modifiers() & Qt::ShiftModifier) != Qt::NoModifier) {
                return {};
            }
            return alt_prefixed(QByteArray(1, '\t'), event);
        case Qt::Key_Backspace:
            if ((event.modifiers() & Qt::ControlModifier) != Qt::NoModifier) {
                return alt_prefixed(QByteArray(1, '\b'), event);
            }
            return alt_prefixed(QByteArray(1, '\x7f'), event);
        case Qt::Key_Escape:
            return alt_prefixed(QByteArray(1, '\x1b'), event);
        default:
            return {};
    }
}

QByteArray arrow_key_bytes(const QKeyEvent& event, bool application_cursor_keys)
{
    char final_byte = '\0';
    switch (event.key()) {
        case Qt::Key_Up:    final_byte = 'A'; break;
        case Qt::Key_Down:  final_byte = 'B'; break;
        case Qt::Key_Right: final_byte = 'C'; break;
        case Qt::Key_Left:  final_byte = 'D'; break;
        default:            return {};
    }

    if (has_terminal_modifiers(event)) {
        return csi_final(1, modifier_parameter(event), final_byte);
    }

    if (application_cursor_keys) {
        return ss3(final_byte);
    }
    return QByteArrayLiteral("\x1b[") + final_byte;
}

QByteArray navigation_key_bytes(const QKeyEvent& event, bool application_cursor_keys)
{
    if (event.key()                                 == Qt::Key_Backtab ||
        (event.key() == Qt::Key_Tab &&
         (event.modifiers() & Qt::ShiftModifier) != Qt::NoModifier))
    {
        const Qt::KeyboardModifiers modifiers = terminal_modifiers(event);
        if (modifiers == Qt::NoModifier ||
            modifiers == Qt::ShiftModifier)
        {
            return QByteArrayLiteral("\x1b[Z");
        }
        return csi_final(1, modifier_parameter(event), 'Z');
    }

    switch (event.key()) {
        case Qt::Key_Home:
            if (has_terminal_modifiers(event)) {
                return csi_final(1, modifier_parameter(event), 'H');
            }
            return application_cursor_keys ? ss3('H') : QByteArrayLiteral("\x1b[H");
        case Qt::Key_End:
            if (has_terminal_modifiers(event)) {
                return csi_final(1, modifier_parameter(event), 'F');
            }
            return application_cursor_keys ? ss3('F') : QByteArrayLiteral("\x1b[F");
        case Qt::Key_Insert:
            return has_terminal_modifiers(event)
                ? csi_tilde(2, modifier_parameter(event))
                : csi_tilde(2);
        case Qt::Key_Delete:
            return has_terminal_modifiers(event)
                ? csi_tilde(3, modifier_parameter(event))
                : csi_tilde(3);
        case Qt::Key_PageUp:
            return has_terminal_modifiers(event)
                ? csi_tilde(5, modifier_parameter(event))
                : csi_tilde(5);
        case Qt::Key_PageDown:
            return has_terminal_modifiers(event)
                ? csi_tilde(6, modifier_parameter(event))
                : csi_tilde(6);
        default:
            return {};
    }
}

QByteArray function_key_bytes(const QKeyEvent& event)
{
    const int key = event.key();
    if (key < Qt::Key_F1 || key > Qt::Key_F12) {
        return {};
    }

    static constexpr std::array<char, 4> k_f1_to_f4_final_bytes = {'P', 'Q', 'R', 'S'};
    static constexpr std::array<int, 8> k_f5_to_f12_parameters = {
        15, 17, 18, 19, 20, 21, 23, 24,
    };

    if (key <= Qt::Key_F4) {
        const char final_byte =
            k_f1_to_f4_final_bytes[static_cast<std::size_t>(key - Qt::Key_F1)];
        return has_terminal_modifiers(event)
            ? csi_final(1, modifier_parameter(event), final_byte)
            : ss3(final_byte);
    }

    const int parameter =
        k_f5_to_f12_parameters[static_cast<std::size_t>(key - Qt::Key_F5)];
    return has_terminal_modifiers(event)
        ? csi_tilde(parameter, modifier_parameter(event))
        : csi_tilde(parameter);
}

bool is_application_keypad_key(int key)
{
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return true;
    }

    switch (key) {
        case Qt::Key_Period:
        case Qt::Key_Minus:
        case Qt::Key_Comma:
        case Qt::Key_Plus:
        case Qt::Key_Asterisk:
        case Qt::Key_Slash:
        case Qt::Key_Enter:
        case Qt::Key_Equal:
            return true;
        default:
            return false;
    }
}

QByteArray application_keypad_bytes(const QKeyEvent& event)
{
    if ((event.modifiers() & Qt::KeypadModifier) == Qt::NoModifier ||
        terminal_modifiers(event)                != Qt::NoModifier)
    {
        return {};
    }

    const int key = event.key();
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return ss3(static_cast<char>('p' + key - Qt::Key_0));
    }

    switch (key) {
        case Qt::Key_Period:   return ss3('n');
        case Qt::Key_Minus:    return ss3('m');
        case Qt::Key_Comma:    return ss3('l');
        case Qt::Key_Plus:     return ss3('k');
        case Qt::Key_Asterisk: return ss3('j');
        case Qt::Key_Slash:    return ss3('o');
        case Qt::Key_Enter:    return ss3('M');
        case Qt::Key_Equal:    return ss3('X');
        default:               return {};
    }
}

QByteArray printable_text_bytes(const QKeyEvent& event)
{
    const QString text = event.text();
    if (text.isEmpty()) {
        return {};
    }

    if ((event.modifiers() & Qt::ControlModifier) != Qt::NoModifier) {
        return {};
    }

    return alt_prefixed(text.toUtf8(), event);
}

QByteArray ctrl_alt_printable_text_bytes(const QKeyEvent& event)
{
    if ((event.modifiers() & Qt::ControlModifier) == Qt::NoModifier ||
        (event.modifiers() & Qt::AltModifier)     == Qt::NoModifier)
    {
        return {};
    }

    const QString text = event.text();
    if (text.isEmpty()) {
        return {};
    }

    const QChar first_character = text.front();
    if (first_character.unicode() < 0x20U || first_character.unicode() == 0x7fU) {
        return {};
    }

    return text.toUtf8();
}

Encoded_key_event encode_terminal_key_event_bytes(
    const QKeyEvent&           event,
    Terminal_input_mode_state  modes)
{
    if (event.type() != QEvent::KeyPress) {
        return {};
    }
    const bool preserve_alt_vt_bytes = windows_alt_chord_uses_vt_sequence_input(event);

    if (modes.application_keypad) {
        const QByteArray keypad_bytes = application_keypad_bytes(event);
        if (!keypad_bytes.isEmpty()) {
            // Keep application-keypad Equal's SS3 X VT sequence intact through
            // ConPTY; Right-Alt keypad operations use the same byte-preserving
            // path. Classic readers intentionally receive VK_PACKET text.
            return {keypad_bytes,
                event.key() == Qt::Key_Equal || preserve_alt_vt_bytes
                    ? Windows_native_input_operation::WINDOWS_OP_VT_SEQUENCE
                    : Windows_native_input_operation::WINDOWS_OP_NATIVE_KEY_STROKES};
        }
    }

    const QByteArray navigation_bytes =
        navigation_key_bytes(event, modes.application_cursor_keys);
    if (!navigation_bytes.isEmpty()) {
        const bool modified_tab =
            (event.key() == Qt::Key_Tab || event.key() == Qt::Key_Backtab) &&
            navigation_bytes != QByteArrayLiteral("\x1b[Z");
        if (preserve_alt_vt_bytes || modified_tab)
        {
            // Preserve modified Tab and Right-Alt VT sequences through ConPTY.
            // Native key records lose these modifier distinctions there;
            // classic readers consequently receive VK_PACKET text.
            return {navigation_bytes,
                Windows_native_input_operation::WINDOWS_OP_VT_SEQUENCE};
        }
        return {navigation_bytes,
            Windows_native_input_operation::WINDOWS_OP_NATIVE_KEY_STROKES};
    }

    const QByteArray arrow_bytes = arrow_key_bytes(event, modes.application_cursor_keys);
    if (!arrow_bytes.isEmpty()) {
        return {arrow_bytes, preserve_alt_vt_bytes
            ? Windows_native_input_operation::WINDOWS_OP_VT_SEQUENCE
            : Windows_native_input_operation::WINDOWS_OP_NATIVE_KEY_STROKES};
    }

    const QByteArray function_bytes = function_key_bytes(event);
    if (!function_bytes.isEmpty()) {
        return {function_bytes, preserve_alt_vt_bytes
            ? Windows_native_input_operation::WINDOWS_OP_VT_SEQUENCE
            : Windows_native_input_operation::WINDOWS_OP_NATIVE_KEY_STROKES};
    }

    const QByteArray special_bytes = special_key_bytes(event);
    if (!special_bytes.isEmpty()) {
        if (event.key() == Qt::Key_Escape) {
            return {special_bytes, preserve_alt_vt_bytes
                ? Windows_native_input_operation::WINDOWS_OP_VT_SEQUENCE
                : Windows_native_input_operation::WINDOWS_OP_ESCAPE_EQUIVALENT};
        }
        if ((event.key() == Qt::Key_Return || event.key() == Qt::Key_Enter) &&
            terminal_modifiers(event) == Qt::ShiftModifier)
        {
            return {special_bytes, preserve_alt_vt_bytes
                ? Windows_native_input_operation::WINDOWS_OP_VT_SEQUENCE
                : Windows_native_input_operation::WINDOWS_OP_SHIFT_RETURN};
        }
        if (preserve_alt_vt_bytes) {
            return {special_bytes,
                Windows_native_input_operation::WINDOWS_OP_VT_SEQUENCE};
        }
        if ((event.modifiers() & Qt::AltModifier) != Qt::NoModifier) {
            return {special_bytes,
                Windows_native_input_operation::WINDOWS_OP_NATIVE_KEY_STROKES};
        }
        return {special_bytes};
    }

    const QByteArray ctrl_alt_printable_bytes = ctrl_alt_printable_text_bytes(event);
    if (!ctrl_alt_printable_bytes.isEmpty()) {
        return {
            ctrl_alt_printable_bytes,
            Windows_native_input_operation::WINDOWS_OP_COMMITTED_TEXT,
        };
    }

#if defined(Q_OS_WIN)
    if ((event.modifiers() & Qt::ControlModifier) != Qt::NoModifier &&
        (event.key() == Qt::Key_3 || event.key() == Qt::Key_BracketLeft))
    {
        const QByteArray control_bytes = control_key_bytes(event);
        if (preserve_alt_vt_bytes) {
            return {
                alt_prefixed(control_bytes, event),
                Windows_native_input_operation::WINDOWS_OP_VT_SEQUENCE,
            };
        }
        return {
            control_bytes,
            Windows_native_input_operation::WINDOWS_OP_ESCAPE_EQUIVALENT,
        };
    }
#endif

    const QByteArray control_bytes = control_key_bytes(event);
    if (!control_bytes.isEmpty()) {
        const QByteArray bytes = alt_prefixed(control_bytes, event);
        return {
            bytes,
            preserve_alt_vt_bytes
                ? Windows_native_input_operation::WINDOWS_OP_VT_SEQUENCE
                : (event.modifiers() & Qt::AltModifier) != Qt::NoModifier
                ? Windows_native_input_operation::WINDOWS_OP_NATIVE_KEY_STROKES
                : Windows_native_input_operation::WINDOWS_OP_NONE,
        };
    }

    const QByteArray text_bytes = printable_text_bytes(event);
    if (text_bytes.isEmpty()) {
        return {};
    }

    const bool group_switch =
        (event.modifiers() & Qt::GroupSwitchModifier) != Qt::NoModifier;
    bool native_right_alt_chord = false;
#if defined(Q_OS_WIN)
    native_right_alt_chord =
        (event.modifiers() & Qt::AltModifier) != Qt::NoModifier &&
        !group_switch &&
        event.nativeVirtualKey() != 0U &&
        event.nativeVirtualKey() != VK_PACKET &&
        event.text().size() == 1 &&
        (event.nativeModifiers() & k_qt_win_alt_right) != 0U;
#endif
    const bool ordinary_alt_text =
        (event.modifiers() & Qt::AltModifier) != Qt::NoModifier &&
        (native_right_alt_chord || event.text().size() > 1
#if defined(Q_OS_WIN)
            || windows_virtual_key(event) == VK_PACKET
#endif
        );
    if (group_switch) {
        // AltGr/GroupSwitch text is committed text, even when Qt also reports
        // AltModifier or native AltRight. Do not turn it into ESC-prefixed Alt
        // text while it passes through ConPTY's VT-input parser.
        return {
            event.text().toUtf8(),
            Windows_native_input_operation::WINDOWS_OP_COMMITTED_TEXT,
        };
    }
    if (native_right_alt_chord) {
        return {
            text_bytes,
            Windows_native_input_operation::WINDOWS_OP_NATIVE_ALT_KEY_TEXT,
        };
    }
    if (ordinary_alt_text) {
        return {text_bytes, Windows_native_input_operation::WINDOWS_OP_ALT_TEXT};
    }

    // Text-bearing QKeyEvents are represented as one native stroke per UTF-16
    // unit. The operation tag, rather than inspecting for an ESC prefix, keeps
    // terminal bytes and native key records distinct until this final choice.
    return {text_bytes, Windows_native_input_operation::WINDOWS_OP_NATIVE_KEY_STROKES};
}
}

QByteArray encode_terminal_key_event(
    const QKeyEvent&           event,
    Terminal_input_mode_state  modes)
{
    VNM_TERMINAL_PROFILE_SCOPE("encode_terminal_key_event");

    const Encoded_key_event encoded = encode_terminal_key_event_bytes(event, modes);
#if defined(Q_OS_WIN)
    switch (encoded.windows_operation) {
        case Windows_native_input_operation::WINDOWS_OP_NATIVE_KEY_STROKES:
            if (const QByteArray native_key_strokes = win32_key_event_strokes(event);
                !native_key_strokes.isEmpty())
            {
                return native_key_strokes;
            }
            break;
        case Windows_native_input_operation::WINDOWS_OP_ESCAPE_EQUIVALENT:
            return win32_escape_strokes(event);
        case Windows_native_input_operation::WINDOWS_OP_SHIFT_RETURN:
            if (const QByteArray native_key_strokes = win32_key_event_strokes(event);
                !native_key_strokes.isEmpty())
            {
                return native_key_strokes;
            }
            break;
        case Windows_native_input_operation::WINDOWS_OP_COMMITTED_TEXT:
            if (const QByteArray packet_strokes = win32_committed_text_strokes(event);
                !packet_strokes.isEmpty())
            {
                return packet_strokes;
            }
            break;
        case Windows_native_input_operation::WINDOWS_OP_ALT_TEXT:
            if (const QByteArray packet_strokes = win32_alt_text_strokes(event);
                !packet_strokes.isEmpty())
            {
                return packet_strokes;
            }
            break;
        case Windows_native_input_operation::WINDOWS_OP_NATIVE_ALT_KEY_TEXT:
            if (const QByteArray native_alt_key_strokes =
                    win32_native_alt_key_text_strokes(event);
                !native_alt_key_strokes.isEmpty())
            {
                return native_alt_key_strokes;
            }
            break;
        case Windows_native_input_operation::WINDOWS_OP_VT_SEQUENCE:
            return win32_vt_sequence_strokes(encoded.bytes);
        case Windows_native_input_operation::WINDOWS_OP_NONE:
            break;
    }
#endif
    return encoded.bytes;
}

QByteArray encode_terminal_mouse_event(
    Terminal_mouse_event       event,
    Terminal_input_mode_state  modes)
{
    VNM_TERMINAL_PROFILE_SCOPE("encode_terminal_mouse_event");

    // Mouse output uses SGR 1006. Other coordinate encodings are unsupported.
    if (!modes.sgr_mouse_encoding ||
        modes.mouse_tracking == Terminal_input_mouse_tracking_mode::NONE ||
        modes.mouse_tracking == Terminal_input_mouse_tracking_mode::X10  ||
        event.row            <  0                                        ||
        event.column         <  0)
    {
        return {};
    }

    switch (event.kind) {
        case Terminal_mouse_event_kind::PRESS:
            if (!is_reportable_button(event.button)) {
                return {};
            }
            return sgr_mouse_report(event, mouse_button_code(event.button), 'M');

        case Terminal_mouse_event_kind::RELEASE:
            if (!is_reportable_button(event.button)) {
                return {};
            }
            return sgr_mouse_report(event, mouse_button_code(event.button), 'm');

        case Terminal_mouse_event_kind::DRAG:
            if (modes.mouse_tracking != Terminal_input_mouse_tracking_mode::BUTTON_EVENT &&
                modes.mouse_tracking != Terminal_input_mouse_tracking_mode::ANY_EVENT)
            {
                return {};
            }
            if (!is_reportable_button(event.button)) {
                return {};
            }
            return sgr_mouse_report(event, mouse_button_code(event.button) + 32, 'M');

        case Terminal_mouse_event_kind::MOVE:
            if (modes.mouse_tracking != Terminal_input_mouse_tracking_mode::ANY_EVENT) {
                return {};
            }
            return sgr_mouse_report(event, 35, 'M');

        case Terminal_mouse_event_kind::WHEEL:
            if (event.button != Terminal_mouse_button::WHEEL_UP &&
                event.button != Terminal_mouse_button::WHEEL_DOWN)
            {
                return {};
            }
            return sgr_mouse_report(event, mouse_button_code(event.button), 'M');
    }

    Q_UNREACHABLE();
    return {};
}

QByteArray encode_terminal_paste_text(
    QString                        text,
    Terminal_input_mode_state      modes,
    Terminal_paste_framing_policy  framing_policy,
    qsizetype                      reject_beyond_bytes)
{
    VNM_TERMINAL_PROFILE_SCOPE("encode_terminal_paste_text");

    const bool frame = should_frame_paste(modes, framing_policy);
    // Clamped once before the subtraction so a negative budget cannot underflow
    // it, and once after so the deduction cannot take it below zero. The
    // unlimited budget is passed through untouched: deducting from it would
    // turn "encode everything" into a very large but finite limit.
    const qsizetype bounded_budget = std::max<qsizetype>(0, reject_beyond_bytes);
    const qsizetype framing_budget = frame ? k_bracketed_paste_framing_bytes : 0;
    const qsizetype body_budget =
        reject_beyond_bytes == std::numeric_limits<qsizetype>::max()
            ? reject_beyond_bytes
            : std::max<qsizetype>(0, bounded_budget - framing_budget);

    const QString sanitized = sanitize_paste_text(std::move(text), body_budget);
    const QByteArray body = sanitized.toUtf8();
    if (body.isEmpty() || !frame) {
        return body;
    }

    QByteArray bytes;
    bytes.reserve(k_bracketed_paste_framing_bytes + body.size());
    bytes.append(k_bracketed_paste_begin, k_bracketed_paste_begin_bytes);
    bytes.append(body);
    bytes.append(k_bracketed_paste_end, k_bracketed_paste_end_bytes);
    return bytes;
}

}
