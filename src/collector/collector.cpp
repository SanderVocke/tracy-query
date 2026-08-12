#include "tracy_collector/collector.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "TracyFileWrite.hpp"
#include "TracyProtocol.hpp"
#include "TracySocket.hpp"
#include "TracyWorker.hpp"

namespace tracy_collector {
namespace {

using Clock = std::chrono::steady_clock;

enum class State {
    connecting,
    capturing,
    awaiting_decision,
    failed_to_connect,
    save_pending,
    discard_pending,
    writing_partial,
    saved,
    discarded,
    save_failed,
};

const char* state_name(State state) {
    switch (state) {
    case State::connecting: return "connecting";
    case State::capturing: return "capturing";
    case State::awaiting_decision: return "awaiting-decision";
    case State::failed_to_connect: return "failed-to-connect";
    case State::save_pending: return "save-pending";
    case State::discard_pending: return "discard-pending";
    case State::writing_partial: return "writing-partial";
    case State::saved: return "saved";
    case State::discarded: return "discarded";
    case State::save_failed: return "save-failed";
    }
    return "unknown";
}

bool terminal(State state) {
    return state == State::saved || state == State::discarded ||
           state == State::save_failed;
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const unsigned char c : value) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned>(c) << std::dec;
            } else {
                out << static_cast<char>(c);
            }
        }
    }
    return out.str();
}

std::string random_hex(std::size_t bytes) {
    std::random_device device;
    std::uniform_int_distribution<unsigned> distribution(0, 255);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < bytes; ++i) {
        out << std::setw(2) << distribution(device);
    }
    return out.str();
}

std::int64_t unix_millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool port_available(std::uint16_t port) {
    tracy::ListenSocket socket;
    if (!socket.Listen(port, 1)) return false;
    socket.Close();
    return true;
}

void atomic_replace(const std::filesystem::path& from,
                    const std::filesystem::path& to) {
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(), "MoveFileExW");
    }
#else
    std::filesystem::rename(from, to);
#endif
}

std::string handshake_name(const tracy::Worker* worker) {
    if (worker == nullptr) return "none";
    switch (worker->GetHandshakeStatus()) {
    case tracy::HandshakePending: return "pending";
    case tracy::HandshakeWelcome: return "welcome";
    case tracy::HandshakeProtocolMismatch: return "protocol-mismatch";
    case tracy::HandshakeNotAvailable: return "not-available";
    case tracy::HandshakeDropped: return "dropped";
    default: return "unknown";
    }
}

}  // namespace

struct Collector::Impl {
    struct Session {
        Attempt attempt;
        std::string id;
        std::uint16_t port = 0;
        State state = State::connecting;
        std::unique_ptr<tracy::Worker> worker;
        Clock::time_point registered_at = Clock::now();
        std::int64_t registered_unix_ms = unix_millis();
        std::int64_t updated_unix_ms = registered_unix_ms;
        bool had_data = false;
        bool has_decision = false;
        Decision decision = Decision::save;
        std::string decision_source;
        std::string output_name;
        std::string error;
        std::string final_handshake = "pending";
    };

    std::filesystem::path output_root;
    std::string run_id;
    Limits limits;
    mutable std::mutex mutex;
    std::unordered_map<std::string, std::unique_ptr<Session>> sessions;
    std::unordered_map<std::string, std::string> attempt_to_session;
    std::set<std::uint16_t> allocated_ports;
    std::string owner_lease;
    Clock::time_point owner_heartbeat = Clock::now();
    bool owner_active = false;
    bool is_finalizing = false;
    bool failures = false;

    Impl(std::filesystem::path root, std::string run, Limits configured)
        : output_root(std::move(root)), run_id(std::move(run)), limits(configured) {}

    Session& find(const std::string& id) {
        const auto it = sessions.find(id);
        if (it == sessions.end()) throw std::out_of_range("session not found");
        return *it->second;
    }

    void touch(Session& session) { session.updated_unix_ms = unix_millis(); }

    void release_worker(Session& session) {
        session.final_handshake = handshake_name(session.worker.get());
        session.worker.reset();
        allocated_ports.erase(session.port);
    }

    std::filesystem::path confined_path(const std::string& name) const {
        if (name.empty() || name == "." || name == ".." ||
            name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
            throw std::runtime_error("unsafe generated artifact name");
        }
        const auto path = (output_root / name).lexically_normal();
        if (path.parent_path() != output_root) {
            throw std::runtime_error("artifact path escapes output root");
        }
        return path;
    }

