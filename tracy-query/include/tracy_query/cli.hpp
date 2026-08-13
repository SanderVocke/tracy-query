#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

#include "tracy_query/model.hpp"

namespace tracy_query {

struct ParseResult {
    std::optional<Options> options;
    bool should_exit = false;
    int exit_code = 0;
    std::string message;
    bool message_to_stderr = false;
};

ParseResult parse_cli(int argc, char* argv[]);
void print_help(std::ostream& output, std::string_view program,
                std::optional<Command> command = std::nullopt);
void print_version(std::ostream& output);

}  // namespace tracy_query
