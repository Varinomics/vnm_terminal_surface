#include "vnm_terminal/internal/terminal_input_encoder.h"
#include "helpers/test_check.h"

#include <QCoreApplication>
#include <QKeyEvent>
#include <iostream>
#include <string>
#include <vector>
#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

bool check_bytes_equal(
    const QByteArray&  actual,
    const QByteArray&  expected,
    const std::string& message)
{
    if (actual != expected) {
        std::cerr << "FAIL: " << message
            << " expected=" << expected.toHex(' ').constData()
            << " actual="   << actual.toHex(' ').constData() << '\n';
        return false;
    }

    return true;
}

bool check_bytes_equal(const QByteArray& actual, const QByteArray& expected, const char* message)
{
    return check_bytes_equal(actual, expected, std::string(message));
}

QByteArray bytes_from_hex(const char* hex)
{
    QByteArray bytes = QByteArray::fromHex(QByteArray(hex));
#if defined(Q_OS_WIN)
    if (!bytes.startsWith(QByteArrayLiteral("\x1b["))) {
        return bytes;
    }

    // Historical expected-frame literals describe key-down records. Expand
    // only those complete W32IM fixtures to the balanced stroke contract.
    QByteArray balanced;
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const qsizetype frame_end = bytes.indexOf('_', offset);
        if (frame_end < 0) {
            return bytes;
        }
        const QByteArray frame = bytes.mid(offset, frame_end - offset + 1);
        const QList<QByteArray> fields = frame.mid(2, frame.size() - 3).split(';');
        if (fields.size() != 6 || fields[3] != QByteArrayLiteral("1")) {
            return bytes;
        }
        balanced += frame;
        balanced += QByteArrayLiteral("\x1b[") + fields[0] + ';' + fields[1] +
            ";0;0;" + fields[4] + ";1_";
        offset = frame_end + 1;
    }
    return balanced;
#else
    return bytes;
#endif
}

QByteArray framed_paste(QByteArray body)
{
    QByteArray bytes = QByteArrayLiteral("\x1b[200~");
    bytes += body;
    bytes += QByteArrayLiteral("\x1b[201~");
    return bytes;
}

QByteArray encode(
    int                                key,
    Qt::KeyboardModifiers              modifiers,
    QString                            text = {},
    term::Terminal_input_mode_state    modes = {})
{
    QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
    return term::encode_terminal_key_event(event, modes);
}

#if defined(Q_OS_WIN)
QByteArray win32_input_event(
    int virtual_key, int scan_code, int unicode_character, int key_down, int control_key_state)
{
    QByteArray bytes = QByteArrayLiteral("\x1b[");
    bytes += QByteArray::number(virtual_key);
    bytes += ';';
    bytes += QByteArray::number(scan_code);
    bytes += ';';
    bytes += QByteArray::number(unicode_character);
    bytes += ';';
    bytes += QByteArray::number(key_down);
    bytes += ';';
    bytes += QByteArray::number(control_key_state);
    bytes += ";1_";
    return bytes;
}

QByteArray win32_key_stroke(
    int virtual_key, int scan_code, int unicode_character, int control_key_state)
{
    return win32_input_event(virtual_key, scan_code, unicode_character, 1, control_key_state) +
        win32_input_event(virtual_key, scan_code, 0, 0, control_key_state);
}

QByteArray encode_native(
    int key,
    Qt::KeyboardModifiers modifiers,
    quint32 scan_code,
    quint32 virtual_key,
    quint32 native_modifiers,
    const QString& text,
    bool auto_repeat = false,
    quint16 count = 1,
    term::Terminal_input_mode_state modes = {})
{
    QKeyEvent event(
        QEvent::KeyPress,
        key,
        modifiers,
        scan_code,
        virtual_key,
        native_modifiers,
        text,
        auto_repeat,
        count);
    return term::encode_terminal_key_event(event, modes);
}
#endif

QByteArray sgr_mouse_report(int button_code, int row, int column, char final_byte)
{
    QByteArray bytes = QByteArrayLiteral("\x1b[<");
    bytes += QByteArray::number(button_code);
    bytes += ';';
    bytes += QByteArray::number(column + 1);
    bytes += ';';
    bytes += QByteArray::number(row + 1);
    bytes += final_byte;
    return bytes;
}

bool test_control_and_altgr()
{
    bool ok = true;

    ok &= check_bytes_equal(
        encode(Qt::Key_At, Qt::ControlModifier),
        bytes_from_hex("00"),
        "Ctrl+@ maps to NUL");
    ok &= check_bytes_equal(
        encode(Qt::Key_Space, Qt::ControlModifier),
        bytes_from_hex("00"),
        "Ctrl+Space maps to NUL");
    ok &= check_bytes_equal(
        encode(Qt::Key_2, Qt::ControlModifier),
        bytes_from_hex("00"),
        "Ctrl+2 maps to NUL");
    ok &= check_bytes_equal(
        encode(Qt::Key_QuoteLeft, Qt::ControlModifier),
        bytes_from_hex("00"),
        "Ctrl+` maps to NUL");
    ok &= check_bytes_equal(
        encode(Qt::Key_3, Qt::ControlModifier),
#if defined(Q_OS_WIN)
        bytes_from_hex("1b5b32373b313b32373b313b303b315f"),
        "Ctrl+3 uses native Win32 Escape framing on Windows");
#else
        bytes_from_hex("1b"),
        "Ctrl+3 maps to ESC");
#endif
    ok &= check_bytes_equal(
        encode(Qt::Key_4, Qt::ControlModifier),
        bytes_from_hex("1c"),
        "Ctrl+4 maps to FS");
    ok &= check_bytes_equal(
        encode(Qt::Key_5, Qt::ControlModifier),
        bytes_from_hex("1d"),
        "Ctrl+5 maps to GS");
    ok &= check_bytes_equal(
        encode(Qt::Key_6, Qt::ControlModifier),
        bytes_from_hex("1e"),
        "Ctrl+6 maps to RS");
    ok &= check_bytes_equal(
        encode(Qt::Key_7, Qt::ControlModifier),
        bytes_from_hex("1f"),
        "Ctrl+7 maps to US");
    ok &= check_bytes_equal(
        encode(Qt::Key_Minus, Qt::ControlModifier),
        bytes_from_hex("1f"),
        "Ctrl+- maps to US");
    ok &= check_bytes_equal(
        encode(Qt::Key_Slash, Qt::ControlModifier),
        bytes_from_hex("1f"),
        "Ctrl+/ maps to US");
    ok &= check_bytes_equal(
        encode(Qt::Key_8, Qt::ControlModifier),
        bytes_from_hex("7f"),
        "Ctrl+8 maps to DEL");
    ok &= check_bytes_equal(
        encode(Qt::Key_Question, Qt::ControlModifier),
        bytes_from_hex("7f"),
        "Ctrl+? maps to DEL");
    ok &= check_bytes_equal(
        encode(Qt::Key_Backspace, Qt::ControlModifier),
        bytes_from_hex("08"),
        "Ctrl+Backspace maps to BS");
    ok &= check_bytes_equal(
        encode(Qt::Key_K, Qt::ControlModifier),
        bytes_from_hex("0b"),
        "Ctrl+K maps to VT for shell line editing");
    ok &= check_bytes_equal(
        encode(Qt::Key_Backspace, Qt::ControlModifier | Qt::AltModifier),
#if defined(Q_OS_WIN)
        bytes_from_hex("1b5b383b31343b383b313b31303b315f"),
        "Ctrl+Alt+Backspace writes a native Win32 key event on Windows");
