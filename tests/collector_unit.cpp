#include "tracy_collector/collector.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main() {
    try {
        require(tracy_collector::Collector::sanitize_display("Suite::A/B test") ==
                    "suite-a-b-test",
                "display sanitization mismatch");
        require(tracy_collector::Collector::sanitize_display("../../") == "",
                "path-like display was not removed");
        require(tracy_collector::Collector::sanitize_display(std::string(100, 'A')).size() == 40,
                "display bound mismatch");

        const auto root = std::filesystem::temp_directory_path() /
                          "tracy-collector-unit-output";
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        tracy_collector::Limits limits;
        limits.data_port_first = 45381;
        limits.data_port_last = 45390;
        limits.max_sessions = 1;
        limits.connect_timeout = std::chrono::milliseconds(50);
        tracy_collector::Collector collector(root, "run", limits);
        const tracy_collector::Attempt attempt{
            "run", "attempt-1", "fixture", "../../unsafe test", 0, "stress=1"};
        const auto registration = collector.register_attempt(attempt);
        require(!registration.session_id.empty(), "empty session ID");
        require(registration.port >= limits.data_port_first &&
                    registration.port <= limits.data_port_last,
                "assigned port out of range");
        const auto state = collector.decide(registration.session_id,
                                            tracy_collector::Decision::discard,
                                            "unit-test");
        require(state == "discarded", "discard did not become terminal");
        require(collector.decide(registration.session_id,
                                 tracy_collector::Decision::discard,
                                 "unit-test") == "discarded",
                "same decision was not idempotent");
        bool conflict = false;
        try {
            collector.decide(registration.session_id,
                             tracy_collector::Decision::save,
                             "unit-test");
        } catch (const std::domain_error&) {
            conflict = true;
        }
        require(conflict, "conflicting decision was accepted");
        bool session_limit = false;
        try {
            collector.register_attempt({"run", "attempt-2", "fixture", "second", 0, ""});
        } catch (const std::length_error&) {
            session_limit = true;
        }
        require(session_limit, "configured session limit was not enforced");
        require(std::filesystem::exists(root / "manifest.json"), "manifest missing");
        for (const auto& item : std::filesystem::directory_iterator(root)) {
            require(item.path().extension() != ".tracy", "discard created a trace");
            require(item.path().extension() != ".partial", "discard created a partial");
        }
        std::filesystem::remove_all(root, ignored);

        const auto port_root = std::filesystem::temp_directory_path() /
                               "tracy-collector-port-unit-output";
        std::filesystem::remove_all(port_root, ignored);
        limits.max_sessions = 3;
        limits.data_port_first = 45401;
        limits.data_port_last = 45401;
        tracy_collector::Collector port_collector(port_root, "ports", limits);
        const auto first = port_collector.register_attempt(
            {"ports", "port-1", "fixture", "first", 0, ""});
        bool exhausted = false;
        try {
            port_collector.register_attempt(
                {"ports", "port-2", "fixture", "second", 0, ""});
        } catch (const std::length_error&) {
            exhausted = true;
        }
        require(exhausted, "single-port range was not exhausted");
        port_collector.decide(first.session_id, tracy_collector::Decision::discard,
                              "unit-test");
        const auto reused = port_collector.register_attempt(
            {"ports", "port-2", "fixture", "second", 0, ""});
        require(reused.port == first.port, "terminal session port was not reused");
        port_collector.decide(reused.session_id, tracy_collector::Decision::discard,
                              "unit-test");
        std::filesystem::remove_all(port_root, ignored);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "collector unit failure: " << error.what() << '\n';
        return 1;
    }
}
