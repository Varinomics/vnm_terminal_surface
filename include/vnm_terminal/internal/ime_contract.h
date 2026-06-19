#pragma once

#include <QString>

namespace vnm_terminal::internal {

struct Ime_preedit_state
{
    QString    text;
    int        cursor_position = 0;
    bool       active          = false;
};

inline bool same_ime_preedit_state(
    const Ime_preedit_state&   lhs,
    const Ime_preedit_state&   rhs)
{
    return
        lhs.text            == rhs.text            &&
        lhs.cursor_position == rhs.cursor_position &&
        lhs.active          == rhs.active;
}

inline bool ime_preedit_has_content(const Ime_preedit_state& state)
{
    return state.active || !state.text.isEmpty();
}

}
