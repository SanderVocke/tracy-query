#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

#include <tracy/Tracy.hpp>
#include <client/TracyProfiler.hpp>

#include "TracyFileWrite.hpp"
#include "TracyWorker.hpp"

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const char* portText = std::getenv("TRACY_PORT");
    if (!portText) return 3;
    const auto port = static_cast<std::uint16_t>(std::strtoul(portText, nullptr, 10));

    tracy::Worker worker("127.0.0.1", port, 256LL * 1024 * 1024);
    TracyMessageL("loopback startup");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!worker.HasData()) {
        if (std::chrono::steady_clock::now() >= deadline) return 4;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    {
        ZoneScopedN("embedded.fixture.root");
        TracyMessageL("embedded.fixture.message");
        TracyPlot("embedded.fixture.plot", 42.0);
        FrameMarkNamed("embedded.fixture.frame");
        TracyLockable(std::mutex, fixtureLock);
        {
            std::lock_guard lock(fixtureLock);
            LockMark(fixtureLock);
        }
        auto* memory = new std::uint8_t[64];
        TracyAllocN(memory, 64, "embedded.fixture.memory");
        TracyFreeN(memory, "embedded.fixture.memory");
        delete[] memory;
    }

    tracy::GetProfiler().RequestShutdown();
    while (!tracy::GetProfiler().HasShutdownFinished()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    while (worker.IsConnected()) std::this_thread::sleep_for(std::chrono::milliseconds(1));

    std::unique_ptr<tracy::FileWrite> writer(
        tracy::FileWrite::Open(argv[1], tracy::FileCompression::Zstd, 3, 1));
    if (!writer) return 5;
    worker.Write(*writer, false);
    writer.reset();
    return 0;
}
