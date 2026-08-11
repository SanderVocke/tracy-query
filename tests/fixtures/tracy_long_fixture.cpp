#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <thread>

#include "tracy/Tracy.hpp"

int main() {
    tracy::SetThreadName("fixture-long");
    for (int i = 0; i < 500 && !TracyIsConnected; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!TracyIsConnected) return 2;
    for (int i = 0; i < 300; ++i) {
        {
            ZoneScopedN("fixture.long.tick");
            TracyPlot("fixture.long.iteration", static_cast<std::int64_t>(i));
            if (i == 0) TracyMessageL("fixture long connected");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return 0;
}