#else
        bytes_from_hex("1b08"),
        "Ctrl+Alt+Backspace prefixes BS with ESC");
#endif
    ok &= check_bytes_equal(
        encode(
            Qt::Key_E,
            Qt::ControlModifier | Qt::AltModifier,
            QString::fromUtf8("\xe2\x82\xac")),
#if defined(Q_OS_WIN)
        win32_key_stroke(VK_PACKET, 0, 0x20ac, 0),
        "Ctrl+Alt committed text uses a Unicode packet without ESC or chord modifiers");
#else
        bytes_from_hex("e282ac"),
        "Ctrl+Alt printable text preserves UTF-8 layout text");
#endif
    ok &= check_bytes_equal(
        encode(
            Qt::Key_C,
            Qt::ControlModifier | Qt::AltModifier,
            QString(QChar(0x0003))),
#if defined(Q_OS_WIN)
        bytes_from_hex("1b5b36373b34363b333b313b31303b315f"),
        "Ctrl+Alt platform C0 text uses a native Win32 key event on Windows");
#else
        bytes_from_hex("1b03"),
        "Ctrl+Alt platform C0 text falls through to Alt-prefixed control");
#endif
    ok &= check_bytes_equal(
        encode(Qt::Key_Left, Qt::ControlModifier | Qt::AltModifier),
#if defined(Q_OS_WIN)
        bytes_from_hex("1b5b33373b37353b303b313b3236363b315f"),
        "Ctrl+Alt navigation uses a native Win32 key event on Windows");
#else
        bytes_from_hex("1b5b313b3744"),
        "Ctrl+Alt navigation keeps modifier-aware arrow encoding");
#endif
    ok &= check_bytes_equal(
        encode(Qt::Key_F5, Qt::ControlModifier | Qt::AltModifier),
#if defined(Q_OS_WIN)
        bytes_from_hex("1b5b3131363b36333b303b313b31303b315f"),
        "Ctrl+Alt function key uses a native Win32 key event on Windows");
#else
        bytes_from_hex("1b5b31353b377e"),
        "Ctrl+Alt function key keeps modifier-aware function encoding");
#endif

    return ok;
}

bool test_cursor_and_navigation_modes()
{
    bool ok = true;

    ok &= check_bytes_equal(
        encode(Qt::Key_Return, Qt::NoModifier),
        bytes_from_hex("0d"),
        "plain Return writes CR");
    ok &= check_bytes_equal(
        encode(Qt::Key_Return, Qt::ShiftModifier),
#if defined(Q_OS_WIN)
        bytes_from_hex("1b5b31333b32383b31333b313b31363b315f"),
        "Shift+Return writes Win32 key record on Windows");
#else
        bytes_from_hex("0a"),
        "Shift+Return writes LF for multiline terminal prompts");
#endif
    ok &= check_bytes_equal(
        encode(Qt::Key_Enter, Qt::ShiftModifier | Qt::KeypadModifier),
#if defined(Q_OS_WIN)
        bytes_from_hex("1b5b31333b32383b31333b313b3237323b315f"),
        "Shift+keypad Enter writes an enhanced Win32 key record on Windows");
#else
        bytes_from_hex("0a"),
        "Shift+keypad Enter writes LF for multiline terminal prompts");
#endif
    ok &= check_bytes_equal(
        encode(Qt::Key_Return, Qt::AltModifier),
#if defined(Q_OS_WIN)
        bytes_from_hex("1b5b31333b32383b31333b313b323b315f"),
        "Alt+Return uses a native Win32 key event on Windows");
#else
        bytes_from_hex("1b0d"),
        "Alt+Return keeps ESC-prefixed CR");
#endif
    ok &= check_bytes_equal(
        encode(Qt::Key_Escape, Qt::NoModifier),
#if defined(Q_OS_WIN)
        bytes_from_hex("1b5b32373b313b32373b313b303b315f"),
        "Escape uses native Win32 framing on Windows");
#else
        bytes_from_hex("1b"),
        "Escape writes ESC");
#endif
    ok &= check_bytes_equal(
        encode(Qt::Key_Escape, Qt::AltModifier),
#if defined(Q_OS_WIN)
        bytes_from_hex("1b5b32373b313b32373b313b303b315f1b5b32373b313b32373b313b303b315f"),
        "Alt+Escape uses two native Escape key events on Windows");
#else
        bytes_from_hex("1b1b"),
        "Alt+Escape prefixes Escape with ESC");
#endif

    term::Terminal_input_mode_state modes;
    ok &= check_bytes_equal(
        encode(Qt::Key_Home, Qt::NoModifier, {}, modes),
#if defined(Q_OS_WIN)
        bytes_from_hex("1b5b33363b37313b303b313b3235363b315f"),
        "normal Home uses a native Win32 key event on Windows");
#else
        bytes_from_hex("1b5b48"),
        "normal Home writes CSI H");
#endif
    ok &= check_bytes_equal(
        encode(Qt::Key_End, Qt::NoModifier, {}, modes),
#if defined(Q_OS_WIN)
        bytes_from_hex("1b5b33353b37393b303b313b3235363b315f"),
        "normal End uses a native Win32 key event on Windows");
#else
        bytes_from_hex("1b5b46"),
        "normal End writes CSI F");
#endif

    modes.application_cursor_keys  = true;
    ok                            &= check_bytes_equal(
        encode(Qt::Key_Home, Qt::NoModifier, {}, modes),
#if defined(Q_OS_WIN)
        bytes_from_hex("1b5b33363b37313b303b313b3235363b315f"),
        "application cursor Home uses a native Win32 key event on Windows");
#else
        bytes_from_hex("1b4f48"),
        "application cursor Home writes SS3 H");
#endif
    ok                            &= check_bytes_equal(
        encode(Qt::Key_End, Qt::NoModifier, {}, modes),
#if defined(Q_OS_WIN)
        bytes_from_hex("1b5b33353b37393b303b313b3235363b315f"),
        "application cursor End uses a native Win32 key event on Windows");
#else
        bytes_from_hex("1b4f46"),
        "application cursor End writes SS3 F");
#endif
    ok                            &= check_bytes_equal(
        encode(Qt::Key_Home, Qt::ShiftModifier, {}, modes),
#if defined(Q_OS_WIN)
        bytes_from_hex("1b5b33363b37313b303b313b3237323b315f"),
        "modified Home uses a native Win32 key event on Windows");