    void write_manifest_locked() {
        const auto temp = output_root / ("manifest.json.tmp-" + random_hex(4));
        const auto target = output_root / "manifest.json";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("cannot open manifest temporary file");
            out << "{\n  \"protocol_version\": " << protocol_version
                << ",\n  \"run_id\": \"" << json_escape(run_id)
                << "\",\n  \"sessions\": [\n";
            std::vector<const Session*> ordered;
            ordered.reserve(sessions.size());
            for (const auto& item : sessions) ordered.push_back(item.second.get());
            std::sort(ordered.begin(), ordered.end(), [](const Session* left,
                                                         const Session* right) {
                return left->id < right->id;
            });
            for (std::size_t i = 0; i < ordered.size(); ++i) {
                const auto& s = *ordered[i];
                const std::string decision = !s.has_decision
                    ? "" : (s.decision == Decision::save ? "save" : "discard");
                const std::string handshake = s.worker
                    ? handshake_name(s.worker.get()) : s.final_handshake;
                out << "    {\"session_id\":\"" << json_escape(s.id)
                    << "\",\"run_id\":\"" << json_escape(s.attempt.run_id)
                    << "\",\"attempt_id\":\"" << json_escape(s.attempt.attempt_id)
                    << "\",\"binary_id\":\"" << json_escape(s.attempt.binary_id)
                    << "\",\"test_name\":\"" << json_escape(s.attempt.test_name)
                    << "\",\"retry\":" << s.attempt.retry
                    << ",\"stress_metadata\":\"" << json_escape(s.attempt.stress_metadata)
                    << "\",\"state\":\"" << state_name(s.state)
                    << "\",\"registered_unix_ms\":" << s.registered_unix_ms
                    << ",\"updated_unix_ms\":" << s.updated_unix_ms
                    << ",\"decision\":\"" << decision
                    << "\",\"decision_source\":\"" << json_escape(s.decision_source)
                    << "\",\"output_name\":\"" << json_escape(s.output_name)
                    << "\",\"error\":\"" << json_escape(s.error)
                    << "\",\"handshake\":\"" << handshake
                    << "\",\"port\":" << s.port << "}";
                if (i + 1 != ordered.size()) out << ',';
                out << '\n';
            }
            out << "  ]\n}\n";
            out.flush();
            if (!out) throw std::runtime_error("cannot write manifest temporary file");
        }
        try {
            atomic_replace(temp, target);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            throw;
        }
    }

    void mark_save_failed(Session& session, const std::string& message) {
        session.error = message;
        session.state = State::save_failed;
        failures = true;
        touch(session);
        release_worker(session);
    }

    void save(Session& session) {
        if (!session.had_data || !session.worker) {
            mark_save_failed(session, "no usable Tracy handshake/data to save");
            return;
        }
        session.state = State::writing_partial;
        touch(session);
        const std::string display = Collector::sanitize_display(session.attempt.test_name);
        const std::string base = session.id + (display.empty() ? "" : "-" + display);
        const std::string partial_name = base + ".tracy.partial";
        const std::string final_name = base + ".tracy";
        std::filesystem::path partial;
        try {
            partial = confined_path(partial_name);
            const auto final = confined_path(final_name);
            if (std::filesystem::exists(final)) {
                throw std::runtime_error("refusing to overwrite existing trace");
            }
            std::unique_ptr<tracy::FileWrite> writer(
                tracy::FileWrite::Open(partial.string().c_str(),
                                       tracy::FileCompression::Zstd, 3, 1));
            if (!writer) throw std::runtime_error("FileWrite::Open failed");
            session.worker->Write(*writer, false);
            writer.reset();
            if (!std::filesystem::exists(partial) ||
                std::filesystem::file_size(partial) == 0) {
                throw std::runtime_error("trace writer produced an empty file");
            }
            std::filesystem::rename(partial, final);
            session.output_name = final_name;
            session.state = State::saved;
            touch(session);
            release_worker(session);
        } catch (const std::exception& error) {
            std::error_code cleanup_error;
            if (!partial.empty()) std::filesystem::remove(partial, cleanup_error);
            std::string message = error.what();
            if (cleanup_error) message += "; partial cleanup failed: " + cleanup_error.message();
            mark_save_failed(session, message);
        }
    }

    void apply_terminal_decision(Session& session) {
        if (!session.has_decision || terminal(session.state)) return;
        if (session.decision == Decision::discard) {
            session.state = State::discard_pending;
            if (session.worker) session.worker->Disconnect();
            session.state = State::discarded;
            touch(session);
            release_worker(session);
            return;
        }
        session.state = State::save_pending;
        if (session.worker && session.worker->IsConnected()) return;
        if (!is_finalizing && !session.had_data &&
            Clock::now() - session.registered_at < limits.connect_timeout) return;
        save(session);
    }

    SessionInfo info(const Session& session) const {
        SessionInfo result;
        result.session_id = session.id;
        result.attempt = session.attempt;
        result.state = state_name(session.state);
        result.handshake = session.worker ? handshake_name(session.worker.get())
                                          : session.final_handshake;
        if (session.has_decision) {
            result.decision = session.decision == Decision::save ? "save" : "discard";
        }
        result.decision_source = session.decision_source;
        result.output_name = session.output_name;
        result.error = session.error;
        result.port = session.port;
        return result;
    }
};

