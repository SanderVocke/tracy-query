#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

#include "TracyEmbeddedTransport.hpp"

namespace {

int fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

}  // namespace

int main() {
    using namespace tracy::embedded;
    if (!Configure(8)) return fail("configure failed");
    if (!Listen()) return fail("listen failed");

    void* server = nullptr;
    void* client = nullptr;
    if (!Connect(server) || !Accept(client)) return fail("rendezvous failed");
    if (!IsValid(server) || !IsValid(client) || Capacity(server) != 8) {
        return fail("invalid endpoints");
    }

    std::array<char, 32> source{};
    for (std::size_t i = 0; i < source.size(); ++i) source[i] = static_cast<char>(i);
    std::atomic<bool> writerDone = false;
    std::thread writer([&] {
        if (Send(client, source.data(), static_cast<int>(source.size())) !=
            static_cast<int>(source.size())) {
            std::abort();
        }
        writerDone = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (writerDone) return fail("bounded stream did not apply backpressure");

    std::array<char, 32> destination{};
    int offset = 0;
    while (offset != static_cast<int>(destination.size())) {
        const int count = Read(server, destination.data() + offset, 3, 1000);
        if (count <= 0) return fail("ordered read failed");
        offset += count;
    }
    writer.join();
    if (destination != source) return fail("stream ordering or wraparound failed");

    char byte = 0;
    if (Read(server, &byte, 1, 5) != -1) return fail("read timeout was not reported");

    constexpr char reply[] = "reply";
    if (Send(server, reply, 5) != 5 || !HasData(client)) {
        return fail("reverse stream failed");
    }
    std::array<char, 5> replyBuffer{};
    if (Read(client, replyBuffer.data(), 5, 1000) != 5 ||
        std::memcmp(replyBuffer.data(), reply, 5) != 0) {
        return fail("reverse stream contents failed");
    }

    const auto statistics = GetStatistics();
    if (statistics.clientToServerBytes != source.size() ||
        statistics.serverToClientBytes != 5 ||
        statistics.clientToServerHighWater > 8 ||
        statistics.clientToServerHighWater == 0) {
        return fail("transport statistics failed");
    }

    std::array<char, 8> fill{};
    if (Send(server, fill.data(), static_cast<int>(fill.size())) !=
        static_cast<int>(fill.size())) {
        return fail("cannot fill reverse stream");
    }
    std::atomic<int> closeRead = -2;
    std::atomic<int> closeWrite = -2;
    std::thread blockedReader([&] { closeRead = Read(server, &byte, 1, -1); });
    std::thread blockedWriter([&] { closeWrite = Send(server, &byte, 1); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CloseEndpoint(client);
    blockedReader.join();
    blockedWriter.join();
    if (closeRead != 0) return fail("peer close did not wake blocked reader");
    if (closeWrite != -1) return fail("peer close did not wake blocked writer");
    CloseEndpoint(server);
    Cancel();
    return 0;
}
