#include "tracy_collector/collector.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "TracyAlloc.hpp"
#include "TracySocket.hpp"

namespace {

constexpr std::uint32_t max_payload = 65536;
constexpr std::uint16_t response_bit = 0x8000;
constexpr std::array<unsigned char, 4> magic{'T', 'C', 'O', 'L'};
std::atomic<bool> interrupted{false};

struct Options {
    std::filesystem::path output_root;
    std::filesystem::path ready_file;
    std::uint16_t control_port = 9327;
    tracy_collector::Limits limits;
};

struct SocketDeleter {
    void operator()(tracy::Socket* socket) const {
        if (socket == nullptr) return;
        socket->~Socket();
        tracy_free(socket);
    }
};
using SocketPtr = std::unique_ptr<tracy::Socket, SocketDeleter>;

struct ProtocolError : std::runtime_error {
    std::uint16_t status;
    ProtocolError(std::uint16_t code, const std::string& message)
        : std::runtime_error(message), status(code) {}
};

void signal_handler(int) { interrupted.store(true, std::memory_order_relaxed); }

std::uint64_t parse_unsigned(const std::string& value, const char* option) {
    if (value.empty() || value.front() == '-') {
        throw std::invalid_argument(std::string("invalid value for ") + option);
    }
    std::size_t consumed = 0;
    const auto parsed = std::stoull(value, &consumed);
    if (consumed != value.size()) {
        throw std::invalid_argument(std::string("invalid value for ") + option);
    }
    return parsed;
}

std::uint16_t parse_port(const std::string& value, const char* option) {
    const auto parsed = parse_unsigned(value, option);
    if (parsed == 0 || parsed > 65535) {
        throw std::invalid_argument(std::string("port out of range for ") + option);
    }
    return static_cast<std::uint16_t>(parsed);
}

void print_usage(std::ostream& out) {
    out << "Usage: tracy-collector --output-root DIR --ready-file FILE [options]\n"
        << "Options:\n"
        << "  --control-port PORT\n"
        << "  --data-port-first PORT\n"
        << "  --data-port-last PORT\n"
        << "  --max-sessions N\n"
        << "  --memory-limit BYTES\n"
        << "  --connect-timeout-ms MS\n"
        << "  --owner-timeout-ms MS\n"
        << "  --finalize-timeout-ms MS\n"
        << "  --version\n"
        << "  --help\n";
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        auto value = [&]() -> std::string {
            if (++i >= argc) throw std::invalid_argument("missing value for " + argument);
            return argv[i];
        };
        if (argument == "--output-root") options.output_root = value();
        else if (argument == "--ready-file") options.ready_file = value();
        else if (argument == "--control-port") options.control_port = parse_port(value(), argument.c_str());
        else if (argument == "--data-port-first") options.limits.data_port_first = parse_port(value(), argument.c_str());
        else if (argument == "--data-port-last") options.limits.data_port_last = parse_port(value(), argument.c_str());
        else if (argument == "--max-sessions") {
            const auto number = parse_unsigned(value(), argument.c_str());
            if (number == 0 || number > 10000) throw std::invalid_argument("max sessions out of range");
            options.limits.max_sessions = static_cast<std::size_t>(number);
        } else if (argument == "--memory-limit") {
            const auto number = parse_unsigned(value(), argument.c_str());
            if (number == 0 || number > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
                throw std::invalid_argument("memory limit out of range");
            }
            options.limits.memory_limit = static_cast<std::int64_t>(number);
        } else if (argument == "--connect-timeout-ms") {
            options.limits.connect_timeout = std::chrono::milliseconds(parse_unsigned(value(), argument.c_str()));
        } else if (argument == "--owner-timeout-ms") {
            options.limits.owner_timeout = std::chrono::milliseconds(parse_unsigned(value(), argument.c_str()));
        } else if (argument == "--finalize-timeout-ms") {
            options.limits.finalize_timeout = std::chrono::milliseconds(parse_unsigned(value(), argument.c_str()));
        } else if (argument == "--help") {
            print_usage(std::cout);
            std::exit(0);
        } else if (argument == "--version") {
            std::cout << "tracy-collector 0.2.0 (control protocol 1, Tracy 0.13.1)\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    if (options.output_root.empty() || options.ready_file.empty()) {
        throw std::invalid_argument("--output-root and --ready-file are required");
    }
    return options;
}

std::string random_hex(std::size_t bytes) {
    std::random_device device;
    std::uniform_int_distribution<unsigned> distribution(0, 255);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < bytes; ++i) out << std::setw(2) << distribution(device);
    return out.str();
}

