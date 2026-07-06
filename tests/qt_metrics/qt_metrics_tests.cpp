#include "vnm_terminal/internal/qt_grid_metrics_provider.h"
#include "vnm_terminal/internal/terminal_resize_controller.h"
#include "vnm_terminal/internal/terminal_session.h"
#include "vnm_terminal/internal/vnm_terminal_font.h"
#include "vnm_terminal/diagnostics/metrics_json.h"
#include "vnm_terminal/font_metrics.h"
#include "vnm_terminal/vnm_terminal_surface.h"
#include "helpers/test_check.h"

#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QJsonObject>
#include <QQuickWindow>
#include <QThread>
#include <cmath>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

bool nearly_equal(qreal lhs, qreal rhs, qreal tolerance = 0.001)
{
    return std::abs(lhs - rhs) <= tolerance;
}

bool same_metrics(
    term::terminal_cell_metrics_t  lhs,
    term::terminal_cell_metrics_t  rhs)
{
    return
        nearly_equal(lhs.width, rhs.width)   &&
        nearly_equal(lhs.height, rhs.height) &&
        nearly_equal(lhs.ascent, rhs.ascent) &&
        nearly_equal(lhs.descent, rhs.descent);
}

bool metric_is_device_pixel_aligned(qreal value, qreal device_pixel_ratio)
{
    const qreal physical_value = value * device_pixel_ratio;
    return nearly_equal(physical_value, std::round(physical_value));
}

