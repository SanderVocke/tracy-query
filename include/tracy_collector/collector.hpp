#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tracy_collector {

inline constexpr std::uint16_t protocol_version = 1;

struct Limits {
    std::uint16_t data_port_first = 9400;
    std::uint16_t data_port_last = 9499;
    std::size_t max_sessions = 256;
    std::int64_t memory_limit = 512LL * 1024 * 1024;
    std::chrono::milliseconds connect_timeout{30000};
    std::chrono::milliseconds owner_timeout{15000};
    std::chrono::milliseconds finalize_timeout{30000};
};

struct Attempt {
    std::string run_id;
    std::string attempt_id;
    std::string binary_id;
    std::string test_name;
    std::uint32_t retry = 0;
    std::string stress_metadata;
};

enum class Decision : std::uint16_t { save = 1, discard = 2 };

struct Registration {
    std::string session_id;
    std::uint16_t port = 0;
};

struct SessionInfo {
    std::string session_id;
    Attempt attempt;
    std::string state;
    std::string handshake;
    std::string decision;
    std::string decision_source;
    std::string output_name;
    std::string error;
    std::uint16_t port = 0;
};

class Collector {
public:
    Collector(std::filesystem::path output_root, std::string run_id, Limits limits);
    ~Collector();
    Collector(const Collector&) = delete;
    Collector& operator=(const Collector&) = delete;

    Registration register_attempt(const Attempt& attempt);
    SessionInfo status(const std::string& session_id);
    std::vector<SessionInfo> list();
    std::string decide(const std::string& session_id, Decision decision,
                       const std::string& source);

    std::string acquire_owner(const std::string& run_id);
    void heartbeat(const std::string& lease_id);
    std::uint32_t finalize(const std::string& lease_id, const std::string& source,
                           bool disconnect_connected);
    void force_finalize(const std::string& source, bool disconnect_connected);

    void poll();
    bool owner_expired() const;
    bool finalizing() const;
    bool all_terminal() const;
    bool has_failures() const;
    std::chrono::milliseconds finalize_timeout() const;

    static std::string sanitize_display(const std::string& text);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tracy_collector
