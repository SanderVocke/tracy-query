#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tracy_query {

struct ParsedTime {
    enum class Special { None, Start, End };
    Special special = Special::None;
    int64_t nanoseconds = 0;
};

std::optional<ParsedTime> parse_time(std::string_view text, std::string& error);
int64_t normalize_timestamp(int64_t tracy_timestamp, int64_t first_timestamp);

}  // namespace tracy_query