void pump_events(QGuiApplication& app, int rounds = 6)
{
    for (int i = 0; i < rounds; ++i) {
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
}

QFont terminal_font(const QString& family, qreal pixel_size)
{
    return term::vnm_terminal_font(family, pixel_size);
}

term::Terminal_launch_config valid_launch_config()
{
    term::Terminal_launch_config config;
    config.argv              = {QStringLiteral("terminal-fixture")};
    config.working_directory = QStringLiteral("C:/workspace");
    return config;
}

term::Terminal_session_config traced_session_config()
{
    term::Terminal_session_config config;
    config.trace_command_limit      = 64U;
    config.trace_notification_limit = 64U;
    config.trace_result_limit       = 64U;
    config.trace_resize_limit       = 64U;
    return config;
}

class Recording_backend final : public term::Terminal_backend
{
public:
    term::Terminal_backend_result start(
        const term::Terminal_launch_config&    config,
        term::Terminal_backend_callbacks       callbacks) override
    {
        const term::Terminal_backend_result callback_result =
            term::validate_backend_callbacks(callbacks);
        if (term::is_backend_rejection(callback_result)) {
            return callback_result;
        }

        start_configs.push_back(config);

        const term::Terminal_backend_result config_result =
            term::validate_launch_config(config);
        if (term::is_backend_rejection(config_result)) {
            return config_result;
        }

        running = true;
        return term::backend_accept();
    }

    term::Terminal_backend_result write(QByteArray bytes) override
    {
        writes.push_back(std::move(bytes));
        return running
            ? term::backend_accept()
            : term::backend_reject(
                term::Terminal_backend_error_code::WRITE_FAILED,
                QStringLiteral("recording backend is not running"));
    }

    term::Terminal_backend_result resize(term::Terminal_backend_resize_request request) override
    {
        resize_requests.push_back(request);
        return term::is_valid_grid_size(request.grid_size)
            ? term::backend_accept()
            : term::backend_reject(
                term::Terminal_backend_error_code::RESIZE_FAILED,
                QStringLiteral("recording backend received invalid grid"));
    }

    term::Terminal_backend_result set_output_paused(bool paused) override
    {
        pause_requests.push_back(paused);
        return term::backend_accept();
    }

    term::Terminal_backend_result interrupt() override
    {
        running = false;
        return term::backend_accept();
    }

    term::Terminal_backend_result terminate() override
    {
        running = false;
        return term::backend_accept();
    }

    bool                       running = false;
    std::vector<term::Terminal_launch_config>
                               start_configs;
    std::vector<term::Terminal_backend_resize_request>
                               resize_requests;
    std::vector<QByteArray>    writes;
    std::vector<bool>          pause_requests;
};

term::Terminal_metrics_result expected_grid(
    const VNM_TerminalSurface&             surface,
    const term::Qt_grid_metrics_provider&  provider)
{
    return term::grid_size_for_geometry(surface.size(), provider.cell_metrics());
}

bool grid_matches_surface(
    const VNM_TerminalSurface&             surface,
    term::terminal_grid_size_t             grid_size)
{
    return surface.rows() == grid_size.rows && surface.columns() == grid_size.columns;
}

std::optional<QSizeF> geometry_with_different_grid(
    const term::Qt_grid_metrics_provider&  provider,
    QSizeF                                 base_geometry)
{
    const term::Terminal_metrics_result base_grid =
        provider.grid_size_for_item_geometry(base_geometry);
    if (base_grid.status != term::Terminal_metrics_status::OK) {
        return std::nullopt;
    }

    const term::terminal_cell_metrics_t metrics = provider.cell_metrics();
    for (int step = 1; step <= 8; ++step) {
        const QSizeF candidate(
            base_geometry.width() + metrics.width * static_cast<qreal>(step + 1),
            base_geometry.height() + metrics.height * static_cast<qreal>(step + 1));
        const term::Terminal_metrics_result candidate_grid =
            provider.grid_size_for_item_geometry(candidate);
        if (candidate_grid.status                == term::Terminal_metrics_status::OK &&
            (candidate_grid.grid_size.rows != base_grid.grid_size.rows ||
             candidate_grid.grid_size.columns != base_grid.grid_size.columns))
        {
            return candidate;
        }
    }

    return std::nullopt;
}

std::optional<qreal> pixel_size_with_different_grid(
    term::Qt_grid_metrics_provider&        provider,
    const QString&                         family,
    qreal                                  base_pixel_size,
    QSizeF                                 geometry)
{
    const term::Terminal_metrics_result base_grid =
        provider.grid_size_for_item_geometry(geometry);
    if (base_grid.status != term::Terminal_metrics_status::OK) {
        return std::nullopt;
    }

    for (qreal pixel_size = base_pixel_size + 2.0; pixel_size <= base_pixel_size + 16.0;
        pixel_size += 2.0)
    {
        provider.set_font(terminal_font(family, pixel_size));
        const term::Terminal_metrics_result candidate_grid =
            provider.grid_size_for_item_geometry(geometry);
        if (candidate_grid.status                == term::Terminal_metrics_status::OK &&
            (candidate_grid.grid_size.rows != base_grid.grid_size.rows ||
             candidate_grid.grid_size.columns != base_grid.grid_size.columns))
        {
            return pixel_size;
        }
    }

    provider.set_font(terminal_font(family, base_pixel_size));
    return std::nullopt;
}

bool test_provider_metrics(qreal device_pixel_ratio)
{
    bool ok = true;

    const QString family = QStringLiteral("monospace");
    term::Qt_grid_metrics_provider provider(
        terminal_font(family, 12.0),
        device_pixel_ratio);
    const term::terminal_cell_metrics_t metrics = provider.cell_metrics();
    ok &= check(term::is_valid_cell_metrics(metrics),
        "Qt provider returns finite positive cell metrics");
    ok &= check(metrics.ascent > 0.0 && metrics.descent >= 0.0,
        "Qt provider ascent and descent are coherent");
    ok &= check(metrics.height + 0.001 >= metrics.ascent + metrics.descent,
        "Qt provider line height covers ascent and descent");

    term::Qt_grid_metrics_provider larger_provider(
        terminal_font(family, 18.0),
        device_pixel_ratio);
    const term::terminal_cell_metrics_t larger_metrics = larger_provider.cell_metrics();
    ok &= check(larger_metrics.width > metrics.width,
        "larger font size increases cell width for same family");
    ok &= check(larger_metrics.height > metrics.height,
        "larger font size increases cell height for same family");

    const QSizeF geometry(320.0, 160.0);
    const term::Terminal_metrics_result grid =
        provider.grid_size_for_item_geometry(geometry);
    ok &= check(grid.status == term::Terminal_metrics_status::OK,
        "Qt provider computes grid for positive geometry");
    ok &= check(grid.grid_size.rows > 0 && grid.grid_size.columns > 0,
        "Qt provider grid is positive for positive geometry");
    ok &= check(provider.grid_size_for_item_geometry(QSizeF()).status ==
        term::Terminal_metrics_status::INVALID_GEOMETRY,
        "Qt provider rejects zero geometry explicitly");

    term::Qt_grid_metrics_provider dpr_one_provider(
        terminal_font(family, 12.0),
        1.0);
    term::Qt_grid_metrics_provider scaled_dpr_provider(
        terminal_font(family, 12.0),
        2.0);
    const term::Terminal_metrics_result dpr_one_grid =
        dpr_one_provider.grid_size_for_item_geometry(geometry);
    const term::Terminal_metrics_result scaled_dpr_grid =
        scaled_dpr_provider.grid_size_for_item_geometry(geometry);
    const term::terminal_cell_metrics_t dpr_one_metrics =
        dpr_one_provider.cell_metrics();
    const term::terminal_cell_metrics_t scaled_dpr_metrics =
        scaled_dpr_provider.cell_metrics();
    ok &= check(metric_is_device_pixel_aligned(dpr_one_metrics.width, 1.0) &&
            metric_is_device_pixel_aligned(dpr_one_metrics.height, 1.0) &&
            metric_is_device_pixel_aligned(dpr_one_metrics.ascent, 1.0) &&
            metric_is_device_pixel_aligned(dpr_one_metrics.descent, 1.0),
        "Qt provider aligns DPR 1 metrics to physical pixels");
    ok &= check(metric_is_device_pixel_aligned(scaled_dpr_metrics.width, 2.0) &&
            metric_is_device_pixel_aligned(scaled_dpr_metrics.height, 2.0) &&
            metric_is_device_pixel_aligned(scaled_dpr_metrics.ascent, 2.0) &&
            metric_is_device_pixel_aligned(scaled_dpr_metrics.descent, 2.0),
        "Qt provider aligns scaled-DPR metrics to physical pixels");
    ok &= check(dpr_one_grid.status == term::Terminal_metrics_status::OK &&
            scaled_dpr_grid.status == term::Terminal_metrics_status::OK &&
            dpr_one_grid.grid_size.rows > 0 &&
            dpr_one_grid.grid_size.columns > 0 &&
            scaled_dpr_grid.grid_size.rows > 0 &&
            scaled_dpr_grid.grid_size.columns > 0,
        "Qt provider computes positive grids for DPR-snapped metrics");

    term::Qt_grid_metrics_provider normalized_constructor_provider(
        terminal_font(family, 12.0),
        std::numeric_limits<qreal>::quiet_NaN());
    ok &= check(nearly_equal(normalized_constructor_provider.device_pixel_ratio(), 1.0),
        "Qt provider normalizes constructor NaN DPR to 1");
    term::Qt_grid_metrics_provider zero_constructor_provider(
        terminal_font(family, 12.0),
        0.0);
    ok &= check(nearly_equal(zero_constructor_provider.device_pixel_ratio(), 1.0),
        "Qt provider normalizes constructor zero DPR to 1");
    term::Qt_grid_metrics_provider negative_constructor_provider(
        terminal_font(family, 12.0),
        -2.0);
    ok &= check(nearly_equal(negative_constructor_provider.device_pixel_ratio(), 1.0),
        "Qt provider normalizes constructor negative DPR to 1");
    normalized_constructor_provider.set_device_pixel_ratio(0.0);
    ok &= check(nearly_equal(normalized_constructor_provider.device_pixel_ratio(), 1.0),
        "Qt provider normalizes zero DPR to 1");
    normalized_constructor_provider.set_device_pixel_ratio(-2.0);
    ok &= check(nearly_equal(normalized_constructor_provider.device_pixel_ratio(), 1.0),
        "Qt provider normalizes negative DPR to 1");
    normalized_constructor_provider.set_device_pixel_ratio(1.75);
    ok &= check(nearly_equal(normalized_constructor_provider.device_pixel_ratio(), 1.75),
        "Qt provider retains positive finite DPR state");
    normalized_constructor_provider.set_device_pixel_ratio(
        std::numeric_limits<qreal>::quiet_NaN());
    ok &= check(nearly_equal(normalized_constructor_provider.device_pixel_ratio(), 1.0),
        "Qt provider normalizes setter NaN DPR to 1");

    return ok;
}

bool test_surface_publication(QGuiApplication& app, qreal device_pixel_ratio)
{
    bool ok = true;

    int grid_signal_count      = 0;
    int family_signal_count    = 0;
    int font_size_signal_count = 0;

    QQuickWindow window;
    window.setColor(QColor(32, 32, 32));
    window.resize(360, 180);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(360.0, 180.0));
    surface.set_font_family(QStringLiteral("monospace"));
    surface.set_font_size(14.0);

    QObject::connect(
        &surface,
        &VNM_TerminalSurface::grid_geometry_changed,
        &surface,
        [&grid_signal_count] {
            ++grid_signal_count;
        });
    QObject::connect(
        &surface,
        &VNM_TerminalSurface::font_family_changed,
        &surface,
        [&family_signal_count] {
            ++family_signal_count;
        });
    QObject::connect(
        &surface,
        &VNM_TerminalSurface::font_size_changed,
        &surface,
        [&font_size_signal_count] {
            ++font_size_signal_count;
        });

    window.show();
    pump_events(app);

    term::Qt_grid_metrics_provider provider(
        terminal_font(surface.font_family(), surface.font_size()),
        device_pixel_ratio);
    term::Terminal_metrics_result grid = expected_grid(surface, provider);
    ok &= check(grid.status == term::Terminal_metrics_status::OK,
        "surface positive geometry has provider-derived grid");
    ok &= check(grid_matches_surface(surface, grid.grid_size),
        "surface publishes provider-derived rows and columns");

    const int grid_signals_before_noops = grid_signal_count;
    surface.set_font_family(surface.font_family());
    surface.set_font_size(surface.font_size());
    pump_events(app);
    ok &= check(family_signal_count == 0,
        "font-family setter no-op emits no font signal");
    ok &= check(font_size_signal_count == 0,
        "font-size setter no-op emits no font-size signal");
    ok &= check(grid_signal_count == grid_signals_before_noops,
        "font setter no-ops emit no grid signal");

    surface.setSize(QSizeF());
    pump_events(app);
    grid = expected_grid(surface, provider);
    ok &= check(grid.status == term::Terminal_metrics_status::INVALID_GEOMETRY &&
        surface.rows() == 0 &&
        surface.columns() == 0,
        "surface publishes zero grid for provider-invalid zero geometry");

    surface.setSize(QSizeF(0.5, 0.5));
    pump_events(app);
    grid = expected_grid(surface, provider);
    ok &= check(grid.status == term::Terminal_metrics_status::OK &&
        grid_matches_surface(surface, grid.grid_size) &&
        surface.rows() >= 1 &&
        surface.columns() >= 1,
        "surface publishes provider-derived grid for tiny positive geometry");

    surface.setSize(QSizeF(360.0, 180.0));
    pump_events(app);
    provider.set_font(terminal_font(surface.font_family(), surface.font_size()));
    const std::optional<qreal> changed_pixel_size = pixel_size_with_different_grid(
        provider,
        surface.font_family(),
        surface.font_size(),
        surface.size());
    ok &= check(changed_pixel_size.has_value(),
        "surface signal test can find font size with a different provider grid");
    if (!changed_pixel_size.has_value()) {
        return false;
    }

    const int grid_signals_before_font_change = grid_signal_count;
    surface.set_font_size(*changed_pixel_size);
    pump_events(app);
    ok &= check(grid_signal_count >= grid_signals_before_font_change + 1,
        "font-size grid change emits grid signal");

    provider.set_font(terminal_font(surface.font_family(), surface.font_size()));
    grid = expected_grid(surface, provider);
    ok &= check(grid.status == term::Terminal_metrics_status::OK &&
        grid_matches_surface(surface, grid.grid_size),
        "surface grid remains provider-derived after font-size change");

    surface.set_font_size(0.0);
    pump_events(app);
    ok &= check(surface.rows() == 0 && surface.columns() == 0,
        "surface publishes zero grid for zero font size");

    surface.set_font_size(std::numeric_limits<qreal>::quiet_NaN());
    pump_events(app);
    ok &= check(surface.rows() == 0 && surface.columns() == 0,
        "surface publishes zero grid for NaN font size");

    const int font_size_signals_before_nan_noop = font_size_signal_count;
    surface.set_font_size(std::numeric_limits<qreal>::quiet_NaN());
    pump_events(app);
    ok &= check(font_size_signal_count == font_size_signals_before_nan_noop,
        "font-size setter treats repeated NaN as a no-op");

    return ok;
}

