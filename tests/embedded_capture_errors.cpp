#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "tracy_embedded_capture/embedded_capture.h"

namespace {

int expect(int actual, int expected, const char* context) {
    if (actual != expected) {
        std::cerr << context << ": expected " << expected << ", got " << actual << '\n';
        return 1;
    }
    char message[256]{};
    if (___tracy_embedded_capture_get_error(message, sizeof(message)) == 0 || message[0] == '\0') {
        std::cerr << context << ": expected a diagnostic\n";
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const std::string mode = argv[1];
    const std::filesystem::path output = argv[2];
    std::error_code ignored;
    std::filesystem::remove(output, ignored);

    if (mode == "invalid") {
        return expect(___tracy_embedded_capture_configure(nullptr, 1, 1, 1),
                      TRACY_EMBEDDED_CAPTURE_INVALID_ARGUMENT, "invalid configure");
    }
    if (mode == "existing") {
        std::ofstream(output) << "sentinel";
        const auto text = output.string();
        return expect(___tracy_embedded_capture_configure(
                          text.data(), text.size(), 4096, 1024 * 1024),
                      TRACY_EMBEDDED_CAPTURE_OUTPUT_EXISTS, "existing output");
    }

    const auto text = output.string();
    const auto configured = ___tracy_embedded_capture_configure(
        text.data(), text.size(), 4096, 16 * 1024 * 1024);
    if (configured != TRACY_EMBEDDED_CAPTURE_OK) {
        std::cerr << "initial configure failed: " << configured << '\n';
        return 1;
    }
    if (mode == "duplicate") {
        return expect(___tracy_embedded_capture_configure(
                          text.data(), text.size(), 4096, 16 * 1024 * 1024),
                      TRACY_EMBEDDED_CAPTURE_INVALID_STATE, "duplicate configure");
    }
    if (mode == "invalid-disposition") {
        const auto result = expect(
            ___tracy_embedded_capture_finish_with_disposition(99),
            TRACY_EMBEDDED_CAPTURE_INVALID_ARGUMENT, "invalid disposition");
        if (result != 0) return result;
        return ___tracy_embedded_capture_get_state() ==
                       TRACY_EMBEDDED_CAPTURE_CONFIGURED
                   ? 0
                   : 1;
    }
    if (mode == "no-data") {
        const auto result = expect(___tracy_embedded_capture_finish(),
                                   TRACY_EMBEDDED_CAPTURE_NO_DATA, "finish without client");
        if (std::filesystem::exists(output)) {
            std::cerr << "no-data finalization published an output\n";
            return 1;
        }
        if (result != 0) return result;
        return expect(___tracy_embedded_capture_finish(),
                      TRACY_EMBEDDED_CAPTURE_INVALID_STATE,
                      "repeated finish after failure");
    }
    return 2;
}
