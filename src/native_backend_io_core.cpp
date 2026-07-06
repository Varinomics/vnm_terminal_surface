#include "native_backend_io_core.h"

#include "vnm_terminal/internal/session_contract.h"
#include <QDir>
#include <QProcessEnvironment>
#include <algorithm>
#include <limits>
#include <utility>

namespace vnm_terminal::internal {

namespace {

constexpr Terminal_backend_output_delivery_limits k_default_output_delivery_limits{
    k_terminal_default_output_queue_high_water_bytes,
    k_terminal_default_output_queue_hard_limit_bytes,
};

static_assert(
    k_native_backend_output_read_chunk_bytes <
    k_terminal_default_output_queue_hard_limit_bytes -
    k_terminal_default_output_queue_high_water_bytes);

qsizetype clamped_qsizetype_byte_count(std::size_t byte_count)
{
    return static_cast<qsizetype>(std::min<std::size_t>(
        byte_count,
        static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())));
}

std::size_t native_backend_callback_read_chunk_bytes(
    Terminal_backend_output_delivery_limits limits)
{
    const std::size_t headroom =
        limits.hard_limit_bytes > limits.high_water_bytes
            ? limits.hard_limit_bytes - limits.high_water_bytes
            : 1U;
    if (headroom <= 1U) {
        return 1U;
    }

    return std::max<std::size_t>(
        1U,
        std::min(k_native_backend_output_read_chunk_bytes, headroom - 1U));
}

std::size_t native_backend_paused_output_budget_bytes(
    Terminal_backend_output_delivery_limits limits,
    std::size_t                             callback_read_chunk_bytes)
{
    if (limits.hard_limit_bytes <= limits.high_water_bytes) {
        return callback_read_chunk_bytes;
    }

    const std::size_t headroom =
        limits.hard_limit_bytes - limits.high_water_bytes;
    if (headroom <= callback_read_chunk_bytes) {
        return 1U;
    }

    return headroom - callback_read_chunk_bytes;
}

}

Native_backend_start_precheck validate_native_backend_start_preconditions(
    const Terminal_launch_config&      config,
    const Terminal_backend_callbacks&  callbacks,
    Native_backend_start_gate          start_gate,
    QStringView                        backend_label)
{
    const Terminal_backend_result callback_result =
        validate_backend_callbacks(callbacks);
    if (is_backend_rejection(callback_result)) {
        return {callback_result, std::nullopt};
    }

    const Terminal_backend_result config_result = validate_launch_config(config);
    if (is_backend_rejection(config_result)) {
        callbacks.error_reported(*config_result.error);
        return {config_result, std::nullopt};
    }

    bool repeated_start = false;
    {
        std::lock_guard<std::mutex> lock(start_gate.mutex);
        if (start_gate.running ||
            start_gate.start_attempted ||
            start_gate.start_in_progress)
        {
            repeated_start = true;
        }
        else {
            start_gate.start_in_progress = true;
        }
    }

    if (repeated_start) {
        const Terminal_backend_result result = reject_native_backend_start_with_report(
            callbacks,
            Terminal_backend_error_code::START_FAILED,
            QStringLiteral("%1 backend can only be started once").arg(backend_label));
        return {result, std::nullopt};
    }

    std::optional<Terminal_effective_launch_config> effective_config = make_effective_launch_config(
        config,
        QProcessEnvironment::systemEnvironment());
    if (!effective_config.has_value()) {
        const Terminal_backend_result result = reject_native_backend_start_attempt(
            callbacks,
            start_gate,
            Terminal_backend_error_code::INVALID_LAUNCH_CONFIG,
            QStringLiteral("effective launch config could not be built"));
        return {result, std::nullopt};
    }

    if (!effective_config->working_directory.isEmpty() &&
        !QDir(effective_config->working_directory).exists())
    {
        const Terminal_backend_result result = reject_native_backend_start_attempt(
            callbacks,
            start_gate,
            Terminal_backend_error_code::WORKING_DIRECTORY_UNAVAILABLE,
            QStringLiteral("working directory does not exist"));
        return {result, std::nullopt};
    }

    return {backend_accept(), std::move(effective_config)};
}

