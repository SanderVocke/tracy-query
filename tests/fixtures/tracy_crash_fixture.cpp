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
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::abort();
}