#else
        bytes_from_hex("1b5b313b3248"),
        "modified Home keeps CSI modifier form in application cursor mode");
#endif

    ok &= check_bytes_equal(
        encode(Qt::Key_Backtab, Qt::NoModifier),
#if defined(Q_OS_WIN)
        bytes_from_hex("1b5b393b31353b393b313b31363b315f"),
        "Backtab retains reverse-tab identity without an explicit Shift modifier");
#else
        bytes_from_hex("1b5b5a"),
        "Backtab writes CSI Z");
#endif
    ok &= check_bytes_equal(
        encode(Qt::Key_Backtab, Qt::ShiftModifier),
#if defined(Q_OS_WIN)
        bytes_from_hex("1b5b393b31353b393b313b31363b315f"),
        "Shift+Backtab uses a native Win32 key event on Windows");
#else
        bytes_from_hex("1b5b5a"),
        "Shift+Backtab writes CSI Z");
#endif
    ok &= check_bytes_equal(
        encode(Qt::Key_Tab, Qt::ShiftModifier, QStringLiteral("\t")),
#if defined(Q_OS_WIN)
        bytes_from_hex("1b5b393b31353b393b313b31363b315f"),
        "Shift+Tab uses a native Win32 key event on Windows");
#else
        bytes_from_hex("1b5b5a"),
        "Shift+Tab writes CSI Z");
#endif
    ok &= check_bytes_equal(
        encode(Qt::Key_Backtab, Qt::ControlModifier, QStringLiteral("\t")),
        encode(Qt::Key_Tab,
            Qt::ShiftModifier | Qt::ControlModifier, QStringLiteral("\t")),
        "Ctrl+Backtab has the same logical modifier parameter as Ctrl+Shift+Tab");
    ok &= check_bytes_equal(
        encode(Qt::Key_Backtab, Qt::AltModifier, QStringLiteral("\t")),
        encode(Qt::Key_Tab,
            Qt::ShiftModifier | Qt::AltModifier, QStringLiteral("\t")),
        "Alt+Backtab has the same logical modifier parameter as Alt+Shift+Tab");
    ok &= check_bytes_equal(
        encode(Qt::Key_Backtab,
            Qt::ControlModifier | Qt::AltModifier, QStringLiteral("\t")),
        encode(Qt::Key_Tab,
            Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier,
            QStringLiteral("\t")),
        "Ctrl+Alt+Backtab has the same logical modifier parameter as Ctrl+Alt+Shift+Tab");
    ok &= check_bytes_equal(
        encode(Qt::Key_Tab, Qt::ShiftModifier | Qt::ControlModifier, QStringLiteral("\t")),
#if defined(Q_OS_WIN)
        win32_key_stroke(VK_PACKET, 0, 0x1b, 0) +
            win32_key_stroke(VK_PACKET, 0, '[', 0) +
            win32_key_stroke(VK_PACKET, 0, '1', 0) +
            win32_key_stroke(VK_PACKET, 0, ';', 0) +
            win32_key_stroke(VK_PACKET, 0, '6', 0) +
            win32_key_stroke(VK_PACKET, 0, 'Z', 0),
        "Ctrl+Shift+Tab carries CSI 1;6 Z through packet strokes");
#else
        bytes_from_hex("1b5b313b365a"),
        "Ctrl+Shift+Tab writes CSI 1;6 Z");
#endif
    ok &= check_bytes_equal(
        encode(Qt::Key_Tab, Qt::ShiftModifier | Qt::AltModifier, QStringLiteral("\t")),
#if defined(Q_OS_WIN)
        win32_key_stroke(VK_PACKET, 0, 0x1b, 0) +
            win32_key_stroke(VK_PACKET, 0, '[', 0) +
            win32_key_stroke(VK_PACKET, 0, '1', 0) +
            win32_key_stroke(VK_PACKET, 0, ';', 0) +
            win32_key_stroke(VK_PACKET, 0, '4', 0) +
            win32_key_stroke(VK_PACKET, 0, 'Z', 0),
        "Alt+Shift+Tab carries CSI 1;4 Z through packet strokes");
#else
        bytes_from_hex("1b5b313b345a"),
        "Alt+Shift+Tab writes CSI 1;4 Z");
#endif

    ok &= check_bytes_equal(
        encode(Qt::Key_BracketLeft, Qt::AltModifier, QStringLiteral("[")),
#if defined(Q_OS_WIN)
        win32_key_stroke(
            VK_OEM_4,
            static_cast<int>(MapVirtualKeyW(VK_OEM_4, MAPVK_VK_TO_VSC) & 0xffU),
            '[',
            LEFT_ALT_PRESSED),
        "Alt+[ uses a native Win32 key event on Windows");
#else
        bytes_from_hex("1b5b"),
        "Alt+[ uses the terminal Alt-prefix convention");
#endif
    ok &= check_bytes_equal(
        encode(Qt::Key_F3, Qt::ShiftModifier),
#if defined(Q_OS_WIN)
        bytes_from_hex("1b5b3131343b36313b303b313b31363b315f"),
        "Shift+F3 uses a native Win32 key event on Windows");
#else
        bytes_from_hex("1b5b313b3252"),
        "Shift+F3 uses the modified function-key sequence");
#endif

    return ok;
}

bool test_windows_native_scan_identity()
{
#if defined(Q_OS_WIN)
    bool ok = true;
    ok &= check_bytes_equal(
        encode_native(Qt::Key_Up, Qt::NoModifier, 0xe048U, VK_UP, 0U, {}),
        win32_key_stroke(VK_UP, 72, 0, ENHANCED_KEY),
        "Qt extended scan prefix becomes ENHANCED_KEY, not part of the scan byte");
    ok &= check_bytes_equal(
        encode_native(Qt::Key_Up, Qt::KeypadModifier, 0x48U, VK_UP, 0U, {}),
        win32_key_stroke(VK_UP, 72, 0, 0),
        "native NumLock-off keypad Up is not an enhanced navigation key");
    ok &= check_bytes_equal(
        encode(Qt::Key_Up, Qt::KeypadModifier),
        win32_key_stroke(VK_UP, MapVirtualKeyW(VK_UP, MAPVK_VK_TO_VSC), 0, 0),
        "synthetic keypad navigation retains its keypad identity");
    ok &= check_bytes_equal(
        encode_native(Qt::Key_BracketLeft, Qt::AltModifier, 0x1aU, VK_OEM_4,
            0x00000004U, QStringLiteral("[")),
        win32_key_stroke(VK_OEM_4, 0x1a, '[', LEFT_ALT_PRESSED),
        "native Alt+[ retains its physical OEM-4 scan identity");

    term::Terminal_input_mode_state modes;
    modes.application_keypad = true;
    ok &= check_bytes_equal(
        encode_native(Qt::Key_Enter, Qt::KeypadModifier, 0xe01cU, VK_RETURN,
            0U, QStringLiteral("\r"), false, 1, modes),
        win32_key_stroke(VK_RETURN, 28, '\r', ENHANCED_KEY),
        "native application keypad Enter has a scan byte and enhanced identity");
    ok &= check_bytes_equal(
        encode(Qt::Key_Enter, Qt::KeypadModifier, QStringLiteral("\r"), modes),
        win32_key_stroke(VK_RETURN, MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC), '\r',
            ENHANCED_KEY),
        "synthetic application keypad Enter is enhanced");
    ok &= check_bytes_equal(
        encode_native(Qt::Key_Enter, Qt::KeypadModifier | Qt::ShiftModifier,
            0xe01cU, VK_RETURN, 0U, QStringLiteral("\r")),
        win32_key_stroke(VK_RETURN, 28, '\r', ENHANCED_KEY | SHIFT_PRESSED),
        "already-framed Shift+keypad Enter uses the same native scan conversion");
    return ok;
