#pragma once

#include <QByteArrayView>
#include <QString>

#include <cstdint>
#include <functional>

class QKeyEvent;

namespace vnm_terminal::internal {

constexpr qint64 k_interaction_trace_total_capacity_bytes = 16LL * 1024LL * 1024LL;

bool interaction_trace_enabled();
bool set_interaction_trace_enabled(bool enabled, QString* out_error = nullptr);
void set_interaction_trace_failure_handler(std::function<void(QString)> handler);
QString interaction_trace_path();
std::uint64_t next_interaction_trace_correlation_id();
std::uint64_t current_interaction_trace_correlation_id();

class Interaction_trace_scope
{
public:
    explicit Interaction_trace_scope(std::uint64_t correlation_id);
    ~Interaction_trace_scope();

    Interaction_trace_scope(const Interaction_trace_scope&)            = delete;
    Interaction_trace_scope& operator=(const Interaction_trace_scope&) = delete;

private:
    std::uint64_t m_previous_id = 0U;
};

void record_interaction_trace(
    const char*   category,
    const char*   event,
    const QString& details = {},
    std::uint64_t correlation_id = 0U);
QString interaction_trace_byte_summary(QByteArrayView bytes);
QString interaction_trace_key_summary(const QKeyEvent& event);

}