bool test_controller_with_real_provider(QGuiApplication& app, qreal device_pixel_ratio)
{
    bool ok = true;

    QQuickWindow window;
    window.resize(420, 220);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(420.0, 220.0));
    surface.set_font_family(QStringLiteral("monospace"));
    surface.set_font_size(13.0);

    window.show();
    pump_events(app);

    term::Qt_grid_metrics_provider provider(
        terminal_font(surface.font_family(), surface.font_size()),
        device_pixel_ratio);
    const term::Terminal_metrics_result initial_grid = expected_grid(surface, provider);
    ok &= check(initial_grid.status == term::Terminal_metrics_status::OK,
        "controller test initial grid is valid");
    ok &= check(grid_matches_surface(surface, initial_grid.grid_size),
        "initial surface rows and columns are provider-derived before spawn");

    auto               backend     = std::make_unique<Recording_backend>();
    Recording_backend* backend_ptr = backend.get();
    term::Terminal_session session(std::move(backend), traced_session_config());
    term::Terminal_resize_controller controller(session, provider);

    auto               invalid_start_backend     = std::make_unique<Recording_backend>();
    Recording_backend* invalid_start_backend_ptr = invalid_start_backend.get();
    term::Terminal_session invalid_start_session(
        std::move(invalid_start_backend),
        traced_session_config());
    term::Terminal_resize_controller invalid_start_controller(
        invalid_start_session,
        provider);
    const term::Terminal_session_result invalid_start =
        invalid_start_controller.start_from_geometry(valid_launch_config(), QSizeF());
    ok &= check(invalid_start.code == term::Terminal_session_result_code::INVALID_ARGUMENT &&
        invalid_start.error.has_value() &&
        invalid_start.error->code ==
            term::Terminal_backend_error_code::INVALID_INITIAL_GRID_SIZE,
        "real-provider start rejects zero geometry before backend start");
    ok &= check(invalid_start_backend_ptr->start_configs.empty(),
        "invalid start geometry does not reach backend");

    const term::Terminal_session_result start_result =
        controller.start_from_geometry(valid_launch_config(), surface.size());
    ok &= check(start_result.code == term::Terminal_session_result_code::ACCEPTED,
        "real-provider controller start is accepted");
    ok &= check(backend_ptr->start_configs.size() == 1U,
        "real-provider controller starts backend once");
    ok &= check(backend_ptr->start_configs.front().initial_grid_size.has_value() &&
        backend_ptr->start_configs.front().initial_grid_size->rows ==
            initial_grid.grid_size.rows &&
        backend_ptr->start_configs.front().initial_grid_size->columns ==
            initial_grid.grid_size.columns,
        "backend start receives provider-derived initial grid");
    ok &= check(backend_ptr->resize_requests.empty(),
        "controller start does not send resize request");

    const std::optional<QSizeF> changed_geometry =
        geometry_with_different_grid(provider, surface.size());
    ok &= check(changed_geometry.has_value(),
        "test can find geometry with a different provider grid");
    if (!changed_geometry.has_value()) {
        return false;
    }

    surface.setSize(*changed_geometry);
    pump_events(app);
    const term::Terminal_metrics_result resized_grid =
        provider.grid_size_for_item_geometry(surface.size());
    const term::Terminal_session_result resize_result =
        controller.resize_from_geometry(surface.size());
    ok &= check(resize_result.code == term::Terminal_session_result_code::ACCEPTED,
        "real-provider changed geometry resize is accepted");
    ok &= check(backend_ptr->resize_requests.size() == 1U &&
        backend_ptr->resize_requests.back().grid_size.rows == resized_grid.grid_size.rows &&
        backend_ptr->resize_requests.back().grid_size.columns ==
            resized_grid.grid_size.columns,
        "changed geometry sends one provider-derived resize request");
    ok &= check(session.resize_transactions().size() == 1U,
        "changed geometry records one resize transaction");
    const std::uint64_t first_resize_id = session.resize_transactions().back().id;

    const term::Terminal_session_result same_grid_refresh =
        controller.refresh_from_geometry(surface.size());
    ok &= check(same_grid_refresh.code == term::Terminal_session_result_code::ACCEPTED,
        "same-grid refresh is accepted");
    ok &= check(backend_ptr->resize_requests.size() == 1U,
        "same-grid refresh sends no backend resize");
    ok &= check(session.resize_transactions().size() == 1U,
        "same-grid refresh records no resize transaction");

    const std::size_t resize_requests_before_invalid = backend_ptr->resize_requests.size();
    const term::Terminal_session_result invalid_resize =
        controller.resize_from_geometry(QSizeF());
    ok &= check(invalid_resize.code == term::Terminal_session_result_code::INVALID_ARGUMENT &&
        invalid_resize.error.has_value() &&
        invalid_resize.error->code == term::Terminal_backend_error_code::RESIZE_FAILED,
        "real-provider resize rejects zero geometry before backend resize");
    const term::Terminal_session_result invalid_refresh =
        controller.refresh_from_geometry(QSizeF());
    ok &= check(invalid_refresh.code == term::Terminal_session_result_code::INVALID_ARGUMENT &&
        invalid_refresh.error.has_value() &&
        invalid_refresh.error->code == term::Terminal_backend_error_code::RESIZE_FAILED,
        "real-provider refresh rejects zero geometry before backend resize");
    ok &= check(backend_ptr->resize_requests.size() == resize_requests_before_invalid,
        "invalid resize and refresh geometry do not reach backend");

    const std::optional<qreal> changed_pixel_size = pixel_size_with_different_grid(
        provider,
        surface.font_family(),
        surface.font_size(),
        surface.size());
    ok &= check(changed_pixel_size.has_value(),
        "test can find font size with a different provider grid");
    if (!changed_pixel_size.has_value()) {
        return false;
    }

    surface.set_font_size(*changed_pixel_size);
    pump_events(app);
    const term::Terminal_metrics_result font_grid =
        provider.grid_size_for_item_geometry(surface.size());
    const term::Terminal_session_result font_refresh =
        controller.refresh_from_geometry(surface.size());
    ok &= check(font_refresh.code == term::Terminal_session_result_code::ACCEPTED,
        "font-size provider grid refresh is accepted");
    ok &= check(backend_ptr->resize_requests.size() == 2U &&
        backend_ptr->resize_requests.back().grid_size.rows == font_grid.grid_size.rows &&
        backend_ptr->resize_requests.back().grid_size.columns == font_grid.grid_size.columns,
        "font-size grid change sends provider-derived resize request");
    ok &= check(session.resize_transactions().size() == 2U &&
        session.resize_transactions().back().id > first_resize_id,
        "resize transaction ids are monotonic");
    ok &= check(backend_ptr->resize_requests.back().transaction_id > first_resize_id,
        "backend resize request ids are monotonic");

    return ok;
}