void clear_native_backend_start_in_progress(
    Native_backend_start_gate start_gate)
{
    std::lock_guard<std::mutex> lock(start_gate.mutex);
    start_gate.start_in_progress = false;
}

Terminal_backend_result reject_native_backend_start_attempt(
    const Terminal_backend_callbacks&  callbacks,
    Native_backend_start_gate          start_gate,
    Terminal_backend_error_code        code,
    QString                            message)
{
    clear_native_backend_start_in_progress(start_gate);
    return reject_native_backend_start_with_report(callbacks, code, std::move(message));
}

Terminal_backend_result reject_native_backend_start_with_report(
    const Terminal_backend_callbacks&  callbacks,
    Terminal_backend_error_code        code,
    QString                            message)
{
    const Terminal_backend_result result = backend_reject(code, std::move(message));
    callbacks.error_reported(*result.error);
    return result;
}

bool native_backend_write_queue_can_accept(
    std::size_t    queued_write_bytes,
    std::size_t    incoming_write_bytes)
{
    return
        incoming_write_bytes <= k_native_backend_max_queued_write_bytes &&
        queued_write_bytes   <= k_native_backend_max_queued_write_bytes - incoming_write_bytes;
}

void add_native_backend_queued_write_bytes(
    std::size_t&   queued_write_bytes,
    std::size_t    incoming_write_bytes)
{
    queued_write_bytes += incoming_write_bytes;
}

void remove_native_backend_queued_write_bytes(
    std::size_t&   queued_write_bytes,
    std::size_t    completed_write_bytes)
{
    queued_write_bytes -= std::min(queued_write_bytes, completed_write_bytes);
}

native_backend_output_delivery_limits_t derive_native_backend_output_delivery_limits(
    const std::optional<Terminal_backend_output_delivery_limits>& configured_limits,
    std::optional<std::size_t> paused_output_high_watermark_ceiling_bytes)
{
    const Terminal_backend_output_delivery_limits limits =
        configured_limits.value_or(k_default_output_delivery_limits);
    const std::size_t read_chunk =
        native_backend_callback_read_chunk_bytes(limits);
    const std::size_t paused_budget =
        native_backend_paused_output_budget_bytes(limits, read_chunk);
    std::size_t delivery_chunk =
        paused_budget == 0U
            ? 0U
            : std::min(
                  paused_budget,
                  limits.high_water_bytes != 0U
                      ? limits.high_water_bytes
                      : paused_budget);
    std::size_t high_watermark = paused_budget;
    if (paused_output_high_watermark_ceiling_bytes.has_value()) {
        high_watermark =
            std::min(high_watermark, *paused_output_high_watermark_ceiling_bytes);
        delivery_chunk =
            std::min(delivery_chunk, *paused_output_high_watermark_ceiling_bytes);
    }

    return {
        clamped_qsizetype_byte_count(high_watermark),
        clamped_qsizetype_byte_count(delivery_chunk),
        clamped_qsizetype_byte_count(read_chunk),
    };
}

void append_native_backend_paused_output(
    QByteArray&    paused_output,
    QByteArray     bytes)
{
    if (paused_output.isEmpty()) {
        paused_output = std::move(bytes);
    }
    else {
        paused_output += bytes;
    }
}

void report_native_backend_error(
    const Terminal_backend_callbacks&  callbacks,
    Terminal_backend_error_code        code,
    QString                            message)
{
    if (callbacks.error_reported) {
        callbacks.error_reported({code, std::move(message)});
    }
}

void deliver_native_backend_output(
    const Terminal_backend_callbacks&  callbacks,
    QByteArray                         bytes)
{
    if (callbacks.output_received) {
        callbacks.output_received(std::move(bytes));
    }
}

void report_native_backend_exit(
    const Terminal_backend_callbacks&  callbacks,
    Terminal_exit_reason               reason,
    int                                exit_code)
{
    if (callbacks.process_exited) {
        callbacks.process_exited({reason, exit_code});
    }
}

void join_or_detach_native_backend_thread(std::thread& thread)
{
    if (!thread.joinable()) {
        return;
    }

    if (thread.get_id() == std::this_thread::get_id()) {
        thread.detach();
    }
    else {
        thread.join();
    }
}

}
