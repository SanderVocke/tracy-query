#include "tracy_query/commands.hpp"

#include "tracy_query/output.hpp"
#include "tracy_query/query.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include <TracyEvent.hpp>
#include <TracyWorker.hpp>

namespace tracy_query {
namespace {

std::string string_or_empty(const tracy::Worker& worker, const uint64_t reference) {
    if (reference == 0) return {};
    const auto* value = worker.GetString(reference);
    return value ? value : "";
}

std::string frame_set_name(const tracy::Worker& worker, const uint64_t reference) {
    if (reference == 0) return "default";
    if ((reference & 0x8000000000000000ull) != 0) {
        return "vsync:" + std::to_string(reference & 0x7FFFFFFFFFFFFFFFull);
    }
    return string_or_empty(worker, reference);
}

std::string trace_version(const int version) {
    return std::to_string(version >> 16) + "." + std::to_string((version >> 8) & 0xFF) +
           "." + std::to_string(version & 0xFF);
}

std::string cpu_architecture(const tracy::CpuArchitecture architecture) {
    switch (architecture) {
    case tracy::CpuArchX86: return "x86";
    case tracy::CpuArchX64: return "x86_64";
    case tracy::CpuArchArm32: return "arm32";
    case tracy::CpuArchArm64: return "arm64";
    default: return "unknown";
    }
}

void update_range(std::optional<int64_t>& first, std::optional<int64_t>& last,
                  const int64_t timestamp) {
    if (!first || timestamp < *first) first = timestamp;
    if (!last || timestamp > *last) last = timestamp;
}

void update_interval(std::optional<int64_t>& first, std::optional<int64_t>& last,
                     const int64_t begin, const int64_t end) {
    update_range(first, last, begin);
    update_range(first, last, end);
}

uint64_t count_gpu_zones(tracy::Worker& worker,
                         const tracy::Vector<tracy::short_ptr<tracy::GpuEvent>>& zones,
                         std::optional<int64_t>& first, std::optional<int64_t>& last) {
    uint64_t count = 0;
    const auto visit = [&](const tracy::GpuEvent& zone) {
        ++count;
        update_interval(first, last, zone.GpuStart(), worker.GetZoneEnd(zone));
        if (zone.Child() >= 0) count += count_gpu_zones(worker, worker.GetGpuChildren(zone.Child()), first, last);
    };
    if (zones.is_magic()) {
        const auto& direct = reinterpret_cast<const tracy::Vector<tracy::GpuEvent>&>(zones);
        for (const auto& zone : direct) visit(zone);
    } else {
        for (const auto& zone : zones) visit(*zone);
    }
    return count;
}

#ifndef TRACY_NO_STATISTICS
uint64_t count_ghost_zones(const tracy::Worker& worker, const tracy::Vector<tracy::GhostZone>& zones,
                           std::optional<int64_t>& first, std::optional<int64_t>& last) {
    uint64_t count = 0;
    for (const auto& zone : zones) {
        ++count;
        update_interval(first, last, zone.start.Val(), zone.end.Val());
        if (zone.child >= 0) count += count_ghost_zones(worker, worker.GetGhostChildren(zone.child), first, last);
    }
    return count;
}
#endif

bool selected_kind(const Options& options, const Source& source) {
    if (options.kinds.empty()) return true;
    for (const auto source_kind : source.kinds) {
        if (std::find(options.kinds.begin(), options.kinds.end(), source_kind) != options.kinds.end()) return true;
    }
    return false;
}

bool matches_any(const std::string& value, const std::vector<std::string>& patterns,
                 const bool ignore_case) {
    if (patterns.empty()) return true;
    const auto flags = std::regex::ECMAScript |
                       (ignore_case ? std::regex::icase : std::regex_constants::syntax_option_type{});
    for (const auto& pattern : patterns) {
        try {
            if (std::regex_search(value, std::regex{pattern, flags})) return true;
        } catch (const std::regex_error& error) {
            throw std::runtime_error{"invalid regular expression '" + pattern + "': " + error.what()};
        }
    }
    return false;
}

bool matches_either(const std::string& first, const std::string& second,
                    const std::vector<std::string>& patterns, const bool ignore_case) {
    return patterns.empty() || matches_any(first, patterns, ignore_case) ||
           matches_any(second, patterns, ignore_case);
}

bool source_selected(const Options& options, const Source& source) {
    if (!selected_kind(options, source)) return false;
    if (!options.source_ids.empty() &&
        std::find(options.source_ids.begin(), options.source_ids.end(), source.id) == options.source_ids.end()) return false;
    if (!matches_any(source.id, options.source_regexes, options.ignore_case)) return false;
    if (!options.source_types.empty() &&
        std::find(options.source_types.begin(), options.source_types.end(), source.type) == options.source_types.end()) return false;

    if (!options.threads.empty()) {
        if (source.type != "thread" && source.type != "gpu-thread") return false;
        if (!matches_either(source.id, source.name, options.threads, options.ignore_case)) return false;
    }
    if (!options.gpu_contexts.empty()) {
        if (source.type != "gpu-thread") return false;
        if (!matches_either(source.id, source.name, options.gpu_contexts, options.ignore_case)) return false;
    }
    if (!options.plots.empty() &&
        (source.type != "plot" || !matches_any(source.name, options.plots, options.ignore_case))) return false;
    if (!options.frame_sets.empty() &&
        (source.type != "frame-set" || !matches_any(source.name, options.frame_sets, options.ignore_case))) return false;
    if (!options.locks.empty() &&
        (source.type != "lock" || !matches_either(source.id, source.name, options.locks, options.ignore_case))) return false;
    if (!options.memory_pools.empty() &&
        (source.type != "memory-pool" || !matches_either(source.id, source.name, options.memory_pools, options.ignore_case))) return false;
    if (!options.cpus.empty()) {
        if (source.type != "cpu") return false;
        bool found = false;
        for (const auto cpu : options.cpus) if (source.id == "cpu:" + std::to_string(cpu)) found = true;
        if (!found) return false;
    }

    for (const auto& filter : options.filters) {
        if (filter.kind) {
            if (std::find(source.kinds.begin(), source.kinds.end(), *filter.kind) == source.kinds.end()) continue;
        }
        std::string value;
        if (filter.field == "name") value = source.name;
        else if (filter.field == "source") value = source.id;
        else if (filter.field == "source_type") value = source.type;
        else continue;
        if (!matches_any(value, {filter.pattern}, options.ignore_case)) return false;
    }
    return true;
}

std::vector<Source> discover_sources(Trace& trace) {
    auto& worker = trace.worker();
    std::vector<Source> sources;

    for (const auto* thread : worker.GetThreadData()) {
        Source source{trace.input().label, "thread:" + std::to_string(thread->id), "thread",
                      worker.GetThreadName(thread->id)};
        if (thread->count != 0) {
            source.kinds.push_back(Kind::CpuZone);
            source.counts.emplace_back("cpu_zone_count", thread->count);
            if (thread->timeline.is_magic()) {
                const auto& timeline = reinterpret_cast<const tracy::Vector<tracy::ZoneEvent>&>(thread->timeline);
                update_interval(source.first_timestamp_ns, source.last_timestamp_ns,
                                trace.normalize(timeline.front().Start()),
                                trace.normalize(worker.GetZoneEnd(timeline.back())));
            } else if (!thread->timeline.empty()) {
                update_interval(source.first_timestamp_ns, source.last_timestamp_ns,
                                trace.normalize(thread->timeline.front()->Start()),
                                trace.normalize(worker.GetZoneEnd(*thread->timeline.back())));
            }
        }
        if (!thread->messages.empty()) {
            source.kinds.push_back(Kind::Message);
            source.counts.emplace_back("message_count", static_cast<uint64_t>(thread->messages.size()));
            update_range(source.first_timestamp_ns, source.last_timestamp_ns,
                         trace.normalize(thread->messages.front()->time));
            update_range(source.first_timestamp_ns, source.last_timestamp_ns,
                         trace.normalize(thread->messages.back()->time));
        }
        const auto sample_count = thread->samples.size() + thread->ctxSwitchSamples.size();
        if (sample_count != 0) {
            source.kinds.push_back(Kind::Sample);
            source.counts.emplace_back("sample_count", static_cast<uint64_t>(sample_count));
            if (!thread->samples.empty()) {
                update_range(source.first_timestamp_ns, source.last_timestamp_ns,
                             trace.normalize(thread->samples.front().time.Val()));
                update_range(source.first_timestamp_ns, source.last_timestamp_ns,
                             trace.normalize(thread->samples.back().time.Val()));
            }
            if (!thread->ctxSwitchSamples.empty()) {
                update_range(source.first_timestamp_ns, source.last_timestamp_ns,
                             trace.normalize(thread->ctxSwitchSamples.front().time.Val()));
                update_range(source.first_timestamp_ns, source.last_timestamp_ns,
                             trace.normalize(thread->ctxSwitchSamples.back().time.Val()));
            }
        }
#ifndef TRACY_NO_STATISTICS
        if (!thread->ghostZones.empty()) {
            source.kinds.push_back(Kind::GhostZone);
            std::optional<int64_t> first, last;
            const auto count = count_ghost_zones(worker, thread->ghostZones, first, last);
            source.counts.emplace_back("ghost_zone_count", count);
            if (first) update_range(source.first_timestamp_ns, source.last_timestamp_ns, trace.normalize(*first));
            if (last) update_range(source.first_timestamp_ns, source.last_timestamp_ns, trace.normalize(*last));
        }
#endif
        if (const auto* context = worker.GetContextSwitchData(thread->id); context && !context->v.empty()) {
            source.kinds.push_back(Kind::ContextSwitch);
            source.counts.emplace_back("context_switch_count", static_cast<uint64_t>(context->v.size()));
            update_interval(source.first_timestamp_ns, source.last_timestamp_ns,
                            trace.normalize(context->v.front().Start()),
                            trace.normalize(context->v.back().End()));
        }
        if (!source.kinds.empty()) sources.push_back(std::move(source));
    }

    uint64_t context_index = 0;
    for (const auto* context : worker.GetGpuData()) {
        for (const auto& [thread_id, thread_data] : context->threadData) {
            const auto context_name = context->name.Active() ? std::string{worker.GetString(context->name)} : std::string{};
            const auto thread_name = std::string{worker.GetThreadName(thread_id)};
            Source source{trace.input().label,
                          "gpu-context:" + std::to_string(context_index) + "/thread:" + std::to_string(thread_id),
                          "gpu-thread", context_name.empty() ? thread_name : context_name + " / " + thread_name,
                          {Kind::GpuZone}};
            std::optional<int64_t> first, last;
            const auto count = count_gpu_zones(worker, thread_data.timeline, first, last);
            source.counts.emplace_back("gpu_zone_count", count);
            if (first) source.first_timestamp_ns = trace.normalize(*first);
            if (last) source.last_timestamp_ns = trace.normalize(*last);
            sources.push_back(std::move(source));
        }
        ++context_index;
    }

    uint64_t plot_index = 0;
    for (const auto* plot : worker.GetPlots()) {
        Source source{trace.input().label, "plot:" + std::to_string(plot_index++), "plot",
                      string_or_empty(worker, plot->name), {Kind::Plot}};
        source.counts.emplace_back("plot_count", static_cast<uint64_t>(plot->data.size()));
        if (!plot->data.empty()) {
            source.first_timestamp_ns = trace.normalize(plot->data.front().time.Val());
            source.last_timestamp_ns = trace.normalize(plot->data.back().time.Val());
        }
        sources.push_back(std::move(source));
    }

    uint64_t frame_index = 0;
    for (const auto* frames : worker.GetFrames()) {
        Source source{trace.input().label, "frame-set:" + std::to_string(frame_index++), "frame-set",
                      frame_set_name(worker, frames->name), {Kind::Frame}};
        uint64_t images = 0;
        source.counts.emplace_back("frame_count", static_cast<uint64_t>(frames->frames.size()));
        for (const auto& frame : frames->frames) if (frame.frameImage >= 0) ++images;
        if (images != 0) {
            source.kinds.push_back(Kind::FrameImage);
            source.counts.emplace_back("frame_image_count", images);
        }
        if (!frames->frames.empty()) {
            source.first_timestamp_ns = trace.normalize(worker.GetFrameBegin(*frames, 0));
            source.last_timestamp_ns = trace.normalize(worker.GetFrameEnd(*frames, frames->frames.size() - 1));
        }
        sources.push_back(std::move(source));
    }

    for (const auto& [lock_id, lock] : worker.GetLockMap()) {
        const auto lock_name = lock->customName.Active()
            ? std::string{worker.GetString(lock->customName)}
            : std::string{worker.GetZoneName(worker.GetSourceLocation(lock->srcloc))};
        Source source{trace.input().label, "lock:" + std::to_string(lock_id), "lock",
                      lock_name, {Kind::Lock}};
        source.counts.emplace_back("lock_count", static_cast<uint64_t>(lock->timeline.size()));
        if (!lock->timeline.empty()) {
            source.first_timestamp_ns = trace.normalize(lock->timeline.front().ptr->Time());
            source.last_timestamp_ns = trace.normalize(lock->timeline.back().ptr->Time());
        }
        sources.push_back(std::move(source));
    }

    uint64_t memory_index = 0;
    for (const auto& [name, memory] : worker.GetMemNameMap()) {
        Source source{trace.input().label, "memory-pool:" + std::to_string(memory_index++), "memory-pool",
                      name == 0 ? "default" : string_or_empty(worker, name), {Kind::Memory}};
        source.counts.emplace_back("allocation_count", static_cast<uint64_t>(memory->data.size()));
        source.counts.emplace_back("free_count", static_cast<uint64_t>(memory->frees.size()));
        for (const auto& event : memory->data) {
            update_range(source.first_timestamp_ns, source.last_timestamp_ns, trace.normalize(event.TimeAlloc()));
            if (event.TimeFree() >= 0) update_range(source.first_timestamp_ns, source.last_timestamp_ns, trace.normalize(event.TimeFree()));
        }
        sources.push_back(std::move(source));
    }

    for (int cpu = 0; cpu < worker.GetCpuDataCpuCount(); ++cpu) {
        const auto& slices = worker.GetCpuData()[cpu].cs;
        if (slices.empty()) continue;
        Source source{trace.input().label, "cpu:" + std::to_string(cpu), "cpu", std::to_string(cpu),
                      {Kind::CpuSlice}};
        source.counts.emplace_back("cpu_slice_count", static_cast<uint64_t>(slices.size()));
        source.first_timestamp_ns = trace.normalize(slices.front().Start());
        source.last_timestamp_ns = trace.normalize(slices.back().End());
        sources.push_back(std::move(source));
    }

    constexpr std::array counters{"cycles", "retired", "cache-ref", "cache-miss", "branch-retired", "branch-miss"};
    std::array<uint64_t, 6> counter_counts{};
    std::array<std::optional<int64_t>, 6> counter_first{}, counter_last{};
    for (const auto& [address, samples] : worker.GetHwSampleMapForQuery()) {
        static_cast<void>(address);
        const std::array<const tracy::SortedVector<tracy::Int48, tracy::Int48Sort>*, 6> vectors{
            &samples.cycles, &samples.retired, &samples.cacheRef, &samples.cacheMiss,
            &samples.branchRetired, &samples.branchMiss};
        for (size_t index = 0; index < vectors.size(); ++index) {
            counter_counts[index] += vectors[index]->size();
            if (!vectors[index]->empty()) {
                update_range(counter_first[index], counter_last[index], trace.normalize(vectors[index]->front().Val()));
                update_range(counter_first[index], counter_last[index], trace.normalize(vectors[index]->back().Val()));
            }
        }
    }
    for (size_t index = 0; index < counters.size(); ++index) {
        if (counter_counts[index] == 0) continue;
        Source source{trace.input().label, "hardware-counter:" + std::string{counters[index]},
                      "hardware-counter", counters[index], {Kind::HardwareSample}};
        source.counts.emplace_back("hardware_sample_count", counter_counts[index]);
        source.first_timestamp_ns = counter_first[index];
        source.last_timestamp_ns = counter_last[index];
        sources.push_back(std::move(source));
    }

    const auto& crash = worker.GetCrashEvent();
    if (crash.thread != 0) {
        Source source{trace.input().label, "capture", "capture", {}, {Kind::Crash}};
        source.counts.emplace_back("crash_count", uint64_t{1});
        source.first_timestamp_ns = source.last_timestamp_ns = trace.normalize(crash.time);
        sources.push_back(std::move(source));
    }

    return sources;
}

void run_range(const Options& options, const std::vector<Trace>& traces, std::ostream& output) {
    for (const auto& trace : traces) {
        emit_object(output, options.format,
                    {{"trace", trace.input().label},
                     {"first_timestamp_ns", int64_t{0}},
                     {"last_timestamp_ns", trace.normalize(trace.last_time())},
                     {"duration_ns", trace.normalize(trace.last_time())},
                     {"tracy_first_timestamp_ns", trace.first_time()},
                     {"tracy_last_timestamp_ns", trace.last_time()}});
    }
}

void run_info(const Options& options, const std::vector<Trace>& traces, std::ostream& output) {
    for (const auto& trace : traces) {
        const auto& worker = trace.worker();
        uint64_t frame_count = 0;
        for (const auto* frames : worker.GetFrames()) frame_count += frames->frames.size();
        uint64_t memory_count = 0;
        for (const auto& [name, memory] : worker.GetMemNameMap()) {
            static_cast<void>(name);
            memory_count += memory->data.size();
        }
        Fields fields{{"trace", trace.input().label},
                      {"trace_version", trace_version(worker.GetTraceVersion())},
                      {"capture_name", worker.GetCaptureName()},
                      {"capture_program", worker.GetCaptureProgram()},
                      {"capture_time", worker.GetCaptureTime()},
                      {"executable_time", worker.GetExecutableTime()},
                      {"host_info", worker.GetHostInfo()},
                      {"pid", worker.GetPid()},
                      {"timer_resolution_ns", worker.GetResolution()},
                      {"cpu_architecture", cpu_architecture(worker.GetCpuArch())},
                      {"cpu_id", static_cast<uint64_t>(worker.GetCpuId())},
                      {"cpu_manufacturer", std::string{worker.GetCpuManufacturer()}},
                      {"sampling_period_ns", worker.GetSamplingPeriod()},
                      {"first_timestamp_ns", int64_t{0}},
                      {"last_timestamp_ns", trace.normalize(trace.last_time())},
                      {"duration_ns", trace.normalize(trace.last_time())},
                      {"cpu_zone_count", worker.GetZoneCount()},
                      {"gpu_zone_count", worker.GetGpuZoneCount()},
                      {"message_count", static_cast<uint64_t>(worker.GetMessages().size())},
                      {"plot_point_count", worker.GetPlotCount() + worker.GetTracyPlotCount()},
                      {"lock_event_count", worker.GetLockCount()},
                      {"memory_allocation_count", memory_count},
                      {"frame_count", frame_count},
                      {"frame_image_count", static_cast<uint64_t>(worker.GetFrameImageCount())},
                      {"sample_count", worker.GetCallstackSampleCount()},
                      {"ghost_zone_count", worker.GetGhostZonesCount()},
                      {"context_switch_count", worker.GetContextSwitchCount()},
                      {"hardware_sample_count", worker.GetHwSampleCount()}};
        if (options.detail == Detail::Full) {
            fields.emplace_back("source_location_count", worker.GetSrcLocCount());
            fields.emplace_back("string_count", worker.GetStringsCount());
            fields.emplace_back("callstack_count", worker.GetCallstackPayloadCount());
            fields.emplace_back("callstack_frame_count", worker.GetCallstackFrameCount());
            fields.emplace_back("symbol_count", worker.GetSymbolsCount());
            fields.emplace_back("symbol_code_count", worker.GetSymbolCodeCount());
            fields.emplace_back("symbol_code_size", worker.GetSymbolCodeSize());
            fields.emplace_back("source_cache_count", worker.GetSourceFileCacheCount());
            fields.emplace_back("source_cache_size", worker.GetSourceFileCacheSize());
            fields.emplace_back("app_info_count", static_cast<uint64_t>(worker.GetAppInfo().size()));
            std::string app_info;
            for (const auto& item : worker.GetAppInfo()) {
                if (!app_info.empty()) app_info += "\n";
                app_info += worker.GetString(item);
            }
            if (!app_info.empty()) fields.emplace_back("app_info", std::move(app_info));
            fields.emplace_back("parameter_count", static_cast<uint64_t>(worker.GetParameters().size()));
            std::string parameters;
            for (const auto& parameter : worker.GetParameters()) {
                if (!parameters.empty()) parameters += ',';
                parameters += worker.GetString(parameter.name);
                parameters += '=';
                parameters += parameter.isBool ? (parameter.val ? "true" : "false") : std::to_string(parameter.val);
            }
            if (!parameters.empty()) fields.emplace_back("parameters", std::move(parameters));
            fields.emplace_back("cpu_topology_package_count", static_cast<uint64_t>(worker.GetCpuTopology().size()));
            uint64_t topology_thread_count = 0;
            for (const auto& [package, dies] : worker.GetCpuTopology()) {
                static_cast<void>(package);
                for (const auto& [die, cores] : dies) {
                    static_cast<void>(die);
                    for (const auto& [core, threads] : cores) {
                        static_cast<void>(core);
                        topology_thread_count += threads.size();
                    }
                }
            }
            fields.emplace_back("cpu_topology_thread_count", topology_thread_count);
        }
        emit_object(output, options.format, fields);
    }
}

}  // namespace

int run_command(const Options& options, std::vector<Trace>& traces,
                std::ostream& output, std::ostream& diagnostics) {
    switch (options.command) {
    case Command::Check:
        return 0;
    case Command::Range:
        run_range(options, traces, output);
        break;
    case Command::Info:
        run_info(options, traces, output);
        break;
    case Command::Sources: {
        uint64_t count = 0;
        for (auto& trace : traces) {
            for (const auto& source : discover_sources(trace)) {
                if (!source_selected(options, source)) continue;
                ++count;
                if (!options.count) emit_source(output, options.format, source);
            }
        }
        if (options.count) emit_object(output, options.format, {{"count", count}});
        break;
    }
    case Command::Query:
        run_query(options, traces, output, diagnostics);
        return 0;
    }
    verify_output(output);
    return 0;
}

}  // namespace tracy_query
