#pragma once

#include "vnm_terminal/internal/unicode_width.h"

#include <QChar>
#include <QString>
#include <QtGlobal>
#include <array>
#include <cstddef>

namespace vnm_terminal::internal {

// Share immutable text across active and retained ASCII cells. A QString write
// detaches the cell, so history decoding does not need a separate allocation.
inline const QString& printable_ascii_cell_text(QChar character)
{
    Q_ASSERT(character.unicode() >= k_printable_ascii_first &&
        character.unicode() <= k_printable_ascii_last);

    constexpr std::size_t count =
        k_printable_ascii_last - k_printable_ascii_first + 1U;

    static const std::array<QString, count> strings{
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

}