#else
    return true;
#endif
}

bool test_windows_balanced_input_semantics()
{
#if defined(Q_OS_WIN)
    bool ok = true;

    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_Up,
            Qt::ControlModifier,
            0xe048U,
            VK_UP,
            0x01000220U,
            {}),
        win32_key_stroke(
            VK_UP,
            0x48,
            0,
            RIGHT_CTRL_PRESSED | NUMLOCK_ON | ENHANCED_KEY),
        "Qt 6.11 0x01000220 preserves right Ctrl, NumLock, and ExtendedKey exactly");

    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_F3,
            Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier,
            0x3dU,
            VK_F3,
            0x00000770U,
            {}),
        win32_key_stroke(
            VK_F3,
            0x3d,
            0,
            SHIFT_PRESSED | RIGHT_CTRL_PRESSED | RIGHT_ALT_PRESSED |
                CAPSLOCK_ON | NUMLOCK_ON | SCROLLLOCK_ON),
        "Qt 6.11 right-side modifiers and every lock bit reach the native state");

    const QString compressed_bmp = QString::fromUtf8("\xc3\xa9x");
    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_A,
            Qt::NoModifier,
            0x1eU,
            'A',
            0U,
            compressed_bmp,
            false,
            2),
        win32_key_stroke(VK_PACKET, 0, 0x00e9, 0) +
            win32_key_stroke(VK_PACKET, 0, 'x', 0),
        "compressed multi-unit non-packet BMP text uses ordered packets without multiplying count");

    const QString supplementary = QString::fromUtf8("\xf0\x9f\x98\x80");
    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_A,
            Qt::NoModifier,
            0x1eU,
            'A',
            0U,
            supplementary,
            false,
            2),
        win32_key_stroke(VK_PACKET, 0, 0xd83d, 0) +
            win32_key_stroke(VK_PACKET, 0, 0xde00, 0),
        "supplementary non-packet text keeps both surrogate units in one packet-only payload");

    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_unknown,
            Qt::NoModifier,
            0U,
            0U,
            0U,
            QStringLiteral("xy"),
            false,
            2),
        win32_key_stroke(VK_PACKET, 0, 'x', 0) +
            win32_key_stroke(VK_PACKET, 0, 'y', 0),
        "VK_PACKET text emits each unit once even when event.count is two");

    const QString altgr_character = QString::fromUtf8("\xe2\x82\xac");
    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_unknown,
            Qt::AltModifier | Qt::GroupSwitchModifier,
            0U,
            VK_PACKET,
            0x00000040U,
            altgr_character),
        win32_key_stroke(VK_PACKET, 0, 0x20ac, 0),
        "GroupSwitch AltGr is committed VK_PACKET text without physical Alt state");

    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_At,
            Qt::ControlModifier | Qt::AltModifier,
            0U,
            VK_PACKET,
            0x00000060U,
            QStringLiteral("@")),
        win32_key_stroke(VK_PACKET, 0, '@', 0),
        "right-Ctrl plus right-Alt committed text does not become an ESC chord");

    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_A,
            Qt::AltModifier,
            0x1eU,
            'A',
            0x00000004U,
            QStringLiteral("a")),
        win32_key_stroke('A', 0x1e, 'a', LEFT_ALT_PRESSED),
        "ordinary Alt+text remains a distinct ESC-prefixed native stroke");

    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_X,
            Qt::AltModifier,
            45U,
            'X',
            0x00000040U,
            QStringLiteral("x")),
        win32_key_stroke(VK_ESCAPE, 1, VK_ESCAPE, 0) +
            win32_key_stroke('X', 45, 'x', RIGHT_ALT_PRESSED),
        "native right-Alt+X adds an explicit VT prefix and retains VK_X identity");

    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_X,
            Qt::AltModifier,
            45U,
            'X',
            0x00000044U,
            QStringLiteral("x")),
        win32_key_stroke(
            'X', 45, 'x', LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED),
        "right-Alt+X with left Alt held relies on ConPTY's single Alt prefix");

    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_Return,
            Qt::ControlModifier | Qt::AltModifier,
            0x1cU,
            VK_RETURN,
            0x00000006U,
            QStringLiteral("\r")),
        win32_key_stroke(
            VK_RETURN, 0x1c, '\r', LEFT_CTRL_PRESSED | LEFT_ALT_PRESSED),
        "Ctrl+Alt+Return keeps its native VK_RETURN and CR character");
    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_Return,
            Qt::ControlModifier | Qt::AltModifier,
            0x1cU,
            VK_RETURN,
            0x00000006U,
            QStringLiteral("\n")),
        win32_key_stroke(
            VK_RETURN, 0x1c, '\n', LEFT_CTRL_PRESSED | LEFT_ALT_PRESSED),
        "Ctrl+Alt+Return preserves LF when Qt supplies LF text");

    const QString compressed_alt_text = QString::fromUtf8("\xc3\xa9x");
    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_A,
            Qt::AltModifier,
            0x1eU,
            'A',
            0x00000004U,
            compressed_alt_text,
            false,
            2),
        win32_key_stroke(VK_ESCAPE, 1, VK_ESCAPE, 0) +
            win32_key_stroke(VK_PACKET, 0, 0x00e9, 0) +
            win32_key_stroke(VK_PACKET, 0, 'x', 0),
        "compressed ordinary Alt text uses one Escape then all committed packet units");

    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_unknown,
            Qt::AltModifier,
            0U,
            0U,
            0x00000004U,
            QStringLiteral("x")),
        win32_key_stroke(VK_ESCAPE, 1, VK_ESCAPE, 0) +
            win32_key_stroke(VK_PACKET, 0, 'x', 0),
        "single text-only Alt input carries an explicit Escape before committed text");

    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_Tab,
            Qt::NoModifier,
            0x0fU,
            VK_TAB,
            0U,
            QStringLiteral("\t")),
        QByteArrayLiteral("\t"),
        "plain Tab stays distinct from native reverse Tab");
    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_Tab,
            Qt::ShiftModifier,
            0x0fU,
            VK_TAB,
            0x00000001U,
            QStringLiteral("\t")),
        win32_key_stroke(VK_TAB, 0x0f, '\t', SHIFT_PRESSED),
        "Shift+Tab carries Shift and retains the established reverse-Tab VT operation");
    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_Tab,
            Qt::ShiftModifier | Qt::ControlModifier,
            0x0fU,
            VK_TAB,
            0x00000003U,
            QStringLiteral("\t")),
        win32_key_stroke(VK_PACKET, 0, 0x1b, 0) +
            win32_key_stroke(VK_PACKET, 0, '[', 0) +
            win32_key_stroke(VK_PACKET, 0, '1', 0) +
            win32_key_stroke(VK_PACKET, 0, ';', 0) +
            win32_key_stroke(VK_PACKET, 0, '6', 0) +
            win32_key_stroke(VK_PACKET, 0, 'Z', 0),
        "Ctrl+Shift+Tab carries CSI 1;6 Z through packet strokes");
    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_Tab,
            Qt::ShiftModifier | Qt::AltModifier,
            0x0fU,
            VK_TAB,
            0x00000005U,
            QStringLiteral("\t")),
        win32_key_stroke(VK_PACKET, 0, 0x1b, 0) +
            win32_key_stroke(VK_PACKET, 0, '[', 0) +
            win32_key_stroke(VK_PACKET, 0, '1', 0) +
            win32_key_stroke(VK_PACKET, 0, ';', 0) +
            win32_key_stroke(VK_PACKET, 0, '4', 0) +
            win32_key_stroke(VK_PACKET, 0, 'Z', 0),
        "Alt+Shift+Tab carries CSI 1;4 Z through packet strokes");
    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_Backtab,
            Qt::NoModifier,
            0U,
            0U,
            0U,
            {}),
        win32_key_stroke(
            VK_TAB,
            MapVirtualKeyW(VK_TAB, MAPVK_VK_TO_VSC),
            '\t',
            SHIFT_PRESSED),
        "synthetic Backtab adds Shift without consulting physical key state");

    term::Terminal_input_mode_state keypad_modes;
    keypad_modes.application_keypad = true;
    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_Comma,
            Qt::KeypadModifier,
            0x53U,
            0U,
            0U,
            QStringLiteral(","),
            false,
            1,
            keypad_modes),
        win32_key_stroke(VK_SEPARATOR, 0x53, ',', 0),
        "application-keypad comma retains its existing native identity");
    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_Equal,
            Qt::KeypadModifier,
            0x59U,
            0U,
            0U,
            QStringLiteral("="),
            false,
            1,
            keypad_modes),
        win32_key_stroke(VK_PACKET, 0, 0x1b, 0) +
            win32_key_stroke(VK_PACKET, 0, 'O', 0) +
            win32_key_stroke(VK_PACKET, 0, 'X', 0),
        "application-keypad Equal uses SS3 X for VT input");
    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_Comma,
            Qt::KeypadModifier,
            0x53U,
            0U,
            0U,
            QStringLiteral(",")),
        win32_key_stroke(VK_SEPARATOR, 0x53, ',', 0),
        "classic keypad comma retains VK_SEPARATOR identity");
    ok &= check_bytes_equal(
        encode_native(
            Qt::Key_Equal,
            Qt::KeypadModifier,
            0x59U,
            0U,
            0U,
            QStringLiteral("=")),
        win32_key_stroke(VK_OEM_NEC_EQUAL, 0x59, '=', 0),
        "classic keypad Equal retains VK_OEM_NEC_EQUAL identity");

    QKeyEvent repeated_key(
        QEvent::KeyPress,
        Qt::Key_Up,
        Qt::NoModifier,
        0xe048U,
        VK_UP,
        0x01000000U,
        {},
        true,
        4);
    ok &= check_bytes_equal(
        term::encode_terminal_key_event(repeated_key, {}),
        win32_key_stroke(VK_UP, 0x48, 0, ENHANCED_KEY),
        "one compressed auto-repeat event is one logical down/up stroke");

    QKeyEvent released_key(
        QEvent::KeyRelease,
        Qt::Key_Up,
        Qt::NoModifier,
        0xe048U,
        VK_UP,
        0x01000000U,
        {});
    ok &= check_bytes_equal(
        term::encode_terminal_key_event(released_key, {}),
        {},
        "KeyRelease does not create another stroke in the press-oriented contract");

    return ok;
