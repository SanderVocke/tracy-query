#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace tracy_query {

enum class Command { Check, Range, Info, Sources, Query };
enum class OutputFormat { JsonLines, Text };
enum class Detail { Basic, Full };
enum class RangeMatch { Overlap, Start, Contained };
enum class Kind {
    Message,
    Plot,
    CpuZone,
    GpuZone,
    Frame,
    FrameImage,
    Lock,
    Memory,
    Sample,
    GhostZone,
    ContextSwitch,
    CpuSlice,
    CpuUsage,
    HardwareSample,
    Crash,
};

using Value = std::variant<std::nullptr_t, bool, int64_t, uint64_t, double, std::string>;
using Fields = std::vector<std::pair<std::string, Value>>;

struct TraceInput {
    std::string path;
    std::string label;
};

struct Record {
    Record() = default;
    Record(int64_t timestamp_value, std::optional<int64_t> end_value,
           std::string trace_value, std::string source_value, Kind kind_value,
           uint64_t sequence_value)
        : timestamp_ns(timestamp_value), end_timestamp_ns(end_value),
          trace(std::move(trace_value)), source(std::move(source_value)),
          kind(kind_value), sequence(sequence_value) {}

    int64_t timestamp_ns = 0;
    std::optional<int64_t> end_timestamp_ns;
    std::string trace;
    std::string source;
    Kind kind = Kind::Message;
    uint64_t sequence = 0;
    Fields fields;
    std::vector<std::string> ancestor_names;
};

struct Source {
    Source() = default;
    Source(std::string trace_value, std::string id_value, std::string type_value,
           std::string name_value, std::vector<Kind> kind_values = {})
        : trace(std::move(trace_value)), id(std::move(id_value)), type(std::move(type_value)),
          name(std::move(name_value)), kinds(std::move(kind_values)) {}

    std::string trace;
    std::string id;
    std::string type;
    std::string name;
    std::vector<Kind> kinds;
    Fields counts;
    std::optional<int64_t> first_timestamp_ns;
    std::optional<int64_t> last_timestamp_ns;
};

struct RegexFilter {
    std::optional<Kind> kind;
    std::string field;
    std::string pattern;
};

struct TimeSelection {
    std::optional<int64_t> from_ns;
    std::optional<int64_t> to_ns;
    std::optional<int64_t> at_ns;
    bool from_start = false;
    bool to_end = false;
    bool latest = false;
    bool next = false;
    bool active = false;
    RangeMatch range_match = RangeMatch::Overlap;
};

struct Options {
    Command command = Command::Check;
    OutputFormat format = OutputFormat::JsonLines;
    Detail detail = Detail::Basic;
    bool quiet = false;
    bool ignore_case = false;
    bool count = false;
    bool all_kinds = false;
    bool root_zones = false;
    std::optional<uint64_t> limit;
    std::optional<std::pair<uint32_t, uint32_t>> zone_depth;
    std::optional<std::string> zone_parent;
    std::optional<std::string> zone_ancestor;
    std::optional<std::string> stack_frame;
    std::vector<Kind> kinds;
    std::vector<TraceInput> traces;
    std::vector<std::string> group_by;
    std::vector<std::string> source_ids;
    std::vector<std::string> source_regexes;
    std::vector<std::string> source_types;
    std::vector<std::string> threads;
    std::vector<std::string> gpu_contexts;
    std::vector<std::string> plots;
    std::vector<std::string> frame_sets;
    std::vector<std::string> locks;
    std::vector<std::string> memory_pools;
    std::vector<uint32_t> cpus;
    std::vector<RegexFilter> filters;
    TimeSelection time;
};

std::string_view kind_name(Kind kind);
std::optional<Kind> parse_kind(std::string_view text);
const std::vector<Kind>& all_kinds();

}  // namespace tracy_query
