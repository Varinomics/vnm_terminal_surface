#pragma once

#include <QGuiApplication>

namespace vnm_terminal {

// Native Qt events follow the host's macOS Control/Command swap convention.
// Use this at physical shortcut boundaries, before any prior normalization.
inline Qt::KeyboardModifiers physical_keyboard_modifiers(Qt::KeyboardModifiers modifiers)
{
#if defined(Q_OS_MACOS)
    if (!QGuiApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta)) {
        const bool qt_control = modifiers.testFlag(Qt::ControlModifier);
        const bool qt_meta    = modifiers.testFlag(Qt::MetaModifier);
        modifiers &= ~(Qt::ControlModifier | Qt::MetaModifier);
        if (qt_control) { modifiers |= Qt::MetaModifier;    }
        if (qt_meta)    { modifiers |= Qt::ControlModifier; }
    }
#endif
    return modifiers;
}

} // namespace vnm_terminal