bool test_dpr_expectation(const QStringList& arguments, qreal observed_dpr)
{
    if (!std::isfinite(observed_dpr) || observed_dpr <= 0.0) {
        std::cerr << "FAIL: Qt metrics entry observed invalid DPR " << observed_dpr << '\n';
        return false;
    }

    if (arguments.contains(QStringLiteral("--expect-scaled"))) {
        if (observed_dpr <= 1.0) {
            std::cerr << "FAIL: scaled Qt metrics entry observed DPR " << observed_dpr
                << ", expected a value greater than 1.0\n";
            return false;
        }
        return true;
    }

    return true;
}

std::string metric_message(
    std::string_view object_name,
    std::string_view expectation,
    const char*      key)
{
    std::string message(object_name);
    message += expectation;
    message += key;
    return message;
}

bool expected_runtime_metric_key(
    const QString&                    key,
    std::initializer_list<const char*> string_keys,
    std::initializer_list<const char*> bool_keys)
{
    for (const char* expected_key : string_keys) {
        if (key == QString::fromLatin1(expected_key)) {
            return true;
        }
    }
    for (const char* expected_key : bool_keys) {
        if (key == QString::fromLatin1(expected_key)) {
            return true;
        }
    }
    return false;
}

bool check_runtime_metric_object(
    const QJsonObject&                 object,
    std::string_view                   object_name,
    std::initializer_list<const char*> string_keys,
    std::initializer_list<const char*> bool_keys)
{
    bool ok = true;

    const int expected_size =
        static_cast<int>(string_keys.size() + bool_keys.size());
    ok &= check(
        object.size() == expected_size,
        std::string(object_name) + " metrics expose the expected field count");

    for (const char* key : string_keys) {
        const QString json_key = QString::fromLatin1(key);
        ok &= check(
            object.contains(json_key),
            metric_message(object_name, " metrics include string counter ", key));
        ok &= check(
            object.value(json_key).isString(),
            metric_message(object_name, " metrics type string counter ", key));
    }

    for (const char* key : bool_keys) {
        const QString json_key = QString::fromLatin1(key);
        ok &= check(
            object.contains(json_key),
            metric_message(object_name, " metrics include bool field ", key));
        ok &= check(
            object.value(json_key).isBool(),
            metric_message(object_name, " metrics type bool field ", key));
    }

    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        ok &= check(
            expected_runtime_metric_key(it.key(), string_keys, bool_keys),
            std::string(object_name) +
                " metrics do not include unexpected field " +
                it.key().toStdString());
    }

    return ok;
}

