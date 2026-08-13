#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include <tracy/Tracy.hpp>
#include <tracy/TracyC.h>
#include <client/TracyProfiler.hpp>

#include "tracy_embedded_capture/embedded_capture.h"

extern "C" void ___tracy_embedded_capture_test_force_finish_timeout(void);
extern "C" void ___tracy_embedded_capture_test_force_writer_open_failure(void);

namespace {

void emit_events() {
    tracy::SetThreadName("Embedded fixture main");
    {
        ZoneScopedNS("embedded.fixture.root", 8);
        TracyMessageL("embedded.fixture.message");
        TracyPlot("embedded.fixture.plot", 42.0);
        FrameMarkNamed("embedded.fixture.frame");

        TracyLockable(std::mutex, fixtureLock);
        {
            std::lock_guard lock(fixtureLock);
            ZoneScopedN("embedded.fixture.locked");
        }

        auto* memory = new std::uint8_t[64];
        TracyAllocN(memory, 64, "embedded.fixture.memory");
        TracyFreeN(memory, "embedded.fixture.memory");
        delete[] memory;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: tracy-embedded-fixture OUTPUT.tracy [discard|existing-at-finish|expect-io-error|writer-open-failure|finish-timeout]\n");
        return 2;
    }
    const std::string mode = argc == 3 ? argv[2] : "success";
    std::error_code ignored;
    if (mode == "expect-io-error") {
        std::filesystem::remove_all(std::filesystem::path(argv[1]).parent_path(), ignored);
    } else if (mode != "success") {
        std::filesystem::remove(argv[1], ignored);
    }
    const auto pathLength = std::char_traits<char>::length(argv[1]);
    auto status = ___tracy_embedded_capture_configure(
        argv[1], pathLength, 256 * 1024, 256LL * 1024 * 1024);
    if (status != TRACY_EMBEDDED_CAPTURE_OK) return status + 10;

    ___tracy_startup_profiler();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (___tracy_embedded_capture_get_state() != TRACY_EMBEDDED_CAPTURE_CAPTURING) {
        if (std::chrono::steady_clock::now() >= deadline) return 30;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    emit_events();
    if (mode == "existing-at-finish") {
        std::ofstream(argv[1]) << "sentinel";
    } else if (mode == "writer-open-failure") {
        ___tracy_embedded_capture_test_force_writer_open_failure();
    } else if (mode == "finish-timeout") {
        ___tracy_embedded_capture_test_force_finish_timeout();
    }
    status = mode == "discard"
                 ? ___tracy_embedded_capture_finish_with_disposition(
                       TRACY_EMBEDDED_CAPTURE_DISCARD)
                 : ___tracy_embedded_capture_finish();
    if (mode == "discard") {
        tracy_embedded_capture_statistics statistics{};
        const auto statisticsStatus =
            ___tracy_embedded_capture_get_statistics(&statistics);
        return status == TRACY_EMBEDDED_CAPTURE_OK &&
                       statisticsStatus == TRACY_EMBEDDED_CAPTURE_OK &&
                       ___tracy_embedded_capture_get_state() ==
                           TRACY_EMBEDDED_CAPTURE_DISCARDED &&
                       statistics.writer_open_count == 0 &&
                       statistics.worker_write_count == 0 &&
                       statistics.publish_count == 0 &&
                       !std::filesystem::exists(argv[1])
                   ? 0
                   : 33;
    }
    if (mode == "existing-at-finish") {
        std::ifstream input(argv[1]);
        std::string contents;
        input >> contents;
        return status == TRACY_EMBEDDED_CAPTURE_OUTPUT_EXISTS && contents == "sentinel" ? 0 : 31;
    }
    if (mode == "expect-io-error") {
        return status == TRACY_EMBEDDED_CAPTURE_IO_ERROR &&
                       !std::filesystem::exists(argv[1])
                   ? 0
                   : 32;
    }
    if (mode == "writer-open-failure") {
        tracy_embedded_capture_statistics statistics{};
        ___tracy_embedded_capture_get_statistics(&statistics);
        const auto partialPattern = std::filesystem::path(argv[1]).filename().string() + ".";
        bool partialExists = false;
        const auto parent = std::filesystem::path(argv[1]).parent_path();
        for (const auto& item : std::filesystem::directory_iterator(parent.empty() ? "." : parent)) {
            const auto name = item.path().filename().string();
            partialExists = partialExists ||
                            (name.starts_with(partialPattern) && name.ends_with(".partial"));
        }
        return status == TRACY_EMBEDDED_CAPTURE_IO_ERROR &&
                       statistics.writer_open_count == 1 &&
                       statistics.worker_write_count == 0 &&
                       statistics.publish_count == 0 &&
                       !std::filesystem::exists(argv[1]) && !partialExists
                   ? 0
                   : 35;
    }
    if (mode == "finish-timeout") {
        tracy_embedded_capture_statistics statistics{};
        ___tracy_embedded_capture_get_statistics(&statistics);
        return status == TRACY_EMBEDDED_CAPTURE_TRANSPORT_ERROR &&
                       ___tracy_embedded_capture_get_state() ==
                           TRACY_EMBEDDED_CAPTURE_FAILED &&
                       statistics.writer_open_count == 0 &&
                       statistics.worker_write_count == 0 &&
                       statistics.publish_count == 0 &&
                       !std::filesystem::exists(argv[1])
                   ? 0
                   : 34;
    }
    if (status != TRACY_EMBEDDED_CAPTURE_OK) {
        char error[512]{};
        ___tracy_embedded_capture_get_error(error, sizeof(error));
        std::fprintf(stderr, "embedded capture failed (%d): %s\n", status, error);
        return status + 40;
    }
    return 0;
}
