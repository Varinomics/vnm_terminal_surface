#pragma once

#include <QString>
#include <QStringView>

#include <functional>

namespace vnm_terminal::diagnostics {

enum class Level
{
    INFO,
    WARNING,
    ERROR,
};

// The message view is valid only for the duration of the call. A missing sink
// uses the component-labelled stderr fallback so the library remains usable in
// standalone hosts without acquiring a logger dependency.
using Sink = std::function<void(Level, QStringView)>;

void set_sink(Sink sink);
void clear_sink();
void write(Level level, QStringView message);
[[noreturn]] void fatal(QStringView message);

} // namespace vnm_terminal::diagnostics