#else
    return true;
#endif
}

bool test_keypad_policy()
{
    bool ok = true;

    term::Terminal_input_mode_state modes;
    modes.application_keypad = true;

    ok &= check_bytes_equal(
        encode(Qt::Key_5, Qt::KeypadModifier, QStringLiteral("5"), modes),
#if defined(Q_OS_WIN)
        bytes_from_hex("1b5b3130313b37363b35333b313b303b315f"),
        "application keypad digit uses a native Win32 key event on Windows");
#else
        bytes_from_hex("1b4f75"),
        "unmodified application keypad digit uses SS3 encoding");
#endif
    ok &= check_bytes_equal(
        encode(
            Qt::Key_5,
            Qt::KeypadModifier | Qt::ShiftModifier,
            QStringLiteral("5"),
            modes),
#if defined(Q_OS_WIN)
        win32_key_stroke(
            VK_NUMPAD5,
            MapVirtualKeyW(VK_NUMPAD5, MAPVK_VK_TO_VSC),
            '5',
            SHIFT_PRESSED),
        "modified keypad digit preserves text in a balanced native key stroke");
#else
        bytes_from_hex("35"),
        "modified keypad digit falls through to printable text");
#endif
    ok &= check_bytes_equal(
        encode(
            Qt::Key_Plus,
            Qt::KeypadModifier | Qt::AltModifier,
            QStringLiteral("+"),
            modes),
#if defined(Q_OS_WIN)
        bytes_from_hex("1b5b3130373b37383b34333b313b323b315f"),
        "modified keypad operator uses a native Win32 key event on Windows");
#else
        bytes_from_hex("1b2b"),
        "modified keypad operator preserves Alt printable behavior");
#endif

    return ok;
}

