#pragma once

#include <QByteArrayView>
#include <cstddef>
#include <vector>

namespace vnm_terminal::internal {

// Parsed CSI/SGR parameter vocabulary shared by the byte-stream parser (which
// turns raw parameter bytes into SGR operations) and the screen model (which
// re-parses dispatched control-sequence parameters). Declared once here so both
// translation units agree on the representation without duplicating it.
struct Sgr_parameter_atom
{
    bool                   has_value          = false;
    int                    value              = 0;
};

struct Sgr_parameter_group
{
    std::vector<Sgr_parameter_atom> atoms;
};

// Parse CSI parameter bytes into groups of colon-separated atoms, with
// semicolons separating groups. Enforces the parser's atom/group/digit limits
// and returns false on overflow or on any byte outside the parameter grammar.
// Defined in terminal_byte_stream_parser.cpp; linked into both translation
// units of this library.
bool parse_sgr_parameter_groups(
    QByteArrayView                     bytes,
    std::vector<Sgr_parameter_group>&  groups);

// Every group holds exactly one atom. The screen model's control-sequence
// dispatch and the session's backend-output prescan both need this shape before
// they can read a parameter as a plain integer.
inline bool parse_simple_csi_parameter_groups(
    QByteArrayView                       parameter_bytes,
    std::vector<Sgr_parameter_group>&    groups)
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

inline bool csi_parameter_value(
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

inline bool palette_index_is_valid(int value)
{
    return value >= 0 && value <= 255;
}

}
