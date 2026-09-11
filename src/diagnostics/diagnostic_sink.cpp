#include "vnm_terminal/diagnostics/diagnostic_sink.h"

#include <QByteArray>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <utility>

namespace vnm_terminal::diagnostics {
namespace {

std::mutex& sink_mutex()
{
    static std::mutex mutex;
    return mutex;
}

Sink& configured_sink()
{
    static Sink sink;
    return sink;
}

const char* level_name(Level level)
{
    switch (level) {
        case Level::INFO:    return "info";
        case Level::WARNING: return "warning";
        case Level::ERROR:   return "error";
    }
    return "error";
}

void write_fallback(Level level, QStringView message)
{
    const QByteArray utf8 = message.toString().toUtf8();
    std::fprintf(
        stderr,
        "[vnm_terminal_surface][%s] %s\n",
        level_name(level),
        utf8.constData());
    std::fflush(stderr);
}

} // namespace

void set_sink(Sink sink)
{
    const std::lock_guard<std::mutex> lock(sink_mutex());
    configured_sink() = std::move(sink);
}

void clear_sink()
{
    set_sink({});
}

void write(Level level, QStringView message)
{
    Sink sink;
    {
        const std::lock_guard<std::mutex> lock(sink_mutex());
        sink = configured_sink();
    }

    if (!sink) {
        write_fallback(level, message);
        return;
    }

    try {
        sink(level, message);
    }
    catch (...) {
        write_fallback(level, message);
    }
}

[[noreturn]] void fatal(QStringView message)
{
    write(Level::ERROR, message);
    std::abort();
}

} // namespace vnm_terminal::diagnostics
