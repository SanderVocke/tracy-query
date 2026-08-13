#include <algorithm>
#include <cstring>
#include <new>

#include "TracyAlloc.hpp"
#include "TracyEmbeddedTransport.hpp"
#include "TracySocket.hpp"

namespace tracy {

#ifdef _WIN32
void InitWinSock() {}
#endif

Socket::Socket() : m_embedded(nullptr) {}

Socket::Socket(int) : m_embedded(nullptr) {
    embedded::Accept(m_embedded);
}

Socket::~Socket() {
    embedded::DestroyEndpoint(m_embedded);
}

bool Socket::Connect(const char*, uint16_t) {
    return embedded::Connect(m_embedded);
}

bool Socket::ConnectBlocking(const char* address, uint16_t port) {
    while (!Connect(address, port)) {
        if (embedded::GetError()[0] != '\0') return false;
    }
    return true;
}

void Socket::Close() {
    embedded::CloseEndpoint(m_embedded);
}

int Socket::Send(const void* buffer, int length) {
    return embedded::Send(m_embedded, buffer, length);
}

int Socket::GetSendBufSize() {
    return embedded::Capacity(m_embedded);
}

int Socket::RecvBuffered(void* buffer, int length, int timeout) {
    return embedded::Read(m_embedded, buffer, length, timeout);
}

int Socket::Recv(void* buffer, int length, int timeout) {
    return embedded::Read(m_embedded, buffer, length, timeout);
}

int Socket::ReadUpTo(void* rawBuffer, int length) {
    auto* buffer = static_cast<char*>(rawBuffer);
    int read = 0;
    while (length > 0) {
        const int result = embedded::Read(m_embedded, buffer, length, -1);
        if (result == 0) break;
        if (result < 0) return -1;
        length -= result;
        read += result;
        buffer += result;
    }
    return read;
}

bool Socket::Read(void* buffer, int length, int timeout) {
    auto* destination = static_cast<char*>(buffer);
    while (length > 0) {
        if (!ReadImpl(destination, length, timeout)) return false;
    }
    return true;
}

bool Socket::ReadImpl(char*& buffer, int& length, int timeout) {
    const int result = embedded::Read(m_embedded, buffer, length, timeout);
    if (result == 0) return false;
    if (result > 0) {
        length -= result;
        buffer += result;
    }
    return true;
}

bool Socket::ReadRaw(void* rawBuffer, int length, int timeout) {
    auto* buffer = static_cast<char*>(rawBuffer);
    while (length > 0) {
        const int result = embedded::Read(m_embedded, buffer, length, timeout);
        if (result <= 0) return false;
        length -= result;
        buffer += result;
    }
    return true;
}

bool Socket::HasData() {
    return embedded::HasData(m_embedded);
}

bool Socket::IsValid() const {
    return embedded::IsValid(m_embedded);
}

ListenSocket::ListenSocket() : m_sock(-1) {}
ListenSocket::~ListenSocket() { if (m_sock != -1) Close(); }

bool ListenSocket::Listen(uint16_t, int) {
    if (m_sock != -1 || !embedded::Listen()) return false;
    m_sock = 1;
    return true;
}

Socket* ListenSocket::Accept() {
    if (m_sock == -1) return nullptr;
    auto* storage = static_cast<Socket*>(tracy_malloc(sizeof(Socket)));
    new (storage) Socket(1);
    if (!storage->IsValid()) {
        storage->~Socket();
        tracy_free(storage);
        return nullptr;
    }
    return storage;
}

void ListenSocket::Close() {
    m_sock = -1;
}

UdpBroadcast::UdpBroadcast() : m_sock(-1), m_addr(0) {}
UdpBroadcast::~UdpBroadcast() = default;
bool UdpBroadcast::Open(const char*, uint16_t) { return false; }
void UdpBroadcast::Close() { m_sock = -1; }
int UdpBroadcast::Send(uint16_t, const void*, int) { return -1; }

IpAddress::IpAddress() : m_number(0), m_text{} {}
IpAddress::~IpAddress() = default;
void IpAddress::Set(const struct sockaddr&) {}

UdpListen::UdpListen() : m_sock(-1) {}
UdpListen::~UdpListen() = default;
bool UdpListen::Listen(uint16_t) { return false; }
void UdpListen::Close() { m_sock = -1; }
const char* UdpListen::Read(size_t&, IpAddress&, int) { return nullptr; }

}  // namespace tracy
