#include "tracy_query/time.hpp"

#include <charconv>
#include <limits>

namespace tracy_query {
namespace {

bool checked_multiply(const uint64_t left, const uint64_t right, uint64_t& result) {
    if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) return false;
    result = left * right;
    return true;
}

}  // namespace

std::optional<ParsedTime> parse_time(const std::string_view text, std::string& error) {
    error.clear();
    if (text == "start") return ParsedTime{ParsedTime::Special::Start, 0};
    if (text == "end") return ParsedTime{ParsedTime::Special::End, 0};
    if (text.empty()) {
        error = "time value is empty";
        return std::nullopt;
    }

    size_t number_end = 0;
    while (number_end < text.size() && text[number_end] >= '0' && text[number_end] <= '9') {
        ++number_end;
    }
    const auto integer_end = number_end;
    if (number_end < text.size() && text[number_end] == '.') {
        ++number_end;
        const auto fraction_start = number_end;
        while (number_end < text.size() && text[number_end] >= '0' && text[number_end] <= '9') {
            ++number_end;
        }
        if (number_end == fraction_start) {
            error = "fractional time requires digits after the decimal point";
            return std::nullopt;
        }
    }
    if (integer_end == 0) {
        error = "time must be a non-negative number or 'start'/'end'";
        return std::nullopt;
    }

    const auto unit = text.substr(number_end);
    uint64_t multiplier = 0;
    if (unit.empty() || unit == "ns") multiplier = 1;
    else if (unit == "us") multiplier = 1'000;
    else if (unit == "ms") multiplier = 1'000'000;
    else if (unit == "s") multiplier = 1'000'000'000;
    else if (unit == "m") multiplier = 60'000'000'000;
    else if (unit == "h") multiplier = 3'600'000'000'000;
    else {
        error = "unknown time unit '" + std::string{unit} + "'";
        return std::nullopt;
    }

    uint64_t whole = 0;
    const auto [whole_end, whole_error] = std::from_chars(text.data(), text.data() + integer_end, whole);
    if (whole_error != std::errc{} || whole_end != text.data() + integer_end) {
        error = "time value is too large";
        return std::nullopt;
    }

    uint64_t nanoseconds = 0;
    if (!checked_multiply(whole, multiplier, nanoseconds)) {
        error = "time value is too large";
        return std::nullopt;
    }

    if (integer_end < number_end) {
        const auto fraction_begin = integer_end + 1;
        const auto fraction_length = number_end - fraction_begin;
        uint64_t fraction = 0;
        if (fraction_length > 18) {
            error = "fractional time has too many digits";
            return std::nullopt;
        }
        const auto [fraction_end, fraction_error] =
            std::from_chars(text.data() + fraction_begin, text.data() + number_end, fraction);
        if (fraction_error != std::errc{} || fraction_end != text.data() + number_end) {
            error = "invalid fractional time";
            return std::nullopt;
        }
        uint64_t scale = 1;
        for (size_t i = 0; i < fraction_length; ++i) scale *= 10;
        uint64_t scaled_fraction = 0;
        if (!checked_multiply(fraction, multiplier, scaled_fraction)) {
            error = "time value is too large";
            return std::nullopt;
        }
        if (scaled_fraction % scale != 0) {
            error = "time value has sub-nanosecond precision";
            return std::nullopt;
        }
        const auto fractional_ns = scaled_fraction / scale;
        if (nanoseconds > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - fractional_ns) {
            error = "time value is too large";
            return std::nullopt;
        }
        nanoseconds += fractional_ns;
    }

    if (nanoseconds > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        error = "time value is too large";
        return std::nullopt;
    }
    return ParsedTime{ParsedTime::Special::None, static_cast<int64_t>(nanoseconds)};
}

int64_t normalize_timestamp(const int64_t tracy_timestamp, const int64_t first_timestamp) {
    if (tracy_timestamp >= first_timestamp) return tracy_timestamp - first_timestamp;
    return -(first_timestamp - tracy_timestamp);
}

}  // namespace tracy_query
