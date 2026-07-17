#include "vnm_terminal/internal/interaction_trace.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QIODevice>
#include <QStandardPaths>
#include <QString>

#include <cstdio>

namespace term = vnm_terminal::internal;

namespace {

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

bool create_sized_file(const QString& path, qint64 size)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.resize(size);
}

}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("VarinomicsTraceTest"));
    QCoreApplication::setApplicationName(QStringLiteral("interaction_trace"));
    QStandardPaths::setTestModeEnabled(true);

    const QString path = term::interaction_trace_path();
    (void)QFile::remove(path);
    (void)QFile::remove(path + QStringLiteral(".previous"));

    const qint64 per_file_limit = term::k_interaction_trace_total_capacity_bytes / 2;
    bool ok = true;
    ok &= check(!term::interaction_trace_enabled(),
        "interaction trace starts disabled");
    ok &= check(create_sized_file(path, per_file_limit + 1),
        "oversized current trace fixture is created");
    ok &= check(create_sized_file(path + QStringLiteral(".previous"), per_file_limit + 1),
        "oversized previous trace fixture is created");

    QString error;
    ok &= check(term::set_interaction_trace_enabled(true, &error),
        "interaction trace opens in the Qt test data location");
    ok &= check(term::interaction_trace_enabled(),
        "interaction trace reports enabled after opening");
    if (ok) {
        ok &= check(QFileInfo(path).size() == 0,
            "enable normalizes an oversized current trace");
        ok &= check(!QFileInfo::exists(path + QStringLiteral(".previous")),
            "enable removes an oversized previous trace");

        term::record_interaction_trace("test", "json-shape", QStringLiteral("safe"), 4242U);
        QFile trace_file(path);
        ok &= check(trace_file.open(QIODevice::ReadOnly),
            "interaction trace can be read back");
        const QJsonDocument record =
            QJsonDocument::fromJson(trace_file.readLine());
        const QJsonObject record_object = record.object();
        ok &= check(record.isObject()                                      &&
            record_object.value(QStringLiteral("id")).toInteger() == 4242 &&
            record_object.value(QStringLiteral("category")).toString() ==
                QStringLiteral("test")                                    &&
            record_object.value(QStringLiteral("event")).toString() ==
                QStringLiteral("json-shape"),
            "interaction trace writes valid JSON with correlation fields");
        trace_file.close();

        term::record_interaction_trace(
            "test",
            "oversized-record",
            QString(static_cast<int>(per_file_limit + 1), QLatin1Char('y')));
        ok &= check(QFileInfo(path).size() <= per_file_limit,
            "a single oversized record cannot exceed the file bound");
        ok &= check(trace_file.open(QIODevice::ReadOnly),
            "interaction trace reopens after the oversized record");
        ok &= check(trace_file.readAll().contains("oversized-record-omitted"),
            "an oversized record is replaced by the omission marker");
        trace_file.close();

        const QByteArray oversized_category(
            static_cast<qsizetype>(per_file_limit + 1),
            'z');
        term::record_interaction_trace(oversized_category.constData(), "test");
        ok &= check(QFileInfo(path).size() <= per_file_limit,
            "an oversized trace category cannot exceed the file bound");

        const QString payload(1024 * 1024, QLatin1Char('x'));
        for (int i = 0; i < 18; ++i) {
            term::record_interaction_trace("test", "rotation", payload);
        }
        (void)term::set_interaction_trace_enabled(false);
        ok &= check(!term::interaction_trace_enabled(),
            "interaction trace reports disabled after closing");
        const qint64 disabled_size = QFileInfo(path).size();
        term::record_interaction_trace("test", "disabled-write");
        ok &= check(QFileInfo(path).size() == disabled_size,
            "disabled interaction trace writes nothing");

        const QFileInfo current(path);
        const QFileInfo previous(path + QStringLiteral(".previous"));
        ok &= check(current.exists() && current.size() <= per_file_limit,
            "current interaction trace remains within its half of the total bound");
        ok &= check(previous.exists() && previous.size() <= per_file_limit,
            "previous interaction trace remains within its half of the total bound");
    }

    ok &= check(term::current_interaction_trace_correlation_id() == 0U,
        "interaction trace scope starts empty");
    {
        const term::Interaction_trace_scope outer_scope(17U);
        ok &= check(term::current_interaction_trace_correlation_id() == 17U,
            "interaction trace scope installs its correlation id");
        {
            const term::Interaction_trace_scope inner_scope(23U);
            ok &= check(term::current_interaction_trace_correlation_id() == 23U,
                "nested interaction trace scope installs its correlation id");
        }
        ok &= check(term::current_interaction_trace_correlation_id() == 17U,
            "nested interaction trace scope restores its parent");
    }
    ok &= check(term::current_interaction_trace_correlation_id() == 0U,
        "interaction trace scope restores the empty correlation id");

    const QString byte_summary = term::interaction_trace_byte_summary("secret\x1b");
    ok &= check(byte_summary.contains(QStringLiteral("ESC")),
        "byte summary identifies Escape");
    ok &= check(!byte_summary.contains(QStringLiteral("secret")),
        "byte summary does not expose printable input text");

    const QKeyEvent escape_event(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    const QString escape_summary = term::interaction_trace_key_summary(escape_event);
    ok &= check(escape_summary.contains(QStringLiteral("Escape")),
        "key summary identifies Escape");

    const QKeyEvent printable_event(
        QEvent::KeyPress,
        Qt::Key_A,
        Qt::ControlModifier | Qt::AltModifier,
        QStringLiteral("password"));
    const QString printable_summary = term::interaction_trace_key_summary(printable_event);
    ok &= check(printable_summary.contains(QStringLiteral("kind=printable")) &&
        printable_summary.contains(QStringLiteral("text_units=8")),
        "text-producing key summary records only its redacted shape");
    ok &= check(!printable_summary.contains(QStringLiteral("password")) &&
        !printable_summary.contains(QStringLiteral("name=")) &&
        !printable_summary.contains(QStringLiteral("Control")) &&
        !printable_summary.contains(QStringLiteral("Alt")),
        "text-producing key summary omits key identity and modifiers");

    const QKeyEvent altgr_text_event(
        QEvent::KeyPress,
        Qt::Key_unknown,
        Qt::GroupSwitchModifier,
        QString::fromUtf8("\xe2\x82\xac"));
    const QString altgr_summary = term::interaction_trace_key_summary(altgr_text_event);
    ok &= check(altgr_summary.contains(QStringLiteral("kind=printable")) &&
        !altgr_summary.contains(QString::fromUtf8("\xe2\x82\xac")),
        "AltGr text is redacted");

    (void)QFile::remove(path);
    (void)QFile::remove(path + QStringLiteral(".previous"));
    return ok ? 0 : 1;
}
