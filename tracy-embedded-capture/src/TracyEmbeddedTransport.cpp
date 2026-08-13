#include "TracyEmbeddedTransport.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tracy::embedded {
namespace {

struct Pipe {
    explicit Pipe(std::size_t requestedCapacity) : storage(requestedCapacity) {}

    std::vector<char> storage;
    std::size_t head = 0;
    std::size_t size = 0;
    bool writerClosed = false;
    bool readerClosed = false;
    std::uint64_t transferred = 0;
    std::uint64_t highWater = 0;
    std::mutex mutex;
    std::condition_variable readable;
    std::condition_variable writable;
};

struct Session {
    explicit Session(std::size_t capacity)
        : clientToServer(capacity), serverToClient(capacity) {}

    Pipe clientToServer;
    Pipe serverToClient;
    std::mutex rendezvousMutex;
    bool listening = false;
    bool connected = false;
    bool accepted = false;
    bool cancelled = false;
    std::string error;
};

struct Endpoint {
    std::shared_ptr<Session> session;
    bool client = false;
    bool closed = false;
};

std::mutex gMutex;
std::shared_ptr<Session> gSession;
std::string gError;

Pipe& outgoing(Endpoint& endpoint) {
    return endpoint.client ? endpoint.session->clientToServer
                           : endpoint.session->serverToClient;
}

Pipe& incoming(Endpoint& endpoint) {
    return endpoint.client ? endpoint.session->serverToClient
                           : endpoint.session->clientToServer;
}

void setError(const std::string& message) {
    std::lock_guard lock(gMutex);
    gError = message;
    if (gSession) gSession->error = message;
}

void closePipeWriter(Pipe& pipe) {
    std::lock_guard lock(pipe.mutex);
    pipe.writerClosed = true;
    pipe.readable.notify_all();
    pipe.writable.notify_all();
}

void closePipeReader(Pipe& pipe) {
    std::lock_guard lock(pipe.mutex);
    pipe.readerClosed = true;
    pipe.readable.notify_all();
    pipe.writable.notify_all();
}

void cancelPipe(Pipe& pipe) {
    std::lock_guard lock(pipe.mutex);
    pipe.readerClosed = true;
    pipe.writerClosed = true;
    pipe.readable.notify_all();
    pipe.writable.notify_all();
}

int writePipe(Pipe& pipe, const char* source, int length) {
    int written = 0;
    while (written < length) {
        std::unique_lock lock(pipe.mutex);
        pipe.writable.wait(lock, [&] {
            return pipe.readerClosed || pipe.size < pipe.storage.size();
        });
        if (pipe.readerClosed || pipe.writerClosed) return -1;

        const std::size_t tail = (pipe.head + pipe.size) % pipe.storage.size();
        const std::size_t contiguous = std::min(pipe.storage.size() - tail,
                                                pipe.storage.size() - pipe.size);
        const std::size_t count = std::min<std::size_t>(
            contiguous, static_cast<std::size_t>(length - written));
        std::memcpy(pipe.storage.data() + tail, source + written, count);
        pipe.size += count;
        pipe.transferred += count;
        pipe.highWater = std::max<std::uint64_t>(pipe.highWater, pipe.size);
        written += static_cast<int>(count);
        pipe.readable.notify_one();
    }
    return written;
}

int readPipe(Pipe& pipe, char* destination, int length, int timeoutMilliseconds) {
    std::unique_lock lock(pipe.mutex);
    const auto ready = [&] { return pipe.size != 0 || pipe.writerClosed || pipe.readerClosed; };
    if (timeoutMilliseconds < 0) {
        pipe.readable.wait(lock, ready);
    } else if (!pipe.readable.wait_for(lock, std::chrono::milliseconds(timeoutMilliseconds), ready)) {
        return -1;
    }
    if (pipe.size == 0) return 0;

    const std::size_t count = std::min<std::size_t>(
        {static_cast<std::size_t>(length), pipe.size, pipe.storage.size() - pipe.head});
    std::memcpy(destination, pipe.storage.data() + pipe.head, count);
    pipe.head = (pipe.head + count) % pipe.storage.size();
    pipe.size -= count;
    pipe.writable.notify_one();
    return static_cast<int>(count);
}

}  // namespace

