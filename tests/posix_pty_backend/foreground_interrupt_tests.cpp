#include "foreground_interrupt_fixture.h"

int main(int argc, char** argv)
{
    return vnm_terminal::test_helpers::run_foreground_interrupt_tests(
        argc, argv, vnm_terminal::test_helpers::make_default_test_backend);
}