Collector::Collector(std::filesystem::path output_root, std::string run_id,
                     Limits limits) {
    if (run_id.empty() || run_id.size() > 4096) throw std::invalid_argument("invalid run ID");
    if (limits.data_port_first == 0 || limits.data_port_last < limits.data_port_first) {
        throw std::invalid_argument("invalid data port range");
    }
    if (limits.max_sessions == 0 || limits.memory_limit == 0 ||
        limits.connect_timeout.count() <= 0 || limits.owner_timeout.count() <= 0 ||
        limits.finalize_timeout.count() <= 0) {
        throw std::invalid_argument("collector limits must be positive");
    }
    std::filesystem::create_directories(output_root);
    output_root = std::filesystem::canonical(output_root);
    impl_ = std::make_unique<Impl>(std::move(output_root), std::move(run_id), limits);
    std::lock_guard lock(impl_->mutex);
    impl_->write_manifest_locked();
}

Collector::~Collector() = default;

Registration Collector::register_attempt(const Attempt& attempt) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->is_finalizing) throw std::logic_error("collector is finalizing");
    if (attempt.run_id != impl_->run_id) throw std::invalid_argument("run ID mismatch");
    if (attempt.attempt_id.empty() || attempt.attempt_id.size() > 4096 ||
        attempt.binary_id.size() > 4096 || attempt.test_name.size() > 4096 ||
        attempt.stress_metadata.size() > 4096) {
        throw std::invalid_argument("attempt metadata is empty or exceeds limits");
    }
    if (impl_->sessions.size() >= impl_->limits.max_sessions) {
        throw std::length_error("session limit exhausted");
    }
    if (impl_->attempt_to_session.contains(attempt.attempt_id)) {
        throw std::domain_error("duplicate attempt ID");
    }
    std::uint16_t port = 0;
    for (std::uint32_t candidate = impl_->limits.data_port_first;
         candidate <= impl_->limits.data_port_last; ++candidate) {
        const auto value = static_cast<std::uint16_t>(candidate);
        if (!impl_->allocated_ports.contains(value) && port_available(value)) {
            port = value;
            break;
        }
    }
    if (port == 0) throw std::length_error("data port range exhausted");

    auto session = std::make_unique<Impl::Session>();
    session->attempt = attempt;
    do {
        session->id = random_hex(16);
    } while (impl_->sessions.contains(session->id));
    session->port = port;
    session->worker = std::make_unique<tracy::Worker>("127.0.0.1", port,
                                                      impl_->limits.memory_limit);
    const auto id = session->id;
    impl_->allocated_ports.insert(port);
    impl_->attempt_to_session.emplace(attempt.attempt_id, id);
    impl_->sessions.emplace(id, std::move(session));
    impl_->write_manifest_locked();
    return {id, port};
}

SessionInfo Collector::status(const std::string& session_id) {
    poll();
    std::lock_guard lock(impl_->mutex);
    return impl_->info(impl_->find(session_id));
}

std::vector<SessionInfo> Collector::list() {
    poll();
    std::lock_guard lock(impl_->mutex);
    std::vector<SessionInfo> result;
    result.reserve(impl_->sessions.size());
    for (const auto& item : impl_->sessions) result.push_back(impl_->info(*item.second));
    std::sort(result.begin(), result.end(), [](const SessionInfo& left,
                                               const SessionInfo& right) {
        return left.session_id < right.session_id;
    });
    return result;
}

std::string Collector::decide(const std::string& session_id, Decision decision,
                              const std::string& source) {
    if (source.size() > 4096) throw std::invalid_argument("decision source exceeds limit");
    std::lock_guard lock(impl_->mutex);
    auto& session = impl_->find(session_id);
    if (session.has_decision && session.decision != decision) {
        throw std::domain_error("conflicting decision");
    }
    if (!session.has_decision) {
        session.has_decision = true;
        session.decision = decision;
        session.decision_source = source;
        impl_->touch(session);
    }
    impl_->apply_terminal_decision(session);
    impl_->write_manifest_locked();
    return state_name(session.state);
}

