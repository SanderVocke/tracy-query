#include "tracy_query/cli.hpp"

#include "tracy_query/adapter.hpp"
#include "tracy_query/time.hpp"

#include <algorithm>
#include <charconv>
#include <iostream>
#include <limits>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace tracy_query {
namespace {

ParseResult failure(std::string message) {
    return ParseResult{std::nullopt, true, 2, "tracy-query: " + std::move(message) + "\n",
                       true};
}

std::optional<Command> parse_command(const std::string_view text) {
    if (text == "check") return Command::Check;
    if (text == "range") return Command::Range;
    if (text == "info") return Command::Info;
    if (text == "sources") return Command::Sources;
    if (text == "query") return Command::Query;
    return std::nullopt;
}

std::string_view command_name(const Command command) {
    switch (command) {
    case Command::Check: return "check";
    case Command::Range: return "range";
    case Command::Info: return "info";
    case Command::Sources: return "sources";
    case Command::Query: return "query";
    }
    return "unknown";
}

bool parse_u64(const std::string_view text, uint64_t& result) {
    if (text.empty()) return false;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    return error == std::errc{} && end == text.data() + text.size();
}

bool parse_u32(const std::string_view text, uint32_t& result) {
    uint64_t value = 0;
    if (!parse_u64(text, value) || value > std::numeric_limits<uint32_t>::max()) return false;
    result = static_cast<uint32_t>(value);
    return true;
}

std::vector<std::string_view> split(const std::string_view text, const char delimiter) {
    std::vector<std::string_view> result;
    size_t begin = 0;
    while (begin <= text.size()) {
        const auto end = text.find(delimiter, begin);
        result.emplace_back(text.substr(begin, end == std::string_view::npos ? end : end - begin));
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return result;
}

bool has_kind(const std::vector<Kind>& kinds, const Kind kind) {
    return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}

}  // namespace

ParseResult parse_cli(const int argc, char* argv[]) {
    const std::string program = argc > 0 ? argv[0] : "tracy-query";
    if (argc <= 1) return failure("missing command or trace file (try '" + program + " --help')");

    Options options;
    bool command_set = false;
    bool positional_only = false;
    bool query_option_seen = false;
    std::vector<std::pair<std::string, std::string>> labels;

    auto required_value = [&](int& index,
                              const std::optional<std::string_view> inline_value)
        -> std::optional<std::string_view> {
        if (inline_value) return inline_value;
        if (index + 1 >= argc) return std::nullopt;
        ++index;
        return std::string_view{argv[index]};
    };

    for (int i = 1; i < argc; ++i) {
        std::string_view argument{argv[i]};
        if (!positional_only && argument == "--") {
            positional_only = true;
            continue;
        }
        if (!positional_only && (argument == "-h" || argument == "--help")) {
            std::ostringstream output;
            print_help(output, program, command_set ? std::optional{options.command} : std::nullopt);
            return ParseResult{std::nullopt, true, 0, output.str(), false};
        }
        if (!positional_only && argument == "--version") {
            std::ostringstream output;
            print_version(output);
            return ParseResult{std::nullopt, true, 0, output.str(), false};
        }
        if (!command_set && !positional_only) {
            if (const auto command = parse_command(argument)) {
                options.command = *command;
                command_set = true;
                continue;
            }
        }
        if (!command_set && (positional_only || argument.empty() || argument.front() != '-')) {
            options.command = Command::Check;
            command_set = true;
            options.traces.push_back({std::string{argument}, std::string{argument}});
            continue;
        }

        if (!positional_only && argument.starts_with("--")) {
            std::optional<std::string_view> inline_value;
            const auto equals = argument.find('=');
            const auto name = equals == std::string_view::npos ? argument : argument.substr(0, equals);
            if (equals != std::string_view::npos) inline_value = argument.substr(equals + 1);

            auto value_for = [&](const std::string_view) -> std::optional<std::string_view> {
                return required_value(i, inline_value);
            };
            auto missing = [&] { return failure("option '" + std::string{name} + "' requires a value"); };

            if (name == "--quiet") options.quiet = true;
            else if (name == "--ignore-case") { options.ignore_case = true; query_option_seen = true; }
            else if (name == "--count") { options.count = true; query_option_seen = true; }
            else if (name == "--latest") { options.time.latest = true; query_option_seen = true; }
            else if (name == "--next") { options.time.next = true; query_option_seen = true; }
            else if (name == "--active") { options.time.active = true; query_option_seen = true; }
            else if (name == "--root-zones") { options.root_zones = true; query_option_seen = true; }
            else if (name == "--format") {
                const auto value = value_for(name); if (!value) return missing();
                if (*value == "jsonl") options.format = OutputFormat::JsonLines;
                else if (*value == "text") options.format = OutputFormat::Text;
                else return failure("unknown format '" + std::string{*value} + "'");
            } else if (name == "--detail") {
                const auto value = value_for(name); if (!value) return missing();
                if (*value == "basic") options.detail = Detail::Basic;
                else if (*value == "full") options.detail = Detail::Full;
                else return failure("unknown detail level '" + std::string{*value} + "'");
            } else if (name == "--trace-label") {
                const auto value = value_for(name); if (!value) return missing();
                const auto split_at = value->find('=');
                if (split_at == std::string_view::npos || split_at == 0 || split_at + 1 == value->size()) {
                    return failure("trace label must have PATH=LABEL form");
                }
                labels.emplace_back(std::string{value->substr(0, split_at)},
                                    std::string{value->substr(split_at + 1)});
            } else if (name == "--kind") {
                const auto value = value_for(name); if (!value) return missing();
                for (const auto item : split(*value, ',')) {
                    if (item == "all") { options.all_kinds = true; continue; }
                    const auto kind = parse_kind(item);
                    if (!kind) return failure("unknown data kind '" + std::string{item} + "'");
                    if (!has_kind(options.kinds, *kind)) options.kinds.push_back(*kind);
                }
                query_option_seen = true;
            } else if (name == "--from" || name == "--to" || name == "--at") {
                const auto value = value_for(name); if (!value) return missing();
                std::string error;
                const auto parsed = parse_time(*value, error);
                if (!parsed) return failure("invalid value for " + std::string{name} + ": " + error);
                if (name == "--from") {
                    if (parsed->special == ParsedTime::Special::End) options.time.from_ns = std::numeric_limits<int64_t>::max();
                    else if (parsed->special == ParsedTime::Special::Start) options.time.from_start = true;
                    else options.time.from_ns = parsed->nanoseconds;
                } else if (name == "--to") {
                    if (parsed->special == ParsedTime::Special::End) options.time.to_end = true;
                    else if (parsed->special == ParsedTime::Special::Start) options.time.to_ns = 0;
                    else options.time.to_ns = parsed->nanoseconds;
                } else {
                    if (parsed->special != ParsedTime::Special::None) {
                        return failure("--at requires a numeric relative time");
                    }
                    options.time.at_ns = parsed->nanoseconds;
                }
                query_option_seen = true;
            } else if (name == "--range-match") {
                const auto value = value_for(name); if (!value) return missing();
                if (*value == "overlap") options.time.range_match = RangeMatch::Overlap;
                else if (*value == "start") options.time.range_match = RangeMatch::Start;
                else if (*value == "contained") options.time.range_match = RangeMatch::Contained;
                else return failure("unknown range match mode '" + std::string{*value} + "'");
                query_option_seen = true;
            } else if (name == "--limit") {
                const auto value = value_for(name); if (!value) return missing();
                uint64_t parsed = 0; if (!parse_u64(*value, parsed)) return failure("invalid --limit value");
                options.limit = parsed; query_option_seen = true;
            } else if (name == "--group-by") {
                const auto value = value_for(name); if (!value) return missing();
                if (*value != "trace" && *value != "kind" && *value != "source") {
                    return failure("--group-by expects trace, kind, or source");
                }
                options.group_by.emplace_back(*value); query_option_seen = true;
            } else if (name == "--filter") {
                const auto value = value_for(name); if (!value) return missing();
                const auto split_at = value->find('=');
                if (split_at == std::string_view::npos || split_at == 0) return failure("filter must have FIELD=REGEX form");
                auto field = value->substr(0, split_at);
                RegexFilter filter;
                if (const auto dot = field.find('.'); dot != std::string_view::npos) {
                    const auto kind = parse_kind(field.substr(0, dot));
                    if (!kind) return failure("unknown filter kind '" + std::string{field.substr(0, dot)} + "'");
                    filter.kind = *kind;
                    field.remove_prefix(dot + 1);
                }
                if (field.empty()) return failure("filter field is empty");
                filter.field = field;
                filter.pattern = value->substr(split_at + 1);
                options.filters.push_back(std::move(filter)); query_option_seen = true;
            } else if (name == "--zone-depth") {
                const auto value = value_for(name); if (!value) return missing();
                const auto parts = split(*value, ':');
                uint32_t minimum = 0, maximum = 0;
                if (parts.size() == 1) {
                    if (!parse_u32(parts[0], minimum)) return failure("invalid --zone-depth value");
                    maximum = minimum;
                } else if (parts.size() == 2 && parse_u32(parts[0], minimum) && parse_u32(parts[1], maximum) && minimum <= maximum) {
                } else return failure("--zone-depth expects N or MIN:MAX");
                options.zone_depth = std::pair{minimum, maximum}; query_option_seen = true;
            } else {
                auto append_string = [&](std::vector<std::string>& target) -> ParseResult {
                    const auto value = value_for(name); if (!value) return missing();
                    target.emplace_back(*value); query_option_seen = true; return {};
                };
                if (name == "--source") { auto result = append_string(options.source_ids); if (result.should_exit) return result; }
                else if (name == "--source-regex") { auto result = append_string(options.source_regexes); if (result.should_exit) return result; }
                else if (name == "--source-type") { auto result = append_string(options.source_types); if (result.should_exit) return result; }
                else if (name == "--thread") { auto result = append_string(options.threads); if (result.should_exit) return result; }
                else if (name == "--gpu-context") { auto result = append_string(options.gpu_contexts); if (result.should_exit) return result; }
                else if (name == "--plot") { auto result = append_string(options.plots); if (result.should_exit) return result; }
                else if (name == "--frame-set") { auto result = append_string(options.frame_sets); if (result.should_exit) return result; }
                else if (name == "--lock") { auto result = append_string(options.locks); if (result.should_exit) return result; }
                else if (name == "--memory-pool") { auto result = append_string(options.memory_pools); if (result.should_exit) return result; }
                else if (name == "--zone-parent" || name == "--zone-ancestor" || name == "--stack-frame") {
                    const auto value = value_for(name); if (!value) return missing();
                    if (name == "--zone-parent") options.zone_parent = std::string{*value};
                    else if (name == "--zone-ancestor") options.zone_ancestor = std::string{*value};
                    else options.stack_frame = std::string{*value};
                    query_option_seen = true;
                } else if (name == "--cpu") {
                    const auto value = value_for(name); if (!value) return missing();
                    uint32_t cpu = 0; if (!parse_u32(*value, cpu)) return failure("invalid --cpu value");
                    options.cpus.push_back(cpu); query_option_seen = true;
                } else return failure("unknown option '" + std::string{name} + "'");
            }
            continue;
        }

        if (!command_set) return failure("unknown command '" + std::string{argument} + "'");
        options.traces.push_back({std::string{argument}, std::string{argument}});
    }

    if (!command_set) return failure("missing command or trace file");
    if (options.traces.empty()) return failure("command '" + std::string{command_name(options.command)} + "' requires at least one trace file");

    for (const auto& [path, label] : labels) {
        auto found = false;
        for (auto& trace : options.traces) {
            if (trace.path == path) { trace.label = label; found = true; }
        }
        if (!found) return failure("--trace-label path is not an input trace: " + path);
    }

    if (options.all_kinds) options.kinds = all_kinds();

    if (options.command == Command::Query) {
        if (options.kinds.empty()) return failure("query requires at least one --kind");
        const bool range = options.time.from_ns || options.time.to_ns || options.time.from_start || options.time.to_end;
        if (options.time.at_ns && range) return failure("--at cannot be combined with --from or --to");
        if (options.time.at_ns && !options.time.latest && !options.time.next && !options.time.active) {
            return failure("--at requires --latest, --next, or --active");
        }
        if (!options.time.at_ns && (options.time.latest || options.time.next || options.time.active)) {
            return failure("--latest, --next, and --active require --at");
        }
        if (options.time.from_ns && options.time.to_ns && *options.time.from_ns > *options.time.to_ns) {
            return failure("--from must not be later than --to");
        }
        if (options.time.active) {
            for (const auto kind : options.kinds) {
                if (!adapter_for(kind).interval) return failure("--active is not valid for point kind " + std::string{kind_name(kind)});
            }
        }
        const bool zone_structure = options.root_zones || options.zone_depth || options.zone_parent || options.zone_ancestor;
        if (zone_structure) {
            for (const auto kind : options.kinds) {
                if (kind != Kind::CpuZone && kind != Kind::GpuZone && kind != Kind::GhostZone) {
                    return failure("zone structure options require zone kinds");
                }
                if ((options.zone_parent || options.zone_ancestor) && kind == Kind::GhostZone) {
                    return failure("parent-name and ancestor-name filters are not available for ghost zones");
                }
            }
        }
        for (const auto& filter : options.filters) {
            const auto field_valid = [&](const Kind kind) {
                const auto fields = adapter_for(kind).filter_fields;
                return std::find(fields.begin(), fields.end(), filter.field) != fields.end();
            };
            if (filter.kind) {
                if (!field_valid(*filter.kind)) return failure("field '" + filter.field + "' is not valid for " + std::string{kind_name(*filter.kind)});
            } else {
                for (const auto kind : options.kinds) {
                    if (!field_valid(kind)) return failure("unscoped field '" + filter.field + "' is not valid for " + std::string{kind_name(kind)});
                }
            }
        }
    } else if (options.command == Command::Sources) {
        for (const auto& filter : options.filters) {
            if (filter.field != "name" && filter.field != "source" && filter.field != "source_type") {
                return failure("sources filters support only name, source, and source_type fields");
            }
        }
    } else if (query_option_seen) {
        return failure("query options are not valid for command '" + std::string{command_name(options.command)} + "'");
    }

    const auto regex_flags = std::regex::ECMAScript |
        (options.ignore_case ? std::regex::icase : std::regex_constants::syntax_option_type{});
    const auto validate_regex = [&](const std::string& pattern) -> std::optional<ParseResult> {
        try { static_cast<void>(std::regex{pattern, regex_flags}); }
        catch (const std::regex_error& error) {
            return failure("invalid regular expression '" + pattern + "': " + error.what());
        }
        return std::nullopt;
    };
    std::vector<std::string> regexes;
    regexes.insert(regexes.end(), options.source_regexes.begin(), options.source_regexes.end());
    regexes.insert(regexes.end(), options.threads.begin(), options.threads.end());
    regexes.insert(regexes.end(), options.gpu_contexts.begin(), options.gpu_contexts.end());
    regexes.insert(regexes.end(), options.plots.begin(), options.plots.end());
    regexes.insert(regexes.end(), options.frame_sets.begin(), options.frame_sets.end());
    regexes.insert(regexes.end(), options.locks.begin(), options.locks.end());
    regexes.insert(regexes.end(), options.memory_pools.begin(), options.memory_pools.end());
    for (const auto& filter : options.filters) regexes.push_back(filter.pattern);
    if (options.zone_parent) regexes.push_back(*options.zone_parent);
    if (options.zone_ancestor) regexes.push_back(*options.zone_ancestor);
    if (options.stack_frame) regexes.push_back(*options.stack_frame);
    for (const auto& pattern : regexes) {
        if (const auto invalid = validate_regex(pattern)) return *invalid;
    }

    return ParseResult{std::move(options), false, 0, {}, false};
}

void print_help(std::ostream& output, const std::string_view program,
                const std::optional<Command> command) {
    if (!command) {
        output << "Usage: " << program << " [GLOBAL OPTIONS] <COMMAND> [OPTIONS] <TRACE>...\n\n"
               << "Query Tracy 0.13.1-compatible capture files.\n\n"
               << "Commands:\n"
               << "  check    Load and validate captures\n"
               << "  range    Print capture time ranges\n"
               << "  info     Print capture metadata and counts\n"
               << "  sources  List typed data sources\n"
               << "  query    Query timestamped records\n\n"
               << "Compatibility: " << program << " <TRACE> is the same as 'check <TRACE>'.\n"
               << "Use '" << program << " <COMMAND> --help' for command help.\n\n"
               << "Global options: --format jsonl|text, --trace-label PATH=LABEL, --quiet,\n"
               << "                --help, --version\n";
        return;
    }
    output << "Usage: " << program << ' ' << command_name(*command) << " [OPTIONS] <TRACE>...\n\n";
    switch (*command) {
    case Command::Check:
        output << "Load each capture and exit.\n";
        break;
    case Command::Range:
        output << "Emit first timestamp, last timestamp, and duration for each capture.\n";
        break;
    case Command::Info:
        output << "Emit capture metadata and data-kind counts.\nOptions: --detail basic|full\n";
        break;
    case Command::Sources:
        output << "List typed natural sources.\nOptions: --kind, --source, --source-regex, --source-type, and scope selectors.\n";
        break;
    case Command::Query:
        output << "Query timestamped records.\n"
               << "Required: --kind KIND[,KIND...] (or --kind all)\n"
               << "Time: --from TIME --to TIME | --at TIME [--latest] [--next] [--active]\n"
               << "Filter: --filter [KIND.]FIELD=REGEX, source/scope options, --ignore-case\n"
               << "Output: --count, --group-by trace|kind|source, --limit N, --detail basic|full\n";
        break;
    }
}

void print_version(std::ostream& output) {
    output << "tracy-query 0.2.0 (Tracy parser 0.13.1)\n";
}

}  // namespace tracy_query
