#include "vnm_terminal/internal/interaction_trace.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QStandardPaths>
#include <QStringList>
#include <QThread>
#include <QtGlobal>

#include <atomic>
#include <functional>
#include <mutex>
#include <utility>

namespace vnm_terminal::internal {

namespace {

constexpr qint64 k_trace_file_capacity_bytes = k_interaction_trace_total_capacity_bytes / 2;

struct Interaction_trace_state
{
    std::mutex    mutex;
    QFile         file;
    QElapsedTimer elapsed;
    QString       path;
    std::function<void(QString)> failure_handler;
    bool          enabled = false;
};

Interaction_trace_state& trace_state()
{
    static Interaction_trace_state state;
    return state;
}

std::atomic<std::uint64_t>& correlation_sequence()
{
    static std::atomic<std::uint64_t> sequence{1U};
    return sequence;
}

std::atomic<bool>& trace_enabled_flag()
{
    static std::atomic<bool> enabled{false};
    return enabled;
}

thread_local std::uint64_t s_current_correlation_id = 0U;

QString default_trace_path()
{
    const QString directory =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir(directory).filePath(QStringLiteral("interaction-trace.jsonl"));
}

bool remove_oversized_trace_file(const QString& path, QString* out_error)
{
    const QFileInfo info(path);
    if (!info.exists() || info.size() <= k_trace_file_capacity_bytes) {
        return true;
    }
    if (QFile::remove(path)) {
        return true;
    }
    if (out_error != nullptr) {
        *out_error = QStringLiteral("failed to normalize oversized interaction trace: %1")
            .arg(path);
    }
    return false;
}

bool open_trace_file(Interaction_trace_state& state, QString* out_error)
{
    state.path = default_trace_path();
    const QFileInfo info(state.path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (out_error != nullptr) {
            *out_error = QStringLiteral("failed to create interaction trace directory: %1")
                .arg(info.absolutePath());
        }
        return false;
    }

    if (!remove_oversized_trace_file(state.path, out_error) ||
        !remove_oversized_trace_file(state.path + QStringLiteral(".previous"), out_error))
    {
        return false;
    }

    state.file.setFileName(state.path);
    if (!state.file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        if (out_error != nullptr) {
            *out_error = QStringLiteral("failed to open interaction trace %1: %2")
                .arg(state.path, state.file.errorString());
        }
        return false;
    }
    return true;
}

bool rotate_trace_file(Interaction_trace_state& state, QString* out_error)
{
    state.file.close();
    const QString previous_path = state.path + QStringLiteral(".previous");
    (void)QFile::remove(previous_path);
    if (QFile::exists(state.path) && !QFile::rename(state.path, previous_path)) {
        if (out_error != nullptr) {
            *out_error = QStringLiteral("failed to rotate interaction trace: %1")
                .arg(state.path);
        }
        return false;
    }
    if (!state.file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (out_error != nullptr) {
            *out_error = QStringLiteral("failed to reopen interaction trace %1: %2")
                .arg(state.path, state.file.errorString());
        }
        return false;
    }
    return true;
}

QString control_name(uchar byte)
{
    switch (byte) {
        case 0x03: return QStringLiteral("ETX");
        case 0x08: return QStringLiteral("BS");
        case 0x09: return QStringLiteral("TAB");
        case 0x0a: return QStringLiteral("LF");
        case 0x0d: return QStringLiteral("CR");
        case 0x1b: return QStringLiteral("ESC");
        case 0x7f: return QStringLiteral("DEL");
        default:   return QStringLiteral("0x%1").arg(byte, 2, 16, QLatin1Char('0'));
    }
}

QString key_name(int key)
{
    switch (key) {
        case Qt::Key_Escape:    return QStringLiteral("Escape");
        case Qt::Key_Tab:       return QStringLiteral("Tab");
        case Qt::Key_Backtab:   return QStringLiteral("Backtab");
        case Qt::Key_Backspace: return QStringLiteral("Backspace");
        case Qt::Key_Return:    return QStringLiteral("Return");
        case Qt::Key_Enter:     return QStringLiteral("Enter");
        case Qt::Key_Insert:    return QStringLiteral("Insert");
        case Qt::Key_Delete:    return QStringLiteral("Delete");
        case Qt::Key_Home:      return QStringLiteral("Home");
        case Qt::Key_End:       return QStringLiteral("End");
        case Qt::Key_Left:      return QStringLiteral("Left");
        case Qt::Key_Up:        return QStringLiteral("Up");
        case Qt::Key_Right:     return QStringLiteral("Right");
        case Qt::Key_Down:      return QStringLiteral("Down");
        case Qt::Key_PageUp:    return QStringLiteral("PageUp");
        case Qt::Key_PageDown:  return QStringLiteral("PageDown");
        case Qt::Key_Control:   return QStringLiteral("Control");
        case Qt::Key_Shift:     return QStringLiteral("Shift");
        case Qt::Key_Alt:       return QStringLiteral("Alt");
        case Qt::Key_Meta:      return QStringLiteral("Meta");
        case Qt::Key_AltGr:     return QStringLiteral("AltGr");
        default:
            if (key >= Qt::Key_F1 && key <= Qt::Key_F35) {
                return QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
            }
            return {};
    }
}

QString modifier_names(Qt::KeyboardModifiers modifiers)
{
    QStringList names;
    if (modifiers.testFlag(Qt::ShiftModifier))   { names.push_back(QStringLiteral("Shift"));   }
    if (modifiers.testFlag(Qt::ControlModifier)) { names.push_back(QStringLiteral("Control")); }
    if (modifiers.testFlag(Qt::AltModifier))     { names.push_back(QStringLiteral("Alt"));     }
    if (modifiers.testFlag(Qt::MetaModifier))    { names.push_back(QStringLiteral("Meta"));    }
    return names.isEmpty() ? QStringLiteral("none") : names.join(QLatin1Char('+'));
}

}

bool interaction_trace_enabled()
{
    return trace_enabled_flag().load(std::memory_order_acquire);
}

bool set_interaction_trace_enabled(bool enabled, QString* out_error)
{
    Interaction_trace_state& state = trace_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (enabled == state.enabled) {
        return true;
    }
    if (!enabled) {
        state.enabled = false;
        trace_enabled_flag().store(false, std::memory_order_release);
        state.file.close();
        return true;
    }
    if (!open_trace_file(state, out_error)) {
        return false;
    }
    state.elapsed.start();
    state.enabled = true;
    trace_enabled_flag().store(true, std::memory_order_release);
    return true;
}

void set_interaction_trace_failure_handler(std::function<void(QString)> handler)
{
    Interaction_trace_state& state = trace_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.failure_handler = std::move(handler);
}

QString interaction_trace_path()
{
    Interaction_trace_state& state = trace_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.path.isEmpty() ? default_trace_path() : state.path;
}

std::uint64_t next_interaction_trace_correlation_id()
{
    return correlation_sequence().fetch_add(1U, std::memory_order_relaxed);
}

std::uint64_t current_interaction_trace_correlation_id()
{
    return s_current_correlation_id;
}

Interaction_trace_scope::Interaction_trace_scope(std::uint64_t correlation_id)
:
    m_previous_id(s_current_correlation_id)
{
    s_current_correlation_id = correlation_id;
}

Interaction_trace_scope::~Interaction_trace_scope()
{
    s_current_correlation_id = m_previous_id;
}

void record_interaction_trace(
    const char*    category,
    const char*    event,
    const QString& details,
    std::uint64_t  correlation_id)
{
    if (!interaction_trace_enabled()) {
        return;
    }

    Interaction_trace_state& state = trace_state();
    std::unique_lock<std::mutex> lock(state.mutex);
    if (!state.enabled) {
        return;
    }

    QJsonObject object;
    object.insert(QStringLiteral("t_us"), state.elapsed.nsecsElapsed() / 1000);
    object.insert(
        QStringLiteral("wall_utc"),
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    object.insert(QStringLiteral("pid"), QCoreApplication::applicationPid());
    object.insert(
        QStringLiteral("thread"),
        QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId())));
    object.insert(QStringLiteral("id"), static_cast<qint64>(correlation_id));
    object.insert(QStringLiteral("category"), QString::fromLatin1(category));
    object.insert(QStringLiteral("event"), QString::fromLatin1(event));
    if (!details.isEmpty()) {
        object.insert(QStringLiteral("details"), details);
    }
    QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact);
    line.append('\n');

    if (line.size() > k_trace_file_capacity_bytes) {
        object.insert(QStringLiteral("category"), QStringLiteral("trace"));
        object.insert(QStringLiteral("event"), QStringLiteral("oversized-record-omitted"));
        object.insert(
            QStringLiteral("details"),
            QStringLiteral("oversized trace record omitted original_bytes=%1").arg(line.size()));
        line = QJsonDocument(object).toJson(QJsonDocument::Compact);
        line.append('\n');
        Q_ASSERT(line.size() <= k_trace_file_capacity_bytes);
    }

    QString failure;
    if (state.file.size() + line.size() > k_trace_file_capacity_bytes &&
        !rotate_trace_file(state, &failure))
    {
        state.enabled = false;
        trace_enabled_flag().store(false, std::memory_order_release);
    }
    else
    if (state.file.write(line) != line.size() || !state.file.flush()) {
        failure = QStringLiteral("failed to write interaction trace %1: %2")
            .arg(state.path, state.file.errorString());
        state.enabled = false;
        trace_enabled_flag().store(false, std::memory_order_release);
        state.file.close();
    }

    if (!failure.isEmpty()) {
        const std::function<void(QString)> handler = state.failure_handler;
        lock.unlock();
        qWarning().noquote() << failure;
        if (handler) {
            handler(failure);
        }
    }
}

QString interaction_trace_byte_summary(QByteArrayView bytes)
{
    QStringList controls;
    qsizetype non_control_count = 0;
    for (char value : bytes) {
        const uchar byte = static_cast<uchar>(value);
        if (byte < 0x20 || byte == 0x7f) {
            controls.push_back(control_name(byte));
        }
        else {
            ++non_control_count;
        }
    }
    return QStringLiteral("bytes=%1 non_control=%2 controls=%3")
        .arg(bytes.size())
        .arg(non_control_count)
        .arg(controls.isEmpty() ? QStringLiteral("none") : controls.join(QLatin1Char(',')));
}

QString interaction_trace_key_summary(const QKeyEvent& event)
{
    const QString name = key_name(event.key());
    if (!name.isEmpty()) {
        return QStringLiteral("kind=control name=%1 modifiers=%2 autorepeat=%3")
            .arg(name, modifier_names(event.modifiers()))
            .arg(event.isAutoRepeat());
    }

    return QStringLiteral("kind=printable text_units=%1 autorepeat=%2")
        .arg(event.text().size())
        .arg(event.isAutoRepeat());
}

}
