#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include "tracy/Tracy.hpp"
#include "client/TracyProfiler.hpp"

static TracyLockable(std::mutex, fixture_mutex);

int main(int argc, char** argv) {
    tracy::SetThreadName("fixture-main");
    for (int i = 0; i < 500 && !TracyIsConnected; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!TracyIsConnected) return 2;

    TracyMessageL("fixture message");
    if (argc > 1) TracyMessage(argv[1], std::strlen(argv[1]));
    TracyPlot("fixture.plot", 1.25);
    FrameMarkStart("fixture.frame");
    {
        ZoneScopedN("fixture.root");
        ZoneText("fixture zone text", 17);
        {
            ZoneScopedN("fixture.child");
            std::lock_guard<LockableBase(std::mutex)> guard(fixture_mutex);
            LockMark(fixture_mutex);
            auto* memory = std::malloc(64);
            TracyAllocN(memory, 64, "fixture.pool");
            TracyFreeN(memory, "fixture.pool");
            std::free(memory);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    FrameMarkEnd("fixture.frame");
    const unsigned char image[64] = {
        255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255,
        255, 255, 0, 255, 0, 255, 255, 255, 255, 0, 255, 255, 64, 64, 64, 255,
        128, 0, 0, 255, 0, 128, 0, 255, 0, 0, 128, 255, 128, 128, 128, 255,
        255, 128, 0, 255, 0, 128, 255, 255, 128, 0, 255, 255, 32, 32, 32, 255,
    };
    FrameImage(image, 4, 4, 0, false);
    FrameMark;
    TracyPlot("fixture.plot", 2.5);
    TracyMessageL("fixture done");
    const int shutdown_delay_ms = argc > 2 ? std::max(0, std::atoi(argv[2])) : 200;
    std::this_thread::sleep_for(std::chrono::milliseconds(shutdown_delay_ms));
    tracy::GetProfiler().RequestShutdown();
    while (!tracy::GetProfiler().HasShutdownFinished()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 0;
}