bool test_paste_framing_policy()
{
    bool ok = true;

    term::Terminal_input_mode_state modes;
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            QStringLiteral("abc"),
            modes,
            term::Terminal_paste_framing_policy::DISABLED),
        QByteArrayLiteral("abc"),
        "disabled paste policy never frames");
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            QStringLiteral("abc"),
            modes,
            term::Terminal_paste_framing_policy::APPLICATION_CONTROLLED),
        QByteArrayLiteral("abc"),
        "application-controlled paste policy stays unframed without terminal mode");

    modes.bracketed_paste = true;
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            QStringLiteral("abc"),
            modes,
            term::Terminal_paste_framing_policy::APPLICATION_CONTROLLED),
        QByteArrayLiteral("\x1b[200~abc\x1b[201~"),
        "application-controlled paste policy frames when terminal mode is set");

    modes.bracketed_paste = false;
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            QStringLiteral("abc"),
            modes,
            term::Terminal_paste_framing_policy::ENABLED),
        QByteArrayLiteral("\x1b[200~abc\x1b[201~"),
        "enabled paste policy always frames");

    modes.bracketed_paste  = true;
    ok                    &= check_bytes_equal(
        term::encode_terminal_paste_text(
            {},
            modes,
            term::Terminal_paste_framing_policy::DISABLED),
        {},
        "disabled empty paste produces no bytes");
    ok                    &= check_bytes_equal(
        term::encode_terminal_paste_text(
            {},
            modes,
            term::Terminal_paste_framing_policy::APPLICATION_CONTROLLED),
        {},
        "application-controlled empty paste produces no frame");
    ok                    &= check_bytes_equal(
        term::encode_terminal_paste_text(
            {},
            modes,
            term::Terminal_paste_framing_policy::ENABLED),
        {},
        "enabled empty paste produces no frame");

    return ok;
}

bool test_paste_sanitization()
{
    bool ok = true;

    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            QStringLiteral("a\r\nb\rc\n"),
            {},
            term::Terminal_paste_framing_policy::DISABLED),
        QByteArrayLiteral("a\rb\rc\r"),
        "paste sanitization normalizes CRLF, lone CR and lone LF to CR");
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            QStringLiteral("\r"),
            {},
            term::Terminal_paste_framing_policy::DISABLED),
        QByteArrayLiteral("\r"),
        "paste sanitization normalizes trailing lone CR");
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            QStringLiteral("\r\r"),
            {},
            term::Terminal_paste_framing_policy::DISABLED),
        QByteArrayLiteral("\r\r"),
        "paste sanitization normalizes consecutive lone CRs");
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            QStringLiteral("\r\n\r\n"),
            {},
            term::Terminal_paste_framing_policy::DISABLED),
        QByteArrayLiteral("\r\r"),
        "paste sanitization normalizes consecutive CRLF pairs");
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            QStringLiteral("\n\n"),
            {},
            term::Terminal_paste_framing_policy::DISABLED),
        QByteArrayLiteral("\r\r"),
        "paste sanitization normalizes consecutive lone LFs");
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            QStringLiteral("\n\r"),
            {},
            term::Terminal_paste_framing_policy::DISABLED),
        QByteArrayLiteral("\r\r"),
        "paste sanitization keeps LF CR two line breaks");

    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            QStringLiteral(" \t\nkept  "),
            {},
            term::Terminal_paste_framing_policy::DISABLED),
        QByteArrayLiteral(" \t\rkept  "),
        "paste sanitization preserves spaces tabs and trailing spaces");

    QString controls;
    controls.append(QChar(0x0000));
    controls.append(QChar(0x0001));
    controls.append(QChar(0x001b));
    controls.append(QStringLiteral("A"));
    controls.append(QChar(0x007f));
    controls.append(QChar(0x0085));
    controls.append(QStringLiteral("B"));
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            controls,
            {},
            term::Terminal_paste_framing_policy::DISABLED),
        QByteArrayLiteral("AB"),
        "paste sanitization strips NUL ESC DEL C0 and C1 controls");

    QString control_boundaries;
    control_boundaries.append(QChar(0x0009));
    control_boundaries.append(QChar(0x000a));
    control_boundaries.append(QChar(0x000d));
    control_boundaries.append(QChar(0x001f));
    control_boundaries.append(QChar(0x0020));
    control_boundaries.append(QChar(0x007f));
    control_boundaries.append(QChar(0x009f));
    control_boundaries.append(QChar(0x00a0));
    QByteArray expected_boundaries = QByteArrayLiteral("\t\r\r ");
    expected_boundaries += QString(QChar(0x00a0U)).toUtf8();
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            control_boundaries,
            {},
            term::Terminal_paste_framing_policy::DISABLED),
        expected_boundaries,
        "paste sanitization preserves allowed boundaries and strips controls");

    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            controls,
            {},
            term::Terminal_paste_framing_policy::ENABLED),
        framed_paste(QByteArrayLiteral("AB")),
        "framed paste sanitizes the body without stripping the frame");

    QString delimiter_injection = QStringLiteral("\x1b[201~harm");
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            delimiter_injection,
            {},
            term::Terminal_paste_framing_policy::ENABLED),
        framed_paste(QByteArrayLiteral("[201~harm")),
        "framed paste strips ESC from an embedded end delimiter");

    QString c1_csi_injection;
    c1_csi_injection.append(QChar(0x009b));
    c1_csi_injection.append(QStringLiteral("201~harm"));
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            c1_csi_injection,
            {},
            term::Terminal_paste_framing_policy::ENABLED),
        framed_paste(QByteArrayLiteral("201~harm")),
        "framed paste strips C1 CSI from an embedded end delimiter");

    QString stripped_only;
    stripped_only.append(QChar(0x0000));
    stripped_only.append(QChar(0x001b));
    stripped_only.append(QChar(0x007f));
    stripped_only.append(QChar(0x009b));
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            stripped_only,
            {},
            term::Terminal_paste_framing_policy::ENABLED),
        {},
        "paste sanitization that removes the whole body produces no frame");

    const QString unicode_text = QString::fromUtf8("lambda \xce\xbb euro \xe2\x82\xac");
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            unicode_text,
            {},
            term::Terminal_paste_framing_policy::DISABLED),
        unicode_text.toUtf8(),
        "paste encoding preserves alphabetic Unicode and symbols as UTF-8");

    const QString cjk_text = QString::fromUtf8("Chinese \xe4\xb8\xad\xe6\x96\x87");
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            cjk_text,
            {},
            term::Terminal_paste_framing_policy::DISABLED),
        cjk_text.toUtf8(),
        "paste encoding keeps CJK text on the bulk UTF-8 path");

    const QString degree_text = QString::fromUtf8("23\xc2\xb0 C");
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            degree_text,
            {},
            term::Terminal_paste_framing_policy::DISABLED),
        degree_text.toUtf8(),
        "paste encoding preserves degree as UTF-8");
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            degree_text,
            {},
            term::Terminal_paste_framing_policy::ENABLED),
        framed_paste(degree_text.toUtf8()),
        "bracketed paste frames UTF-8 degree input");

    const QString non_bmp_text = QString::fromUtf8("emoji \xf0\x9f\x98\x80");
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            non_bmp_text,
            {},
            term::Terminal_paste_framing_policy::DISABLED),
        non_bmp_text.toUtf8(),
        "paste sanitization preserves non-BMP UTF-8 text");

    return ok;
}