bool test_diagnostics_metrics_json(QGuiApplication& app)
{
    bool ok = true;

    QQuickWindow window;
    window.setColor(QColor(32, 32, 32));
    window.resize(360, 180);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(360.0, 180.0));
    surface.set_font_family(QStringLiteral("monospace"));
    surface.set_font_size(14.0);

    window.show();
    pump_events(app);

    QJsonObject renderer;
    vnm_terminal::diagnostics::append_renderer_metrics_json(surface, renderer);
    ok &= check(!renderer.isEmpty(),
        "append_renderer_metrics_json fills the renderer object");
    ok &= check(renderer.contains(QStringLiteral("frames_published")),
        "renderer metrics include frames_published");
    ok &= check(renderer.contains(QStringLiteral("paint_completed_frames")),
        "renderer metrics include paint_completed_frames");
    ok &= check(renderer.contains(QStringLiteral("frame")),
        "renderer metrics include the nested frame object");
    const QJsonObject renderer_frame =
        renderer.value(QStringLiteral("frame")).toObject();
    ok &= check(renderer_frame.contains(QStringLiteral("row_descriptors_built")),
        "renderer frame metrics include row descriptor builds");
    ok &= check(renderer_frame.contains(QStringLiteral("layer_descriptors_built")),
        "renderer frame metrics include layer descriptor builds");
    ok &= check(renderer.contains(QStringLiteral("text_resource_descriptor_builds")),
        "renderer metrics include text resource descriptor builds");
    ok &= check(renderer.contains(QStringLiteral("text_resource_descriptor_builds_avoided")),
        "renderer metrics include avoided text resource descriptor builds");
    ok &= check(renderer.contains(QStringLiteral("text_resource_descriptor_reuses")),
        "renderer metrics include text resource descriptor reuses");
    const auto renderer_qsg_layer_descriptors =
        renderer.value(QStringLiteral("qsg_layer_descriptors"));
    ok &= check(
        renderer_qsg_layer_descriptors.isString() &&
            renderer_qsg_layer_descriptors.toString() == QStringLiteral("0"),
        "renderer metrics report zero QSG layer descriptors");

    QJsonObject atlas;
    vnm_terminal::diagnostics::append_atlas_metrics_json(surface, atlas);
    ok &= check(!atlas.isEmpty(),
        "append_atlas_metrics_json fills the qsg_atlas object");
    ok &= check(atlas.contains(QStringLiteral("render_count")),
        "atlas metrics include render_count");
    ok &= check(atlas.value(QStringLiteral("renderer")).toString() == QStringLiteral("atlas"),
        "atlas metrics tag the renderer as atlas");
    ok &= check(atlas.contains(QStringLiteral("frame_row_descriptors")),
        "atlas metrics include frame row descriptors");
    ok &= check(atlas.contains(QStringLiteral("frame_layer_descriptors")),
        "atlas metrics include frame layer descriptors");
    const auto atlas_qsg_layer_descriptors =
        atlas.value(QStringLiteral("qsg_layer_descriptors"));
    ok &= check(
        atlas_qsg_layer_descriptors.isString() &&
            atlas_qsg_layer_descriptors.toString() == QStringLiteral("0"),
        "atlas metrics report zero QSG layer descriptors");
    ok &= check(atlas.contains(QStringLiteral("producer")),
        "atlas metrics include producer summary");
    ok &= check(atlas.contains(QStringLiteral("warm_lazy")),
        "atlas metrics include warm-lazy summary");
    ok &= check(atlas.contains(QStringLiteral("buffer_upload")),
        "atlas metrics include buffer upload summary");
    const QJsonObject buffer_upload =
        atlas.value(QStringLiteral("buffer_upload")).toObject();
    ok &= check(
        buffer_upload.value(QStringLiteral("rect_row_capacity")).isString(),
        "atlas buffer upload metrics include rect row capacity counter");
    ok &= check(
        buffer_upload.value(QStringLiteral("glyph_text_row_capacity")).isString(),
        "atlas buffer upload metrics include glyph text row capacity counter");
    ok &= check(
        buffer_upload.value(
            QStringLiteral("glyph_cursor_text_row_capacity")).isString(),
        "atlas buffer upload metrics include glyph cursor-text row capacity counter");

    QJsonObject render_invalidation;
    vnm_terminal::diagnostics::append_render_invalidation_metrics_json(
        surface,
        render_invalidation);
    ok &= check(!render_invalidation.isEmpty(),
        "append_render_invalidation_metrics_json fills the runtime object");
    ok &= check_runtime_metric_object(
        render_invalidation,
        "render invalidation",
        {
            "update_requests",
            "scheduled_updates",
            "coalesced_requests",
            "consumed_updates",
            "render_snapshot_callback_epoch",
            "last_rendered_snapshot_sequence",
            "last_rendered_publication_generation",
        },
        {
            "pending_update",
        });

    QJsonObject backend_drain;
    vnm_terminal::diagnostics::append_backend_drain_metrics_json(
        surface,
        backend_drain);
    ok &= check(!backend_drain.isEmpty(),
        "append_backend_drain_metrics_json fills the runtime object");
    ok &= check_runtime_metric_object(
        backend_drain,
        "backend drain",
        {
            "total_drain_calls",
            "budgeted_drain_calls",
            "unbudgeted_drain_calls",
            "posted_drain_calls",
            "posted_full_budget_calls",
            "posted_frame_pending_small_budget_calls",
            "budget_exhausted_incomplete",
            "cursor_stable_incomplete",
            "total_elapsed_ns",
            "max_elapsed_ns",
            "session_processing_calls",
            "session_processing_elapsed_ns",
            "session_processing_max_elapsed_ns",
            "sync_from_session_calls",
            "sync_from_session_elapsed_ns",
            "sync_from_session_max_elapsed_ns",
            "frame_work_pending_drain_calls",
            "frame_work_pending_elapsed_ns",
            "render_update_pending_drain_calls",
            "atlas_completion_pending_drain_calls",
            "requeue_count",
            "pending_callback_after_drain",
            "output_backpressure_after_drain",
        },
        {});

    return ok;
}

