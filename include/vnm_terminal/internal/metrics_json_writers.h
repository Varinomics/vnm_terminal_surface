#pragma once

#include <QJsonObject>
#include <QString>

namespace vnm_terminal::diagnostics {

// Counter serialization shared with the first-party application, which emits its own
// runtime-metrics document alongside the surface-owned sections and must encode counters
// the same way. Counters are written as decimal STRINGS: a JSON number is a double, and
// the 64-bit frame, byte, and nanosecond counters this project reports lose precision
// above 2^53. Consumers parse these fields as integers, not as numbers.
//
// This is not part of the installed diagnostics API; see docs/public_surface.md.
template<typename Value>
void insert_json_counter(
    QJsonObject&  object,
    const char*   name,
    Value         value)
{
    object.insert(
        QString::fromLatin1(name),
        QString::number(static_cast<qulonglong>(value)));
}

}
