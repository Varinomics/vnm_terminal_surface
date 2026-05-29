#pragma once

#include "vnm_terminal/internal/terminal_screen_model.h"

#include <vector>

namespace vnm_terminal::test_helpers {

enum class Primary_backing_boundary
{
    INGEST,
    RESIZE,
    ALTERNATE_ENTER,
    ALTERNATE_LEAVE,
    SCROLLBACK_CLEAR,
    SCROLLBACK_LIMIT_CHANGE,
};

enum class Primary_backing_observation_classification
{
    TEST_ONLY,
};

enum class Scrollback_delta_operation_annotation
{
    TERMINAL_SCROLL,
    SCROLL_REGION_WITHOUT_HISTORY_APPEND,
    RESIZE,
    CLEAR_SCROLLBACK,
    ALTERNATE_ENTER_LEAVE,
    REPAINT,
    RECOVERY,
};

struct scrollback_delta_observation_t
{
    Primary_backing_boundary                      boundary_annotation = Primary_backing_boundary::INGEST;
    internal::Terminal_buffer_id                  active_buffer_before = internal::Terminal_buffer_id::PRIMARY;
    internal::Terminal_buffer_id                  active_buffer_after = internal::Terminal_buffer_id::PRIMARY;
    int                                           scrollback_rows_before = 0;
    int                                           scrollback_rows_after = 0;
    int                                           result_scrollback_rows = 0;
    int                                           evicted_scrollback_rows = 0;
    std::vector<internal::terminal_backing_delta_t>
                                                  backing_deltas;
    Scrollback_delta_operation_annotation          operation_annotation =
        Scrollback_delta_operation_annotation::TERMINAL_SCROLL;
    Primary_backing_observation_classification    classification_annotation =
        Primary_backing_observation_classification::TEST_ONLY;
    bool                                          viewport_changed = false;
    bool                                          recovery_enabled_annotation = false;
};

inline int scrollback_rows_delta(const scrollback_delta_observation_t& observation)
{
    return observation.scrollback_rows_after - observation.scrollback_rows_before;
}

class Scrollback_delta_observer
{
public:
    explicit Scrollback_delta_observer(bool recovery_enabled)
    :
        m_recovery_enabled(recovery_enabled)
    {}

    template <typename Operation>
    scrollback_delta_observation_t observe(
        internal::Terminal_screen_model&      model,
        Primary_backing_boundary              boundary_annotation,
        Scrollback_delta_operation_annotation operation_annotation,
        Operation                             operation) const
    {
        scrollback_delta_observation_t observation;
        observation.boundary_annotation     = boundary_annotation;
        observation.active_buffer_before    = model.active_buffer_id();
        observation.scrollback_rows_before  = model.scrollback_size();
        observation.operation_annotation    = operation_annotation;
        observation.classification_annotation =
            Primary_backing_observation_classification::TEST_ONLY;
        observation.recovery_enabled_annotation = m_recovery_enabled;

        const internal::Terminal_screen_model_result result = operation();

        observation.active_buffer_after     = model.active_buffer_id();
        observation.scrollback_rows_after   = model.scrollback_size();
        observation.result_scrollback_rows  = result.scrollback_rows;
        observation.evicted_scrollback_rows = result.evicted_scrollback_rows;
        observation.backing_deltas          = result.backing_deltas;
        observation.viewport_changed        = result.viewport_changed;
        return observation;
    }

private:
    bool m_recovery_enabled = false;
};

} // namespace vnm_terminal::test_helpers
