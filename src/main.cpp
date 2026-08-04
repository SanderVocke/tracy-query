#include <csignal>
#include <iostream>
#include <vector>

#include "tracy_query/cli.hpp"
#include "tracy_query/commands.hpp"
#include "tracy_query/output.hpp"
#include "tracy_query/trace.hpp"

int main(const int argc, char* argv[]) {
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif

    const auto parsed = tracy_query::parse_cli(argc, argv);
    if (parsed.should_exit) {
        (parsed.message_to_stderr ? std::cerr : std::cout) << parsed.message;
        return parsed.exit_code;
    }

    try {
        std::vector<tracy_query::Trace> traces;
        traces.reserve(parsed.options->traces.size());
        for (const auto& input : parsed.options->traces) traces.emplace_back(input);
        return tracy_query::run_command(*parsed.options, traces, std::cout, std::cerr);
    } catch (const tracy_query::OutputError&) {
        // A closed stdout pipe is a normal consumer-driven termination.
        return 0;
    } catch (const tracy_query::TraceLoadError& error) {
        std::cerr << "tracy-query: " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "tracy-query: " << error.what() << '\n';
        return 1;
    }
}
