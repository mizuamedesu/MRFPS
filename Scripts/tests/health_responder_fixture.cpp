#include "../../Source/MRFPS/Public/MRFPSHealthResponder.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <poll.h>

// Small Linux-only fixture; no Unreal runtime or network port is started.
int main(int argc, char** argv)
{
    if (argc != 4) return 2;
    const int Port = std::atoi(argv[1]);
    const bool bReady = std::atoi(argv[2]) != 0;
    const bool bTick = std::atoi(argv[3]) != 0;
    FMRFPSHealthResponder Responder;
    if (!Responder.Start()) return 3;
    FMRFPSHealthResponder Duplicate;
    if (Duplicate.Start()) return 5;
    std::puts("READY");
    std::fflush(stdout);
    for (;;) {
        pollfd Input{0, POLLIN | POLLHUP, 0};
        if (::poll(&Input, 1, 5) < 0 || (Input.revents & (POLLIN | POLLHUP | POLLERR))) break;
        timespec Now{};
        if (::clock_gettime(CLOCK_MONOTONIC, &Now) != 0) return 4;
        const auto Milliseconds = static_cast<std::uint64_t>(Now.tv_sec) * 1000 + Now.tv_nsec / 1000000;
        if (bTick) Responder.Tick(bReady, Port, Milliseconds);
    }
    return 0;
}
