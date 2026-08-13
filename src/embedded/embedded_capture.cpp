#include "tracy_embedded_capture/embedded_capture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "TracyEmbeddedTransport.hpp"
#include "TracyFileWrite.hpp"
#include "TracyProtocol.hpp"
#include "TracyVersion.hpp"
#include "TracyWorker.hpp"
#include <tracy/TracyC.h>

namespace {

static_assert(tracy::ProtocolVersion == 76, "embedded capture requires Tracy protocol 76");
static_assert(tracy::Version::Major == 0 && tracy::Version::Minor == 13 &&
                  tracy::Version::Patch == 1,
              "embedded capture requires Tracy 0.13.1");

using State = tracy_embedded_capture_state;
using Status = tracy_embedded_capture_status;

struct Coordinator {
    std::mutex mutex;
    State state = TRACY_EMBEDDED_CAPTURE_UNCONFIGURED;
    std::filesystem::path output;
    std::unique_ptr<tracy::Worker> worker;
    std::string error;
    std::uint64_t writerOpenCount = 0;
    std::uint64_t workerWriteCount = 0;
    std::uint64_t publishCount = 0;
};

Coordinator& coordinator() {
    static Coordinator instance;
    return instance;
}

#ifdef TRACY_EMBEDDED_CAPTURE_TESTING
std::atomic<bool> forceFinishTimeout = false;
#endif

void setFailure(Coordinator& value, std::string message) {
    std::lock_guard lock(value.mutex);
    value.error = std::move(message);
    value.state = TRACY_EMBEDDED_CAPTURE_FAILED;
}

bool validUtf8(const char* data, std::size_t size) {
    std::size_t i = 0;
    while (i < size) {
        const auto c = static_cast<unsigned char>(data[i]);
        std::size_t continuation = 0;
        if (c <= 0x7f) continuation = 0;
        else if (c >= 0xc2 && c <= 0xdf) continuation = 1;
        else if (c >= 0xe0 && c <= 0xef) continuation = 2;
        else if (c >= 0xf0 && c <= 0xf4) continuation = 3;
        else return false;
        if (i + continuation >= size) return false;
        for (std::size_t j = 1; j <= continuation; ++j) {
            if ((static_cast<unsigned char>(data[i + j]) & 0xc0) != 0x80) return false;
        }
        if (continuation == 2) {
            const auto c1 = static_cast<unsigned char>(data[i + 1]);
            if ((c == 0xe0 && c1 < 0xa0) || (c == 0xed && c1 >= 0xa0)) return false;
        } else if (continuation == 3) {
            const auto c1 = static_cast<unsigned char>(data[i + 1]);
            if ((c == 0xf0 && c1 < 0x90) || (c == 0xf4 && c1 >= 0x90)) return false;
        }
        i += continuation + 1;
    }
    return true;
}

std::filesystem::path partialPath(const std::filesystem::path& output) {
    std::random_device random;
    for (int attempt = 0; attempt < 32; ++attempt) {
        const auto suffix = std::to_string(random()) + ".partial";
        auto candidate = output;
        candidate += "." + suffix;
        if (!std::filesystem::exists(candidate)) return candidate;
    }
    throw std::runtime_error("cannot allocate a unique partial capture path");
}

void atomicPublish(const std::filesystem::path& partial,
                   const std::filesystem::path& output) {
#ifdef _WIN32
    if (!MoveFileExW(partial.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH)) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(), "MoveFileExW");
    }
#else
    // Portable no-replace publication: creation of the final hard link is
    // atomic and fails with EEXIST rather than overwriting a racing writer.
    std::filesystem::create_hard_link(partial, output);
    std::filesystem::remove(partial);
#endif
}