bool test_paste_budget_stops_encoding_past_the_budget()
{
    bool ok = true;

    // A paste that fits the caller's budget must come back whole, budget or no
    // budget, because the budget only bounds work that would be thrown away.
    const QString within_budget(64, QChar(u'a'));
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            within_budget,
            {},
            term::Terminal_paste_framing_policy::DISABLED,
            64),
        within_budget.toUtf8(),
        "a paste at the budget is encoded whole");

    // Past the budget the result only has to stay past it, so the caller's own
    // byte check reaches the same refusal it would have reached anyway.
    const QString past_budget(4096, QChar(u'a'));
    const QByteArray bounded = term::encode_terminal_paste_text(
        past_budget,
        {},
        term::Terminal_paste_framing_policy::DISABLED,
        64);
    ok &= check(bounded.size() > 64,
        "a paste past the budget still encodes past the budget");
    ok &= check(bounded.size() < past_budget.size(),
        "a paste past the budget stops short of encoding all of it");

    // Framing consumes twelve bytes of the same queue budget. The encoder must
    // therefore stop just past the remaining body capacity, not copy a full
    // budget of body and add the delimiters afterwards.
    const QByteArray framed_at_budget = term::encode_terminal_paste_text(
        QStringLiteral("a"),
        {},
        term::Terminal_paste_framing_policy::ENABLED,
        13);
    ok &= check_bytes_equal(
        framed_at_budget,
        framed_paste(QByteArrayLiteral("a")),
        "a framed paste exactly at the total budget is encoded whole");

    const QByteArray bounded_framed = term::encode_terminal_paste_text(
        past_budget,
        {},
        term::Terminal_paste_framing_policy::ENABLED,
        64);
    ok &= check(bounded_framed.size() > 64,
        "a framed paste past the total budget still reaches rejection");
    ok &= check(bounded_framed.size() < bounded.size() + 12,
        "framing bytes reduce the body work budget before encoding");

    // Sanitizing removes control characters, so the budget is spent on what
    // survives sanitization rather than on what arrived.
    QString controls_then_text;
    for (int index = 0; index < 4096; ++index) {
        controls_then_text += QChar(u'\x01');
    }
    controls_then_text += QStringLiteral("tail");
    ok &= check_bytes_equal(
        term::encode_terminal_paste_text(
            controls_then_text,
            {},
            term::Terminal_paste_framing_policy::DISABLED,
            64),
        QByteArrayLiteral("tail"),
        "the budget counts sanitized units, not the units that arrived");

    return ok;
}

bool test_sgr_mouse_reporting()
{
    bool ok = true;

    term::Terminal_input_mode_state modes;
    modes.sgr_mouse_encoding = true;

    ok &= check_bytes_equal(
        term::encode_terminal_mouse_event({
            term::Terminal_mouse_event_kind::PRESS,
            term::Terminal_mouse_button::LEFT,
            3,
            4,
            Qt::NoModifier,
        }, modes),
        {},
        "mouse tracking disabled emits no bytes");

    modes.mouse_tracking = term::Terminal_input_mouse_tracking_mode::X10;
    ok &= check_bytes_equal(
        term::encode_terminal_mouse_event({
            term::Terminal_mouse_event_kind::PRESS,
            term::Terminal_mouse_button::LEFT,
            3,
            4,
            Qt::NoModifier,
        }, modes),
        {},
        "X10 mouse tracking emits no SGR bytes");

    modes.mouse_tracking      = term::Terminal_input_mouse_tracking_mode::NORMAL;
    modes.sgr_mouse_encoding  = false;
    ok                       &= check_bytes_equal(
        term::encode_terminal_mouse_event({
            term::Terminal_mouse_event_kind::PRESS,
            term::Terminal_mouse_button::LEFT,
            3,
            4,
            Qt::NoModifier,
        }, modes),
        {},
        "non-SGR mouse mode emits no bytes");

    modes.sgr_mouse_encoding  = true;
    ok                       &= check_bytes_equal(
        term::encode_terminal_mouse_event({
            term::Terminal_mouse_event_kind::PRESS,
            term::Terminal_mouse_button::LEFT,
            3,
            4,
            Qt::NoModifier,
        }, modes),
        QByteArrayLiteral("\x1b[<0;5;4M"),
        "normal SGR left press uses one-based wire coordinates");
    ok                       &= check_bytes_equal(
        term::encode_terminal_mouse_event({
            term::Terminal_mouse_event_kind::RELEASE,
            term::Terminal_mouse_button::LEFT,
            3,
            4,
            Qt::NoModifier,
        }, modes),
        QByteArrayLiteral("\x1b[<0;5;4m"),
        "normal SGR left release uses final m");
    ok                       &= check_bytes_equal(
        term::encode_terminal_mouse_event({
            term::Terminal_mouse_event_kind::PRESS,
            term::Terminal_mouse_button::RIGHT,
            0,
            0,
            Qt::ShiftModifier | Qt::AltModifier | Qt::ControlModifier,
        }, modes),
        QByteArrayLiteral("\x1b[<30;1;1M"),
        "SGR mouse modifier bits add Shift Alt Ctrl");
    ok                       &= check_bytes_equal(
        term::encode_terminal_mouse_event({
            term::Terminal_mouse_event_kind::DRAG,
            term::Terminal_mouse_button::LEFT,
            2,
            3,
            Qt::NoModifier,
        }, modes),
        {},
        "normal tracking does not report drag");

    modes.mouse_tracking = term::Terminal_input_mouse_tracking_mode::BUTTON_EVENT;
    ok &= check_bytes_equal(
        term::encode_terminal_mouse_event({
            term::Terminal_mouse_event_kind::DRAG,
            term::Terminal_mouse_button::LEFT,
            2,
            3,
            Qt::NoModifier,
        }, modes),
        QByteArrayLiteral("\x1b[<32;4;3M"),
        "button-event tracking reports drag");

    modes.mouse_tracking  = term::Terminal_input_mouse_tracking_mode::ANY_EVENT;
    ok                   &= check_bytes_equal(
        term::encode_terminal_mouse_event({
            term::Terminal_mouse_event_kind::MOVE,
            term::Terminal_mouse_button::NONE,
            1,
            2,
            Qt::NoModifier,
        }, modes),
        QByteArrayLiteral("\x1b[<35;3;2M"),
        "any-event tracking reports passive motion");
    ok                   &= check_bytes_equal(
        term::encode_terminal_mouse_event({
            term::Terminal_mouse_event_kind::WHEEL,
            term::Terminal_mouse_button::WHEEL_UP,
            0,
            0,
            Qt::NoModifier,
        }, modes),
        QByteArrayLiteral("\x1b[<64;1;1M"),
        "SGR mouse reports wheel up");
    ok                   &= check_bytes_equal(
        term::encode_terminal_mouse_event({
            term::Terminal_mouse_event_kind::WHEEL,
            term::Terminal_mouse_button::WHEEL_DOWN,
            0,
            0,
            Qt::NoModifier,
        }, modes),
        QByteArrayLiteral("\x1b[<65;1;1M"),
        "SGR mouse reports wheel down");

    return ok;
}

