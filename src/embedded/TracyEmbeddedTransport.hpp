#pragma once

#include <cstddef>
#include <cstdint>

namespace tracy::embedded {

struct Statistics {
    std::uint64_t clientToServerBytes = 0;
    std::uint64_t serverToClientBytes = 0;
    std::uint64_t clientToServerHighWater = 0;
    std::uint64_t serverToClientHighWater = 0;
};

bool Configure(std::size_t capacity);
bool Listen();
bool Connect(void*& endpoint);
bool Accept(void*& endpoint);
void CloseEndpoint(void*& endpoint);
int Send(void* endpoint, const void* data, int length);
int Read(void* endpoint, void* data, int length, int timeoutMilliseconds);
bool HasData(void* endpoint);
bool IsValid(void* endpoint);
int Capacity(void* endpoint);
void Cancel();
Statistics GetStatistics();
const char* GetError();

}  // namespace tracy::embedded
