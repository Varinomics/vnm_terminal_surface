#include "exit_output_fixture.h"

int main(int argc, char** argv)
{
    return vnm_terminal::test_helpers::run_exit_output_tests(
        argc, argv, {vnm_terminal::test_helpers::make_default_test_backend, {}, {}});
}