bool Configure(std::size_t capacity) {
    std::lock_guard lock(gMutex);
    if (capacity == 0 || capacity > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        gError = "embedded transport capacity is invalid";
        return false;
    }
    if (gSession) {
        gError = "embedded transport is already configured";
        return false;
    }
    try {
        gSession = std::make_shared<Session>(capacity);
        gError.clear();
        return true;
    } catch (...) {
        gError = "cannot allocate embedded transport";
        return false;
    }
}

bool Listen() {
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(gMutex);
        session = gSession;
    }
    if (!session) {
        setError("embedded transport is not configured");
        return false;
    }
    std::lock_guard lock(session->rendezvousMutex);
    if (session->cancelled || session->listening) return false;
    session->listening = true;
    return true;
}

bool Connect(void*& endpoint) {
    if (endpoint) return true;
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(gMutex);
        session = gSession;
    }
    if (!session) return false;
    std::lock_guard lock(session->rendezvousMutex);
    if (!session->listening || session->cancelled || session->connected) return false;
    session->connected = true;
    endpoint = new Endpoint{session, false, false};
    return true;
}

bool Accept(void*& endpoint) {
    if (endpoint) return false;
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(gMutex);
        session = gSession;
    }
    if (!session) return false;
    std::lock_guard lock(session->rendezvousMutex);
    if (!session->connected || session->cancelled || session->accepted) return false;
    session->accepted = true;
    endpoint = new Endpoint{session, true, false};
    return true;
}

void CloseEndpoint(void* opaque) {
    auto* endpoint = static_cast<Endpoint*>(opaque);
    if (!endpoint || endpoint->closed) return;
    endpoint->closed = true;
    closePipeWriter(outgoing(*endpoint));
    closePipeReader(incoming(*endpoint));
}

void DestroyEndpoint(void*& opaque) {
    auto* endpoint = static_cast<Endpoint*>(opaque);
    if (!endpoint) return;
    CloseEndpoint(endpoint);
    delete endpoint;
    opaque = nullptr;
}

int Send(void* opaque, const void* data, int length) {
    auto* endpoint = static_cast<Endpoint*>(opaque);
    if (!endpoint || endpoint->closed || !data || length < 0) return -1;
    if (length == 0) return 0;
    return writePipe(outgoing(*endpoint), static_cast<const char*>(data), length);
}

int Read(void* opaque, void* data, int length, int timeoutMilliseconds) {
    auto* endpoint = static_cast<Endpoint*>(opaque);
    if (!endpoint || endpoint->closed || !data || length <= 0) return 0;
    return readPipe(incoming(*endpoint), static_cast<char*>(data), length,
                    timeoutMilliseconds);
}

bool HasData(void* opaque) {
    auto* endpoint = static_cast<Endpoint*>(opaque);
    if (!endpoint || endpoint->closed) return false;
    auto& pipe = incoming(*endpoint);
    std::lock_guard lock(pipe.mutex);
    return pipe.size != 0;
}

bool IsValid(void* opaque) {
    auto* endpoint = static_cast<Endpoint*>(opaque);
    return endpoint && !endpoint->closed;
}

int Capacity(void* opaque) {
    auto* endpoint = static_cast<Endpoint*>(opaque);
    if (!endpoint) return 0;
    return static_cast<int>(outgoing(*endpoint).storage.size());
}

void Cancel() {
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(gMutex);
        session = gSession;
    }
    if (!session) return;
    {
        std::lock_guard lock(session->rendezvousMutex);
        session->cancelled = true;
    }
    cancelPipe(session->clientToServer);
    cancelPipe(session->serverToClient);
}

Statistics GetStatistics() {
    Statistics result;
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(gMutex);
        session = gSession;
    }
    if (!session) return result;
    {
        std::lock_guard lock(session->clientToServer.mutex);
        result.clientToServerBytes = session->clientToServer.transferred;
        result.clientToServerHighWater = session->clientToServer.highWater;
    }
    {
        std::lock_guard lock(session->serverToClient.mutex);
        result.serverToClientBytes = session->serverToClient.transferred;
        result.serverToClientHighWater = session->serverToClient.highWater;
    }
    return result;
}

const char* GetError() {
    std::lock_guard lock(gMutex);
    return gError.c_str();
}

}  // namespace tracy::embedded
