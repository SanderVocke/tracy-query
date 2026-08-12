#include <chrono>
#include <cstdlib>
#include <thread>

#include "tracy/Tracy.hpp"

int main() {
    tracy::SetThreadName("fixture-crash");
    for (int i = 0; i < 500 && !TracyIsConnected; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!TracyIsConnected) return 2;
    TracyMessageL("fixture crash incoming");
    // Allow the collector to resolve and consume the deterministic marker before
    // this fixture exits without Tracy shutdown, including under sanitizers.
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    // Exit without running Tracy shutdown or process destructors. This models an
    // abrupt crash without allowing Tracy's optional signal handler to wait for
    // a collector acknowledgement (which is platform-dependent in a fixture).
    std::_Exit(134);
}