std::string Collector::acquire_owner(const std::string& run_id) {
    std::lock_guard lock(impl_->mutex);
    if (run_id != impl_->run_id) throw std::invalid_argument("run ID mismatch");
    if (impl_->owner_active &&
        Clock::now() - impl_->owner_heartbeat <= impl_->limits.owner_timeout) {
        throw std::runtime_error("owner lease already held");
    }
    impl_->owner_lease = random_hex(16);
    impl_->owner_heartbeat = Clock::now();
    impl_->owner_active = true;
    return impl_->owner_lease;
}

void Collector::heartbeat(const std::string& lease_id) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->owner_active || lease_id != impl_->owner_lease) {
        throw std::runtime_error("invalid owner lease");
    }
    impl_->owner_heartbeat = Clock::now();
}

std::uint32_t Collector::finalize(const std::string& lease_id,
                                  const std::string& source,
                                  bool disconnect_connected) {
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->owner_active || lease_id != impl_->owner_lease) {
            throw std::runtime_error("invalid owner lease");
        }
    }
    force_finalize(source, disconnect_connected);
    std::lock_guard lock(impl_->mutex);
    std::uint32_t unresolved = 0;
    for (const auto& item : impl_->sessions) {
        if (!terminal(item.second->state)) ++unresolved;
    }
    return unresolved;
}

void Collector::force_finalize(const std::string& source,
                               bool disconnect_connected) {
    std::lock_guard lock(impl_->mutex);
    impl_->is_finalizing = true;
    impl_->owner_active = false;
    for (auto& item : impl_->sessions) {
        auto& session = *item.second;
        if (!session.has_decision) {
            session.has_decision = true;
            session.decision = Decision::save;
            session.decision_source = source;
            impl_->touch(session);
        }
        if (disconnect_connected && session.worker) session.worker->Disconnect();
        impl_->apply_terminal_decision(session);
    }
    impl_->write_manifest_locked();
}

void Collector::poll() {
    std::lock_guard lock(impl_->mutex);
    bool changed = false;
    for (auto& item : impl_->sessions) {
        auto& session = *item.second;
        if (terminal(session.state) || !session.worker) continue;

        if (session.worker->HasData()) {
            if (!session.had_data) {
                session.had_data = true;
                session.state = State::capturing;
                impl_->touch(session);
                changed = true;
            }
            if (!session.worker->IsConnected()) {
                session.state = State::awaiting_decision;
                impl_->touch(session);
                changed = true;
                impl_->apply_terminal_decision(session);
            }
        } else {
            const auto handshake = session.worker->GetHandshakeStatus();
            const bool rejected = handshake == tracy::HandshakeProtocolMismatch ||
                                  handshake == tracy::HandshakeNotAvailable ||
                                  handshake == tracy::HandshakeDropped;
            const bool timed_out = Clock::now() - session.registered_at >=
                                   impl_->limits.connect_timeout;
            if (rejected || timed_out) {
                session.worker->Disconnect();
                session.state = State::failed_to_connect;
                session.error = rejected ? "Tracy handshake failed: " + handshake_name(session.worker.get())
                                         : "Tracy connection timeout";
                impl_->touch(session);
                changed = true;
                impl_->apply_terminal_decision(session);
            }
        }
    }
    if (changed) impl_->write_manifest_locked();
}

bool Collector::owner_expired() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->owner_active &&
           Clock::now() - impl_->owner_heartbeat > impl_->limits.owner_timeout;
}

bool Collector::finalizing() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->is_finalizing;
}

bool Collector::all_terminal() const {
    std::lock_guard lock(impl_->mutex);
    for (const auto& item : impl_->sessions) {
        if (!terminal(item.second->state)) return false;
    }
    return true;
}

bool Collector::has_failures() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->failures;
}

std::chrono::milliseconds Collector::finalize_timeout() const {
    return impl_->limits.finalize_timeout;
}

std::string Collector::sanitize_display(const std::string& text) {
    std::string result;
    result.reserve(std::min<std::size_t>(text.size(), 40));
    bool last_dash = false;
    for (const unsigned char c : text) {
        if (result.size() >= 40) break;
        if (std::isalnum(c)) {
            result.push_back(static_cast<char>(std::tolower(c)));
            last_dash = false;
        } else if (!result.empty() && !last_dash) {
            result.push_back('-');
            last_dash = true;
        }
    }
    while (!result.empty() && result.back() == '-') result.pop_back();
    return result;
}

}  // namespace tracy_collector
