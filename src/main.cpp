#include <exception>
#include <iostream>
#include <memory>
#include <string_view>

#include <TracyFileRead.hpp>
#include <TracyWorker.hpp>

namespace {

void print_usage(std::ostream& output, const std::string_view program) {
    output << "Usage: " << program << " <trace-file>\n"
           << "Load a Tracy 0.13.1-compatible trace and exit.\n";
}

}  // namespace

int main(const int argc, char* argv[]) {
    const std::string_view program = argc > 0 ? argv[0] : "tracy-query";

    if (argc == 2 && (std::string_view{argv[1]} == "-h" ||
                      std::string_view{argv[1]} == "--help")) {
        print_usage(std::cout, program);
        return 0;
    }

    if (argc != 2) {
        print_usage(std::cerr, program);
        return 2;
    }

    try {
        auto trace = std::unique_ptr<tracy::FileRead>{tracy::FileRead::Open(argv[1])};
        if (!trace) {
            std::cerr << "tracy-query: cannot open trace file: " << argv[1] << '\n';
            return 1;
        }

        // Worker parses the complete capture in its file-loading constructor.
        const tracy::Worker worker{*trace};
    } catch (const tracy::NotTracyDump&) {
        std::cerr << "tracy-query: not a Tracy trace file: " << argv[1] << '\n';
        return 1;
    } catch (const tracy::UnsupportedVersion& error) {
        std::cerr << "tracy-query: unsupported trace version " << error.version << ": "
                  << argv[1] << '\n';
        return 1;
    } catch (const tracy::LegacyVersion& error) {
        std::cerr << "tracy-query: legacy trace version " << error.version
                  << " is not supported: " << argv[1] << '\n';
        return 1;
    } catch (const tracy::LoadFailure& error) {
        std::cerr << "tracy-query: failed to load trace: " << error.msg << '\n';
        return 1;
    } catch (const tracy::FileReadError&) {
        std::cerr << "tracy-query: failed to read trace file: " << argv[1] << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "tracy-query: failed to load trace: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
