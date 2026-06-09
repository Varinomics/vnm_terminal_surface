#pragma once

#include <QByteArrayView>
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

inline bool palette_index_is_valid(int value)
{
    return value >= 0 && value <= 255;
}

}
