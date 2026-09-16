#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>

// Prints the error mode this process was created with. It is built without the
// test-process setup, which replaces the error mode during static
// initialization and would hide the mode the terminal backend handed down.
int main()
{
    std::printf("error-mode=%u\n", static_cast<unsigned>(GetErrorMode()));
    return std::fflush(stdout) == 0 ? 0 : 1;
}
