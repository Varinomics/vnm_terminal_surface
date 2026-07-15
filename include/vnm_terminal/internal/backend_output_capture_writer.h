#pragma once

#include "vnm_terminal/backend_output_capture.h"

#include <QByteArrayView>
#include <QString>
#include <memory>

namespace vnm_terminal::internal {

struct Backend_output_capture_writer_result
{
    bool                        accepted = false;
    QString                     error;
};

#if defined(VNM_TERMINAL_CAPTURE_TEST_HOOKS)
enum class Backend_output_capture_test_fault : std::uint8_t
{
    NONE,
    PARTIAL_WRITE,
    FLUSH_AFTER_WRITE,
    FAIL_AFTER_SEGMENT_FINALIZATION,
};
#endif

class Backend_output_capture_writer final
{
public:
    explicit Backend_output_capture_writer(
        Backend_output_capture_config config);

    ~Backend_output_capture_writer();

    Backend_output_capture_writer(const Backend_output_capture_writer&) = delete;
    Backend_output_capture_writer& operator=(const Backend_output_capture_writer&) = delete;

    Backend_output_capture_writer_result append(
        QByteArrayView              bytes);

    Backend_output_capture_writer_result finalize();

#if defined(VNM_TERMINAL_CAPTURE_TEST_HOOKS)
    void set_test_fault(Backend_output_capture_test_fault fault);
#endif

private:
    class State;
    std::unique_ptr<State>      m_state;
};

}