// Prove the public font/metrics API replicates the internal Qt_grid_metrics_provider
// computation exactly, so a consumer that drops the internal includes sees no
// behavior change. Equality is exact (not tolerance-based): the public path must
// produce byte-identical width/height to the internal provider.
bool test_public_font_metrics_replicates_internal()
{
    bool ok = true;

    ok &= check(
        vnm_terminal::default_monospace_font_family() ==
            term::vnm_terminal_default_monospace_font_family(),
        "public default family equals internal default family");

    struct font_case_t {
        const char* family;
        qreal       pixel_size;
        qreal       device_pixel_ratio;
    };
    const font_case_t cases[] = {
        {"monospace", 12.0, 1.0},
        {"monospace", 18.0, 2.0},
        {"monospace", 13.0, 1.5},
    };

    for (const font_case_t& font_case : cases) {
        const QString family = QString::fromLatin1(font_case.family);

        const term::Qt_grid_metrics_provider provider(
            terminal_font(family, font_case.pixel_size),
            font_case.device_pixel_ratio);
        const term::terminal_cell_metrics_t internal_metrics = provider.cell_metrics();

        const vnm_terminal::Cell_metrics public_metrics =
            vnm_terminal::cell_metrics_for_font(
                family,
                font_case.pixel_size,
                font_case.device_pixel_ratio);

        ok &= check(public_metrics.width == internal_metrics.width,
            "public cell width equals internal provider cell width");
        ok &= check(public_metrics.height == internal_metrics.height,
            "public cell height equals internal provider cell height");

        ok &= check(
            vnm_terminal::cell_metrics_valid(public_metrics) ==
                term::is_valid_cell_metrics(internal_metrics),
            "public cell_metrics_valid agrees with internal is_valid_cell_metrics");
    }

    const vnm_terminal::Cell_metrics zero_metrics;
    ok &= check(!vnm_terminal::cell_metrics_valid(zero_metrics),
        "public cell_metrics_valid rejects zero-sized metrics");

    return ok;
}

}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    const QStringList arguments = app.arguments();

    QQuickWindow dpr_window;
    dpr_window.resize(64, 64);
    dpr_window.show();
    pump_events(app);
    const qreal observed_dpr = dpr_window.effectiveDevicePixelRatio();
    bool        ok           = true;
    ok &= test_dpr_expectation(arguments, observed_dpr);
    ok &= test_provider_metrics(observed_dpr);
    ok &= test_surface_publication(app, observed_dpr);
    ok &= test_controller_with_real_provider(app, observed_dpr);
    ok &= test_diagnostics_metrics_json(app);
    ok &= test_public_font_metrics_replicates_internal();
    return ok ? 0 : 1;
}