bool test_sgr_mouse_modifier_button_matrix()
{
    struct mouse_modifier_case_t
    {
        Qt::KeyboardModifiers  modifiers;
        int                    wire_bits = 0;
        const char*            label     = "";
    };

    struct mouse_button_case_t
    {
        term::Terminal_mouse_button button;
        int                         wire_code = 0;
        const char*                 label     = "";
    };

    static const std::vector<mouse_modifier_case_t> k_modifier_cases = {
        { Qt::NoModifier, 0, "none" },
        { Qt::ShiftModifier, 4, "shift" },
        { Qt::AltModifier, 8, "alt" },
        { Qt::ControlModifier, 16, "ctrl" },
        {
            Qt::ShiftModifier | Qt::AltModifier | Qt::ControlModifier,
            28,
            "shift-alt-ctrl",
        },
    };
    static const std::vector<mouse_button_case_t> k_button_cases = {
        { term::Terminal_mouse_button::LEFT,   0, "left"   },
        { term::Terminal_mouse_button::MIDDLE, 1, "middle" },
        { term::Terminal_mouse_button::RIGHT,  2, "right"  },
    };

    term::Terminal_input_mode_state modes;
    modes.mouse_tracking     = term::Terminal_input_mouse_tracking_mode::NORMAL;
    modes.sgr_mouse_encoding = true;

    bool ok = true;
    for (const mouse_modifier_case_t& modifier_case : k_modifier_cases) {
        for (const mouse_button_case_t& button_case : k_button_cases) {
            const int wire_code = button_case.wire_code + modifier_case.wire_bits;
            const std::string label =
                std::string("SGR mouse ") + modifier_case.label + ' ' + button_case.label;

            ok &= check_bytes_equal(
                term::encode_terminal_mouse_event({
                    term::Terminal_mouse_event_kind::PRESS,
                    button_case.button,
                    5,
                    7,
                    modifier_case.modifiers,
                }, modes),
                sgr_mouse_report(wire_code, 5, 7, 'M'),
                label + " press");
            ok &= check_bytes_equal(
                term::encode_terminal_mouse_event({
                    term::Terminal_mouse_event_kind::RELEASE,
                    button_case.button,
                    5,
                    7,
                    modifier_case.modifiers,
                }, modes),
                sgr_mouse_report(wire_code, 5, 7, 'm'),
                label + " release");
        }
    }

    return ok;
}

bool test_mouse_tracking_mode_matrix()
{
    struct mouse_tracking_case_t
    {
        term::Terminal_input_mouse_tracking_mode tracking_mode;
        term::Terminal_mouse_event_kind          event_kind;
        term::Terminal_mouse_button              button;
        bool                                     emits      = false;
        int                                      wire_code  = 0;
        char                                     final_byte = 'M';
        const char*                              label      = "";
    };

    static const std::vector<mouse_tracking_case_t> k_tracking_cases = {
        {
            term::Terminal_input_mouse_tracking_mode::NORMAL,
            term::Terminal_mouse_event_kind::PRESS,
            term::Terminal_mouse_button::LEFT,
            true,
            0,
            'M',
            "normal press",
        },
        {
            term::Terminal_input_mouse_tracking_mode::NORMAL,
            term::Terminal_mouse_event_kind::RELEASE,
            term::Terminal_mouse_button::LEFT,
            true,
            0,
            'm',
            "normal release",
        },
        {
            term::Terminal_input_mouse_tracking_mode::NORMAL,
            term::Terminal_mouse_event_kind::DRAG,
            term::Terminal_mouse_button::LEFT,
            false,
            0,
            'M',
            "normal drag",
        },
        {
            term::Terminal_input_mouse_tracking_mode::NORMAL,
            term::Terminal_mouse_event_kind::MOVE,
            term::Terminal_mouse_button::NONE,
            false,
            0,
            'M',
            "normal move",
        },
        {
            term::Terminal_input_mouse_tracking_mode::NORMAL,
            term::Terminal_mouse_event_kind::WHEEL,
            term::Terminal_mouse_button::WHEEL_UP,
            true,
            64,
            'M',
            "normal wheel",
        },
        {
            term::Terminal_input_mouse_tracking_mode::BUTTON_EVENT,
            term::Terminal_mouse_event_kind::DRAG,
            term::Terminal_mouse_button::LEFT,
            true,
            32,
            'M',
            "button-event drag",
        },
        {
            term::Terminal_input_mouse_tracking_mode::BUTTON_EVENT,
            term::Terminal_mouse_event_kind::MOVE,
            term::Terminal_mouse_button::NONE,
            false,
            0,
            'M',
            "button-event move",
        },
        {
            term::Terminal_input_mouse_tracking_mode::ANY_EVENT,
            term::Terminal_mouse_event_kind::DRAG,
            term::Terminal_mouse_button::LEFT,
            true,
            32,
            'M',
            "any-event drag",
        },
        {
            term::Terminal_input_mouse_tracking_mode::ANY_EVENT,
            term::Terminal_mouse_event_kind::MOVE,
            term::Terminal_mouse_button::NONE,
            true,
            35,
            'M',
            "any-event move",
        },
    };

    bool ok = true;
    for (const mouse_tracking_case_t& tracking_case : k_tracking_cases) {
        term::Terminal_input_mode_state modes;
        modes.mouse_tracking     = tracking_case.tracking_mode;
        modes.sgr_mouse_encoding = true;

        const QByteArray expected = tracking_case.emits
            ? sgr_mouse_report(tracking_case.wire_code, 1, 2, tracking_case.final_byte)
            : QByteArray();
        ok &= check_bytes_equal(
            term::encode_terminal_mouse_event({
                tracking_case.event_kind,
                tracking_case.button,
                1,
                2,
                Qt::NoModifier,
            }, modes),
            expected,
            std::string("SGR mouse tracking matrix ") + tracking_case.label);

        modes.sgr_mouse_encoding = false;
        ok &= check_bytes_equal(
            term::encode_terminal_mouse_event({
                tracking_case.event_kind,
                tracking_case.button,
                1,
                2,
                Qt::NoModifier,
            }, modes),
            {},
            std::string("legacy mouse encoding unsupported for ") + tracking_case.label);
    }

    return ok;
}

}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    bool ok = true;
    ok &= test_control_and_altgr();
    ok &= test_cursor_and_navigation_modes();
    ok &= test_windows_native_scan_identity();
    ok &= test_windows_balanced_input_semantics();
    ok &= test_keypad_policy();
    ok &= test_paste_framing_policy();
    ok &= test_paste_sanitization();
    ok &= test_paste_budget_stops_encoding_past_the_budget();
    ok &= test_sgr_mouse_reporting();
    ok &= test_sgr_mouse_modifier_button_matrix();
    ok &= test_mouse_tracking_mode_matrix();
    return ok ? 0 : 1;
}
