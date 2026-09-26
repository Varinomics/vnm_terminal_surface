#pragma once

#include "vnm_terminal/internal/terminal_byte_stream_parser.h"

#include <QByteArrayView>
#include <iterator>
#include <vector>

namespace vnm_terminal::test_helpers {

// Parses all of bytes, across the stops the parser makes after each sixel
// image, and returns every action in order.
inline std::vector<internal::Parser_action> ingest_all(
    internal::Terminal_byte_stream_parser& parser,
    QByteArrayView                         bytes)
{
    std::vector<internal::Parser_action> actions;
    qsizetype parsed = 0;
    do {
        std::vector<internal::Parser_action> batch = parser.ingest(bytes, parsed);
        actions.insert(
            actions.end(),
            std::make_move_iterator(batch.begin()),
            std::make_move_iterator(batch.end()));
    }
    while (parsed < bytes.size());
    return actions;
}

}