int32_t finishImpl(const int32_t disposition) {
    auto& value = coordinator();
    tracy::Worker* worker = nullptr;
    std::filesystem::path output;
    {
        std::lock_guard lock(value.mutex);
        if (disposition != TRACY_EMBEDDED_CAPTURE_SAVE &&
            disposition != TRACY_EMBEDDED_CAPTURE_DISCARD) {
            value.error = "embedded capture disposition is invalid";
            return TRACY_EMBEDDED_CAPTURE_INVALID_ARGUMENT;
        }
        if (value.state != TRACY_EMBEDDED_CAPTURE_CONFIGURED &&
            value.state != TRACY_EMBEDDED_CAPTURE_CAPTURING) {
            value.error = "embedded capture finish called in an invalid state";
            return TRACY_EMBEDDED_CAPTURE_INVALID_STATE;
        }
        value.state = TRACY_EMBEDDED_CAPTURE_FINISHING;
        worker = value.worker.get();
        output = value.output;
    }

    if (!worker || !worker->HasData() || ___tracy_profiler_started() == 0) {
        tracy::embedded::Cancel();
        if (worker) worker->Disconnect();
        setFailure(value, "embedded capture has no completed Tracy handshake/data");
        return TRACY_EMBEDDED_CAPTURE_NO_DATA;
    }

    // The caller guarantees that all instrumentation producers and guards are
    // quiescent. Tracy's manual shutdown drains its queues and completes the
    // protocol termination exchange before destroying the client profiler.
    ___tracy_shutdown_profiler();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (worker->IsConnected() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#ifdef TRACY_EMBEDDED_CAPTURE_TESTING
    const bool timedOut = forceFinishTimeout.exchange(false, std::memory_order_relaxed);
#else
    constexpr bool timedOut = false;
#endif
    if (worker->IsConnected() || timedOut) {
        worker->Disconnect();
        tracy::embedded::Cancel();
        setFailure(value, timedOut
                              ? "embedded Worker termination timeout injected by test"
                              : "embedded Worker did not terminate within 30 seconds");
        return TRACY_EMBEDDED_CAPTURE_TRANSPORT_ERROR;
    }

    if (disposition == TRACY_EMBEDDED_CAPTURE_DISCARD) {
        std::unique_ptr<tracy::Worker> discardedWorker;
        {
            std::lock_guard lock(value.mutex);
            value.state = TRACY_EMBEDDED_CAPTURE_DISCARDED;
            value.error.clear();
            discardedWorker = std::move(value.worker);
        }
        discardedWorker.reset();
        return TRACY_EMBEDDED_CAPTURE_OK;
    }

    std::filesystem::path partial;
    try {
        if (std::filesystem::exists(output)) {
            setFailure(value, "capture output already exists");
            return TRACY_EMBEDDED_CAPTURE_OUTPUT_EXISTS;
        }
        const auto parent = output.has_parent_path() ? output.parent_path()
                                                      : std::filesystem::current_path();
        if (!std::filesystem::exists(parent) || !std::filesystem::is_directory(parent)) {
            setFailure(value, "capture output directory does not exist");
            return TRACY_EMBEDDED_CAPTURE_IO_ERROR;
        }
        partial = partialPath(output);
        {
            std::lock_guard lock(value.mutex);
            ++value.writerOpenCount;
        }
        std::unique_ptr<tracy::FileWrite> writer(tracy::FileWrite::Open(
            partial.string().c_str(), tracy::FileCompression::Zstd, 3, 1));
        if (!writer) throw std::runtime_error("TracyFileWrite cannot open partial capture");
        {
            std::lock_guard lock(value.mutex);
            ++value.workerWriteCount;
        }
        worker->Write(*writer, false);
        writer.reset();
        if (!std::filesystem::exists(partial) || std::filesystem::file_size(partial) == 0) {
            throw std::runtime_error("TracyFileWrite produced an empty capture");
        }
        atomicPublish(partial, output);
        {
            std::lock_guard lock(value.mutex);
            ++value.publishCount;
        }
    } catch (const std::exception& exception) {
        std::error_code ignored;
        if (!partial.empty()) std::filesystem::remove(partial, ignored);
        setFailure(value, exception.what());
        return TRACY_EMBEDDED_CAPTURE_IO_ERROR;
    } catch (...) {
        std::error_code ignored;
        if (!partial.empty()) std::filesystem::remove(partial, ignored);
        setFailure(value, "unknown capture writer failure");
        return TRACY_EMBEDDED_CAPTURE_INTERNAL_ERROR;
    }

    std::unique_ptr<tracy::Worker> completedWorker;
    {
        std::lock_guard lock(value.mutex);
        value.state = TRACY_EMBEDDED_CAPTURE_FINISHED;
        value.error.clear();
        completedWorker = std::move(value.worker);
    }
    // Do not hold the coordinator mutex while Tracy joins any remaining
    // background work in the Worker destructor.
    completedWorker.reset();
    return TRACY_EMBEDDED_CAPTURE_OK;
}

}  // namespace

