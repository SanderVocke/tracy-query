#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "tracy_query/adapter.hpp"
#include "tracy_query/cli.hpp"
#include "tracy_query/output.hpp"
#include "tracy_query/time.hpp"

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

tracy_query::ParseResult parse(std::initializer_list<const char*> arguments) {
    std::vector<std::string> storage;
    for (const auto* argument : arguments) storage.emplace_back(argument);
    std::vector<char*> argv;
    for (auto& argument : storage) argv.push_back(argument.data());
    return tracy_query::parse_cli(static_cast<int>(argv.size()), argv.data());
}

void test_times() {
    std::string error;
    auto value = tracy_query::parse_time("start", error);
    require(value && value->special == tracy_query::ParsedTime::Special::Start, "parse start");
    value = tracy_query::parse_time("end", error);
    require(value && value->special == tracy_query::ParsedTime::Special::End, "parse end");
    value = tracy_query::parse_time("123", error);
    require(value && value->nanoseconds == 123, "bare nanoseconds");
    value = tracy_query::parse_time("50us", error);
    require(value && value->nanoseconds == 50'000, "microseconds");
    value = tracy_query::parse_time("20ms", error);
    require(value && value->nanoseconds == 20'000'000, "milliseconds");
    value = tracy_query::parse_time("1.5s", error);
    require(value && value->nanoseconds == 1'500'000'000, "fractional seconds");
    value = tracy_query::parse_time("2m", error);
    require(value && value->nanoseconds == 120'000'000'000, "minutes");
    value = tracy_query::parse_time("1h", error);
    require(value && value->nanoseconds == 3'600'000'000'000, "hours");
    require(!tracy_query::parse_time("", error), "reject empty time");
    require(!tracy_query::parse_time("-1s", error), "reject negative time");
    require(!tracy_query::parse_time("1.5ns", error), "reject sub-nanosecond time");
    require(!tracy_query::parse_time("999999999999999999999h", error), "reject overflow");
    require(!tracy_query::parse_time("1fortnight", error), "reject unknown unit");
}

void test_cli() {
    auto result = parse({"tracy-query", "capture.tracy"});
    require(result.options && result.options->command == tracy_query::Command::Check,
            "bare trace is check");
    require(result.options->traces.size() == 1, "bare trace input");

    result = parse({"tracy-query", "check", "a.tracy", "b.tracy"});
    require(result.options && result.options->traces.size() == 2, "multi-trace check");

    result = parse({"tracy-query", "--format", "text", "range", "a.tracy"});
    require(result.options && result.options->format == tracy_query::OutputFormat::Text,
            "global option before command");

    result = parse({"tracy-query", "info", "--detail", "full", "a.tracy"});
    require(result.options && result.options->detail == tracy_query::Detail::Full,
            "info detail");

    result = parse({"tracy-query", "query", "--kind", "message,plot", "--from", "1s",
                    "--to", "2s", "--filter", "message.text=error|warning", "a.tracy"});
    require(result.options && result.options->kinds.size() == 2, "mixed kinds");
    require(result.options->filters.size() == 1 && result.options->filters[0].kind,
            "scoped filter");

    result = parse({"tracy-query", "query", "--kind", "plot", "--at", "1s", "--latest",
                    "--next", "--plot", "^FPS$", "a.tracy"});
    require(result.options && result.options->time.latest && result.options->time.next,
            "point selectors");

    result = parse({"tracy-query", "query", "--kind", "all", "--count", "--group-by",
                    "kind", "a.tracy"});
    require(result.options && result.options->kinds.size() == tracy_query::all_kinds().size(),
            "all kinds");

    result = parse({"tracy-query", "--trace-label", "a.tracy=first", "check", "a.tracy"});
    require(result.options && result.options->traces[0].label == "first", "trace label");

    require(parse({"tracy-query"}).exit_code == 2, "reject no args");
    require(parse({"tracy-query", "query", "a.tracy"}).exit_code == 2,
            "query requires kind");
    require(parse({"tracy-query", "query", "--kind", "nope", "a.tracy"}).exit_code == 2,
            "reject kind");
    require(parse({"tracy-query", "query", "--kind", "plot", "--at", "1s", "a.tracy"}).exit_code == 2,
            "at requires selector");
    require(parse({"tracy-query", "query", "--kind", "plot", "--latest", "a.tracy"}).exit_code == 2,
            "latest requires at");
    require(parse({"tracy-query", "query", "--kind", "plot", "--from", "2s", "--to", "1s", "a.tracy"}).exit_code == 2,
            "ordered range");
    require(parse({"tracy-query", "check", "--kind", "plot", "a.tracy"}).exit_code == 2,
            "reject query option on check");
    require(parse({"tracy-query", "check", "--wat", "a.tracy"}).exit_code == 2,
            "reject unknown option");
    require(parse({"tracy-query", "query", "--kind", "plot", "--zone-depth", "4:2", "a.tracy"}).exit_code == 2,
            "reject inverted depth");
    require(parse({"tracy-query", "query", "--kind", "plot", "--at", "1s", "--active", "a.tracy"}).exit_code == 2,
            "reject active for point kind");
    require(parse({"tracy-query", "query", "--kind", "plot", "--filter", "plot.nope=x", "a.tracy"}).exit_code == 2,
            "reject unsupported filter field");
    require(parse({"tracy-query", "query", "--kind", "message", "--filter", "message.text=[", "a.tracy"}).exit_code == 2,
            "reject invalid regex before loading");
    require(parse({"tracy-query", "sources", "--filter", "text=x", "a.tracy"}).exit_code == 2,
            "reject unsupported source filter field");

    result = parse({"tracy-query", "query", "--help"});
    require(result.should_exit && result.exit_code == 0 && result.message.find("Query timestamped") != std::string::npos,
            "context help");
}

void test_output() {
    require(tracy_query::json_escape("a\n\"b\\c") == "a\\n\\\"b\\\\c", "JSON escaping");
    std::ostringstream output;
    tracy_query::Record record{10, 15, "trace", "thread:1", tracy_query::Kind::CpuZone, 0};
    record.fields = {{"name", std::string{"line\none"}}};
    tracy_query::emit_record(output, tracy_query::OutputFormat::JsonLines, record);
    const auto rendered = output.str();
    require(rendered.find("\"timestamp_ns\":10") != std::string::npos &&
            rendered.find("\"duration_ns\":5") != std::string::npos &&
            rendered.find("line\\none") != std::string::npos,
            "JSONL record contract");
    require(std::count(rendered.begin(), rendered.end(), '\n') == 1,
            "one physical output line");

    std::ostringstream text_output;
    tracy_query::emit_record(text_output, tracy_query::OutputFormat::Text, record);
    const auto text = text_output.str();
    require(text.starts_with("timestamp_ns=10 end_timestamp_ns=15 duration_ns=5") &&
            std::count(text.begin(), text.end(), '\n') == 1 && text.find("line\\none") != std::string::npos,
            "text record contract");
}

void test_registry() {
    require(tracy_query::adapter_registry().size() == tracy_query::all_kinds().size(),
            "every kind has adapter descriptor");
    for (const auto kind : tracy_query::all_kinds()) {
        const auto& adapter = tracy_query::adapter_for(kind);
        require(adapter.kind == kind && !adapter.source_type.empty() && !adapter.filter_fields.empty(),
                "valid adapter descriptor");
    }
}

}  // namespace

int main() {
    test_times();
    test_cli();
    test_output();
    test_registry();
    return 0;
}