bool valid_utf8(const std::string& text) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t i = 0;
    while (i < text.size()) {
        unsigned char c = bytes[i++];
        if (c <= 0x7f) continue;
        unsigned remaining = 0;
        std::uint32_t value = 0;
        if ((c & 0xe0) == 0xc0) { remaining = 1; value = c & 0x1f; }
        else if ((c & 0xf0) == 0xe0) { remaining = 2; value = c & 0x0f; }
        else if ((c & 0xf8) == 0xf0) { remaining = 3; value = c & 0x07; }
        else return false;
        if (i + remaining > text.size()) return false;
        for (unsigned j = 0; j < remaining; ++j) {
            const unsigned char next = bytes[i++];
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if ((remaining == 1 && value < 0x80) ||
            (remaining == 2 && value < 0x800) ||
            (remaining == 3 && value < 0x10000) ||
            value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

std::uint16_t read_u16(const std::vector<unsigned char>& data, std::size_t& offset) {
    if (offset + 2 > data.size()) throw ProtocolError(1, "truncated u16");
    const auto value = static_cast<std::uint16_t>((data[offset] << 8) | data[offset + 1]);
    offset += 2;
    return value;
}

std::uint32_t read_u32(const std::vector<unsigned char>& data, std::size_t& offset) {
    if (offset + 4 > data.size()) throw ProtocolError(1, "truncated u32");
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value = (value << 8) | data[offset++];
    return value;
}

std::string read_string(const std::vector<unsigned char>& data, std::size_t& offset) {
    const auto length = read_u16(data, offset);
    if (length > 4096 || offset + length > data.size()) {
        throw ProtocolError(1, "invalid string length");
    }
    std::string result(reinterpret_cast<const char*>(data.data() + offset), length);
    offset += length;
    if (!valid_utf8(result)) throw ProtocolError(1, "invalid UTF-8");
    return result;
}

void put_u16(std::vector<unsigned char>& data, std::uint16_t value) {
    data.push_back(static_cast<unsigned char>(value >> 8));
    data.push_back(static_cast<unsigned char>(value));
}

void put_u32(std::vector<unsigned char>& data, std::uint32_t value) {
    data.push_back(static_cast<unsigned char>(value >> 24));
    data.push_back(static_cast<unsigned char>(value >> 16));
    data.push_back(static_cast<unsigned char>(value >> 8));
    data.push_back(static_cast<unsigned char>(value));
}

void put_string(std::vector<unsigned char>& data, const std::string& value) {
    if (value.size() > 4096) throw ProtocolError(5, "response string exceeds limit");
    put_u16(data, static_cast<std::uint16_t>(value.size()));
    data.insert(data.end(), value.begin(), value.end());
}

bool secure_equal(const std::string& left, const std::string& right) {
    std::size_t difference = left.size() ^ right.size();
    const std::size_t count = std::max(left.size(), right.size());
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned char a = i < left.size() ? static_cast<unsigned char>(left[i]) : 0;
        const unsigned char b = i < right.size() ? static_cast<unsigned char>(right[i]) : 0;
        difference |= a ^ b;
    }
    return difference == 0;
}

std::uint16_t exception_status(const std::exception& error) {
    if (dynamic_cast<const std::out_of_range*>(&error)) return 4;
    if (dynamic_cast<const std::length_error*>(&error)) return 5;
    if (dynamic_cast<const std::domain_error*>(&error)) return 3;
    if (dynamic_cast<const std::logic_error*>(&error)) return 6;
    const std::string message = error.what();
    if (message.find("owner") != std::string::npos || message.find("lease") != std::string::npos) return 9;
    return 7;
}

std::vector<unsigned char> dispatch(std::uint16_t type,
                                    const std::vector<unsigned char>& request,
                                    const std::string& token,
                                    tracy_collector::Collector& collector) {
    std::size_t offset = 0;
    const auto supplied_token = read_string(request, offset);
    if (!secure_equal(supplied_token, token)) throw ProtocolError(2, "authentication failed");
    std::vector<unsigned char> result;
    const auto require_end = [&]() {
        if (offset != request.size()) throw ProtocolError(1, "trailing payload data");
    };
    switch (type) {
    case 1: {
        const auto run_id = read_string(request, offset);
        require_end();
        put_string(result, collector.acquire_owner(run_id));
        break;
    }
    case 2: {
        const auto lease = read_string(request, offset);
        require_end();
        collector.heartbeat(lease);
        break;
    }
    case 3: {
        tracy_collector::Attempt attempt;
        attempt.run_id = read_string(request, offset);
        attempt.attempt_id = read_string(request, offset);
        attempt.binary_id = read_string(request, offset);
        attempt.test_name = read_string(request, offset);
        attempt.retry = read_u32(request, offset);
        attempt.stress_metadata = read_string(request, offset);
        require_end();
        const auto registration = collector.register_attempt(attempt);
        put_string(result, registration.session_id);
        put_u16(result, registration.port);
        break;
    }
    case 4: {
        const auto session = read_string(request, offset);
        require_end();
        const auto info = collector.status(session);
        put_string(result, info.state);
        put_string(result, info.handshake);
        put_string(result, info.error);
        put_string(result, info.output_name);
        break;
    }
    case 5: {
        const auto session = read_string(request, offset);
        const auto raw_decision = read_u16(request, offset);
        if (raw_decision != 1 && raw_decision != 2) throw ProtocolError(1, "invalid decision");
        const auto source = read_string(request, offset);
        require_end();
        put_string(result, collector.decide(
            session, static_cast<tracy_collector::Decision>(raw_decision), source));
        break;
    }
    case 6: {
        const auto lease = read_string(request, offset);
        require_end();
        put_u32(result, collector.finalize(lease, "owner-finalize", true));
        break;
    }
    case 7: {
        require_end();
        const auto sessions = collector.list();
        put_u32(result, static_cast<std::uint32_t>(sessions.size()));
        for (const auto& info : sessions) {
            put_string(result, info.session_id);
            put_string(result, info.attempt.attempt_id);
            put_string(result, info.state);
            put_string(result, info.output_name);
            put_string(result, info.error);
        }
        break;
    }
    default: throw ProtocolError(8, "unknown message type");
    }
    if (result.size() > max_payload) throw ProtocolError(5, "response exceeds frame limit");
    return result;
}

void send_response(tracy::Socket& socket, std::uint16_t type, std::uint16_t status,
                   const std::string& message,
                   const std::vector<unsigned char>& operation = {}) {
    std::vector<unsigned char> payload;
    put_u16(payload, status);
    put_string(payload, message.substr(0, 4096));
    if (status == 0) payload.insert(payload.end(), operation.begin(), operation.end());
    std::array<unsigned char, 12> header{};
    std::copy(magic.begin(), magic.end(), header.begin());
    header[4] = 0;
    header[5] = static_cast<unsigned char>(tracy_collector::protocol_version);
    const auto response_type = static_cast<std::uint16_t>(type | response_bit);
    header[6] = static_cast<unsigned char>(response_type >> 8);
    header[7] = static_cast<unsigned char>(response_type);
    const auto size = static_cast<std::uint32_t>(payload.size());
    header[8] = static_cast<unsigned char>(size >> 24);
    header[9] = static_cast<unsigned char>(size >> 16);
    header[10] = static_cast<unsigned char>(size >> 8);
    header[11] = static_cast<unsigned char>(size);
    socket.Send(header.data(), static_cast<int>(header.size()));
    if (!payload.empty()) socket.Send(payload.data(), static_cast<int>(payload.size()));
}

void handle_connection(SocketPtr socket,
                       const std::string& token,
                       tracy_collector::Collector& collector) {
    std::uint16_t type = 0;
    try {
        std::array<unsigned char, 12> header{};
        if (!socket->Read(header.data(), static_cast<int>(header.size()), 1000)) return;
        if (!std::equal(magic.begin(), magic.end(), header.begin())) {
            throw ProtocolError(1, "invalid frame magic");
        }
        const auto version = static_cast<std::uint16_t>((header[4] << 8) | header[5]);
        type = static_cast<std::uint16_t>((header[6] << 8) | header[7]);
        std::uint32_t size = 0;
        for (unsigned i = 8; i < 12; ++i) size = (size << 8) | header[i];
        if (size > max_payload) throw ProtocolError(1, "payload exceeds frame limit");
        std::vector<unsigned char> request(size);
        if (size != 0 && !socket->Read(request.data(), static_cast<int>(size), 1000)) {
            throw ProtocolError(1, "truncated payload");
        }
        if (socket->HasData()) throw ProtocolError(1, "multiple frames per connection are not allowed");
        if (version != tracy_collector::protocol_version) throw ProtocolError(8, "unsupported protocol version");
        const auto response = dispatch(type, request, token, collector);
        send_response(*socket, type, 0, "ok", response);
    } catch (const ProtocolError& error) {
        send_response(*socket, type, error.status, error.what());
    } catch (const std::exception& error) {
        send_response(*socket, type, exception_status(error), error.what());
    }
}

std::string json_escape(const std::string& value) {
    std::string result;
    for (const char c : value) {
        if (c == '"' || c == '\\') result.push_back('\\');
        result.push_back(c);
    }
    return result;
}

void atomic_write(const std::filesystem::path& target, const std::string& content) {
    if (!target.parent_path().empty()) std::filesystem::create_directories(target.parent_path());
    const auto temp = target.string() + ".tmp-" + random_hex(4);
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot create ready descriptor temporary file");
        out << content;
        out.flush();
        if (!out) throw std::runtime_error("cannot write ready descriptor");
    }
    std::error_code ignored;
    std::filesystem::remove(target, ignored);
    std::filesystem::rename(temp, target);
}

int run(const Options& options) {
    std::string token;
    if (const char* supplied = std::getenv("TRACY_COLLECTOR_TOKEN")) token = supplied;
    if (token.empty()) token = random_hex(32);
    if (token.size() > 128 || !valid_utf8(token)) throw std::runtime_error("invalid TRACY_COLLECTOR_TOKEN");
    const std::string run_id = random_hex(16);

    std::filesystem::create_directories(options.output_root);
    const auto output_root = std::filesystem::canonical(options.output_root);
    auto secret_file = options.ready_file;
    secret_file += ".secret";
    atomic_write(secret_file, token + "\n");
#ifndef _WIN32
    if (chmod(secret_file.c_str(), S_IRUSR | S_IWUSR) != 0) {
        throw std::runtime_error("cannot restrict secret file permissions");
    }
#endif

    tracy_collector::Collector collector(output_root, run_id, options.limits);
    tracy::ListenSocket listener;
    if (!listener.Listen(options.control_port, 32)) {
        throw std::runtime_error("cannot bind loopback control port " + std::to_string(options.control_port));
    }

    std::ostringstream ready;
    ready << "{\"protocol_version\":1,\"endpoint\":\"127.0.0.1:"
          << options.control_port << "\",\"run_id\":\"" << run_id
          << "\",\"secret_file\":\"" << json_escape(std::filesystem::absolute(secret_file).string())
          << "\"}\n";
    atomic_write(options.ready_file, ready.str());
    std::cerr << "tracy-collector ready on 127.0.0.1:" << options.control_port << '\n';

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::vector<std::thread> handlers;
    bool expiry_started = false;
    bool observed_finalizing = false;
    auto finalize_started = std::chrono::steady_clock::now();
    bool forced_deadline = false;

    for (;;) {
        collector.poll();
        if (interrupted.load(std::memory_order_relaxed) && !collector.finalizing()) {
            std::cerr << "signal received; saving unresolved sessions\n";
            collector.force_finalize("signal", true);
            finalize_started = std::chrono::steady_clock::now();
        }
        if (collector.owner_expired() && !collector.finalizing()) {
            std::cerr << "owner lease expired; saving unresolved sessions\n";
            collector.force_finalize("owner-loss", false);
            expiry_started = true;
            finalize_started = std::chrono::steady_clock::now();
        }
        if (collector.finalizing()) {
            if (!observed_finalizing) {
                observed_finalizing = true;
                finalize_started = std::chrono::steady_clock::now();
            }
            if (collector.all_terminal()) break;
            if (std::chrono::steady_clock::now() - finalize_started > collector.finalize_timeout()) {
                std::cerr << "finalization deadline reached; disconnecting active sessions\n";
                collector.force_finalize(expiry_started ? "owner-loss-deadline" : "finalize-deadline", true);
                forced_deadline = true;
                finalize_started = std::chrono::steady_clock::now();
            }
        }
        if (auto* accepted = listener.Accept()) {
            handlers.emplace_back(handle_connection, SocketPtr(accepted),
                                  std::cref(token), std::ref(collector));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    listener.Close();
    for (auto& handler : handlers) handler.join();
    std::error_code ignored;
    std::filesystem::remove(secret_file, ignored);
    return collector.has_failures() || forced_deadline ? 4 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(parse_options(argc, argv));
    } catch (const std::invalid_argument& error) {
        std::cerr << "tracy-collector: " << error.what() << '\n';
        print_usage(std::cerr);
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "tracy-collector: " << error.what() << '\n';
        return 3;
    }
}
