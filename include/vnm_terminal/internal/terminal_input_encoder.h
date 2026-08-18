#pragma once

#include "vnm_terminal/internal/terminal_input_mode.h"
#include <QByteArray>
#include <QString>
#include <Qt>
#include <cstddef>
#include <limits>

class QKeyEvent;

namespace vnm_terminal::internal {

enum class Terminal_paste_framing_policy
{
    DISABLED,
    APPLICATION_CONTROLLED,
    ENABLED,
};

enum class Terminal_mouse_event_kind
{
    PRESS,
    RELEASE,
    DRAG,
    MOVE,
    WHEEL,
};

enum class Terminal_mouse_button
{
    LEFT,
    MIDDLE,
    RIGHT,
    NONE,
    WHEEL_UP,
    WHEEL_DOWN,
};

struct Terminal_mouse_event
{
    Terminal_mouse_event_kind  kind      = Terminal_mouse_event_kind::MOVE;
    Terminal_mouse_button      button    = Terminal_mouse_button::NONE;
    int                        row       = 0;
    int                        column    = 0;
    Qt::KeyboardModifiers      modifiers = Qt::NoModifier;
};

QByteArray encode_terminal_key_event(
    const QKeyEvent&               event,
    Terminal_input_mode_state      modes);

QByteArray encode_terminal_mouse_event(
    Terminal_mouse_event           event,
    Terminal_input_mode_state      modes);

// `reject_beyond_bytes` is the caller's byte budget for the encoded result, and
// it bounds the work rather than the outcome. Sanitizing, encoding and framing
// each copy the whole paste, and a caller that is going to refuse the result
// anyway gains nothing from completing those copies for a clipboard far past
// its budget. Encoding therefore stops once the sanitized body is longer than
// the body budget, and what comes back is then deliberately over the caller's
// total budget: UTF-8 is never shorter than the UTF-16 code-unit count it
// encodes, and bracketed-paste delimiters are charged before sanitization. The
// caller's own limit check therefore still refuses the result for the same
// reason and with the same message it would have used for complete encoding.
// Pass the default to encode everything.
QByteArray encode_terminal_paste_text(
    QString                        text,
    Terminal_input_mode_state      modes,
    Terminal_paste_framing_policy  framing_policy,
    qsizetype                      reject_beyond_bytes =
                                       std::numeric_limits<qsizetype>::max());

}