extern "C" {

int32_t ___tracy_embedded_capture_configure(const char* path, size_t pathLength,
                                             size_t channelCapacity,
                                             int64_t workerMemoryLimit) {
    auto& value = coordinator();
    try {
        if (!path || pathLength == 0 || pathLength > 32767 ||
            !validUtf8(path, pathLength) || channelCapacity == 0 ||
            workerMemoryLimit == 0) {
            std::lock_guard lock(value.mutex);
            value.error = "invalid embedded capture configuration";
            return TRACY_EMBEDDED_CAPTURE_INVALID_ARGUMENT;
        }
        std::lock_guard lock(value.mutex);
        if (value.state != TRACY_EMBEDDED_CAPTURE_UNCONFIGURED) {
            value.error = "embedded capture is already configured";
            return TRACY_EMBEDDED_CAPTURE_INVALID_STATE;
        }
#ifdef _WIN32
        const auto* first = reinterpret_cast<const char8_t*>(path);
        value.output = std::filesystem::path(std::u8string(first, first + pathLength));
#else
        value.output = std::filesystem::path(std::string(path, pathLength));
#endif
        if (std::filesystem::exists(value.output)) {
            value.error = "capture output already exists";
            return TRACY_EMBEDDED_CAPTURE_OUTPUT_EXISTS;
        }
        if (!tracy::embedded::Configure(channelCapacity)) {
            value.error = tracy::embedded::GetError();
            value.state = TRACY_EMBEDDED_CAPTURE_FAILED;
            return TRACY_EMBEDDED_CAPTURE_TRANSPORT_ERROR;
        }
        value.worker = std::make_unique<tracy::Worker>("embedded", 0,
                                                       workerMemoryLimit);
        value.error.clear();
        value.state = TRACY_EMBEDDED_CAPTURE_CONFIGURED;
        return TRACY_EMBEDDED_CAPTURE_OK;
    } catch (const std::exception& exception) {
        setFailure(value, exception.what());
        return TRACY_EMBEDDED_CAPTURE_INTERNAL_ERROR;
    } catch (...) {
        setFailure(value, "unknown embedded capture configuration failure");
        return TRACY_EMBEDDED_CAPTURE_INTERNAL_ERROR;
    }
}

int32_t ___tracy_embedded_capture_finish_with_disposition(int32_t disposition) {
    try {
        return finishImpl(disposition);
    } catch (const std::exception& exception) {
        setFailure(coordinator(), exception.what());
        tracy::embedded::Cancel();
        return TRACY_EMBEDDED_CAPTURE_INTERNAL_ERROR;
    } catch (...) {
        setFailure(coordinator(), "unknown embedded capture finalization failure");
        tracy::embedded::Cancel();
        return TRACY_EMBEDDED_CAPTURE_INTERNAL_ERROR;
    }
}

int32_t ___tracy_embedded_capture_finish(void) {
    return ___tracy_embedded_capture_finish_with_disposition(TRACY_EMBEDDED_CAPTURE_SAVE);
}

uint32_t ___tracy_embedded_capture_abi_version(void) {
    return TRACY_EMBEDDED_CAPTURE_ABI_VERSION;
}

int32_t ___tracy_embedded_capture_get_state(void) {
    auto& value = coordinator();
    std::lock_guard lock(value.mutex);
    if (value.state == TRACY_EMBEDDED_CAPTURE_CONFIGURED && value.worker &&
        value.worker->HasData()) {
        value.state = TRACY_EMBEDDED_CAPTURE_CAPTURING;
    }
    return value.state;
}

int32_t ___tracy_embedded_capture_get_statistics(
    tracy_embedded_capture_statistics* statistics) {
    if (!statistics) return TRACY_EMBEDDED_CAPTURE_INVALID_ARGUMENT;
    const auto source = tracy::embedded::GetStatistics();
    statistics->client_to_server_bytes = source.clientToServerBytes;
    statistics->server_to_client_bytes = source.serverToClientBytes;
    statistics->client_to_server_high_water = source.clientToServerHighWater;
    statistics->server_to_client_high_water = source.serverToClientHighWater;
    auto& value = coordinator();
    std::lock_guard lock(value.mutex);
    statistics->writer_open_count = value.writerOpenCount;
    statistics->worker_write_count = value.workerWriteCount;
    statistics->publish_count = value.publishCount;
    return TRACY_EMBEDDED_CAPTURE_OK;
}

#ifdef TRACY_EMBEDDED_CAPTURE_TESTING
void ___tracy_embedded_capture_test_force_finish_timeout(void) {
    forceFinishTimeout.store(true, std::memory_order_relaxed);
}
#endif

size_t ___tracy_embedded_capture_get_error(char* destination, size_t capacity) {
    auto& value = coordinator();
    std::lock_guard lock(value.mutex);
    const auto length = value.error.size();
    if (destination && capacity != 0) {
        const auto count = std::min(length, capacity - 1);
        std::memcpy(destination, value.error.data(), count);
        destination[count] = '\0';
    }
    return length;
}

}  // extern "C"
