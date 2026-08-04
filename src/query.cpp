#include "tracy_query/query.hpp"

#include "tracy_query/adapter.hpp"
#include "tracy_query/output.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <map>
#include <queue>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include <TracyEvent.hpp>
#include <TracyWorker.hpp>

namespace tracy_query {
namespace {

using Visitor = std::function<void(Record&&)>;

thread_local bool expand_callstacks = false;
thread_local bool expand_symbols = false;

std::string safe(const char* value) { return value ? value : ""; }

std::string field_string(const Record& record, const std::string& name) {
    if (name == "trace") return record.trace;
    if (name == "source") return record.source;
    if (name == "kind") return std::string{kind_name(record.kind)};
    for (const auto& [field, value] : record.fields) {
        if (field != name) continue;
        return std::visit([](const auto& item) -> std::string {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) return {};
            else if constexpr (std::is_same_v<T, bool>) return item ? "true" : "false";
            else if constexpr (std::is_same_v<T, std::string>) return item;
            else return std::to_string(item);
        }, value);
    }
    if (name == "symbol" || name == "function" || name == "file") {
        for (const auto& [field, value] : record.fields) {
            if (field == "callstack" && std::holds_alternative<std::string>(value)) {
                return std::get<std::string>(value);
            }
        }
    }
    return {};
}

const Value* find_field(const Record& record, const std::string_view name) {
    for (const auto& [field, value] : record.fields) if (field == name) return &value;
    return nullptr;
}

std::string frame_set_name(const tracy::Worker& worker, const uint64_t reference) {
    if (reference == 0) return "default";
    if ((reference & 0x8000000000000000ull) != 0) {
        return "vsync:" + std::to_string(reference & 0x7FFFFFFFFFFFFFFFull);
    }
    return safe(worker.GetString(reference));
}

std::string plot_type(const tracy::PlotType type) {
    switch (type) {
    case tracy::PlotType::User: return "user";
    case tracy::PlotType::Memory: return "memory";
    case tracy::PlotType::SysTime: return "system-time";
    case tracy::PlotType::Power: return "power";
    }
    return "unknown";
}

std::string plot_format(const tracy::PlotValueFormatting format) {
    switch (format) {
    case tracy::PlotValueFormatting::Number: return "number";
    case tracy::PlotValueFormatting::Memory: return "memory";
    case tracy::PlotValueFormatting::Percentage: return "percentage";
    case tracy::PlotValueFormatting::Watt: return "watt";
    }
    return "unknown";
}

std::string lock_operation(const tracy::LockEvent::Type type) {
    switch (type) {
    case tracy::LockEvent::Type::Wait: return "wait";
    case tracy::LockEvent::Type::Obtain: return "obtain";
    case tracy::LockEvent::Type::Release: return "release";
    case tracy::LockEvent::Type::WaitShared: return "wait-shared";
    case tracy::LockEvent::Type::ObtainShared: return "obtain-shared";
    case tracy::LockEvent::Type::ReleaseShared: return "release-shared";
    }
    return "unknown";
}

std::string callstack_text(tracy::Worker& worker, const uint32_t index) {
    if (index == 0) return {};
    std::string result;
    try {
        const auto& callstack = worker.GetCallstack(index);
        for (const auto& frame_id : callstack) {
            const auto* frame_data = worker.GetCallstackFrame(frame_id);
            if (!frame_data) continue;
            for (uint8_t frame_index = 0; frame_index < frame_data->size; ++frame_index) {
                const auto& frame = frame_data->data.get()[frame_index];
                if (!result.empty()) result += " <- ";
                result += worker.GetString(frame.name);
            }
        }
    } catch (...) {
        return {};
    }
    return result;
}

void add_callstack(tracy::Worker& worker, Fields& fields, const uint32_t index) {
    if (index == 0) return;
    fields.emplace_back("callstack_id", static_cast<uint64_t>(index));
    if (expand_callstacks) fields.emplace_back("callstack", callstack_text(worker, index));
}

Fields source_location_fields(tracy::Worker& worker, const int16_t location_id) {
    Fields fields;
    const auto& location = worker.GetSourceLocation(location_id);
    fields.emplace_back("function", safe(worker.GetString(location.function)));
    fields.emplace_back("file", safe(worker.GetString(location.file)));
    fields.emplace_back("line", static_cast<uint64_t>(location.line));
    fields.emplace_back("color", static_cast<uint64_t>(location.color));
    return fields;
}

void append_fields(Fields& target, Fields source) {
    target.insert(target.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
}

void set_field(Fields& fields, std::string name, Value value) {
    for (auto& [field, current] : fields) {
        if (field == name) { current = std::move(value); return; }
    }
    fields.emplace_back(std::move(name), std::move(value));
}

void visit_messages(Trace& trace, const Visitor& visitor) {
    auto& worker = trace.worker();
    uint64_t sequence = 0;
    for (const auto& message_ptr : worker.GetMessages()) {
        const auto& message = *message_ptr;
        const auto thread = worker.DecompressThread(message.thread);
        Record record{trace.normalize(message.time), {}, trace.input().label,
                      "thread:" + std::to_string(thread), Kind::Message, sequence++};
        record.fields = {{"text", safe(worker.GetString(message.ref))},
                         {"thread", thread},
                         {"thread_name", safe(worker.GetThreadName(thread))},
                         {"color", static_cast<uint64_t>(message.color)}};
        add_callstack(worker, record.fields, message.callstack.Val());
        visitor(std::move(record));
    }
}

void visit_plots(Trace& trace, const Visitor& visitor) {
    auto& worker = trace.worker();
    uint64_t plot_index = 0;
    for (const auto* plot : worker.GetPlots()) {
        uint64_t sequence = 0;
        const auto name = safe(worker.GetString(plot->name));
        const auto source = "plot:" + std::to_string(plot_index++);
        for (const auto& item : plot->data) {
            Record record{trace.normalize(item.time.Val()), {}, trace.input().label, source,
                          Kind::Plot, sequence++};
            record.fields = {{"name", name}, {"plot_type", plot_type(plot->type)},
                             {"format", plot_format(plot->format)}, {"value", item.val}};
            visitor(std::move(record));
        }
    }
}

void visit_cpu_zone_vector(Trace& trace, tracy::Worker& worker,
                           const tracy::Vector<tracy::short_ptr<tracy::ZoneEvent>>& zones,
                           const uint64_t thread, const uint32_t depth,
                           const std::optional<uint64_t> parent_id,
                           const std::vector<std::string>& ancestors, uint64_t& next_id,
                           const Visitor& visitor) {
    const auto visit = [&](const tracy::ZoneEvent& zone) {
        const auto id = next_id++;
        const auto name = safe(worker.GetZoneName(zone));
        Record record{trace.normalize(zone.Start()), trace.normalize(worker.GetZoneEnd(zone)),
                      trace.input().label, "thread:" + std::to_string(thread), Kind::CpuZone, id};
        record.fields = {{"zone_id", id}, {"depth", static_cast<uint64_t>(depth)},
                         {"name", name}, {"thread", thread},
                         {"thread_name", safe(worker.GetThreadName(thread))}};
        if (parent_id) record.fields.emplace_back("parent_id", *parent_id);
        if (!ancestors.empty()) {
            std::string joined;
            for (const auto& ancestor : ancestors) { if (!joined.empty()) joined += " / "; joined += ancestor; }
            record.fields.emplace_back("ancestors", std::move(joined));
            record.fields.emplace_back("parent_name", ancestors.back());
            record.ancestor_names = ancestors;
        }
        append_fields(record.fields, source_location_fields(worker, zone.SrcLoc()));
        if (worker.HasZoneExtra(zone)) {
            const auto& extra = worker.GetZoneExtra(zone);
            if (extra.name.Active()) set_field(record.fields, "name", safe(worker.GetString(extra.name)));
            if (extra.text.Active()) record.fields.emplace_back("text", safe(worker.GetString(extra.text)));
            add_callstack(worker, record.fields, extra.callstack.Val());
        }
        visitor(std::move(record));
        if (zone.Child() >= 0) {
            auto next_ancestors = ancestors;
            next_ancestors.push_back(name);
            visit_cpu_zone_vector(trace, worker, worker.GetZoneChildren(zone.Child()), thread,
                                  depth + 1, id, next_ancestors, next_id, visitor);
        }
    };
    if (zones.is_magic()) {
        const auto& direct = reinterpret_cast<const tracy::Vector<tracy::ZoneEvent>&>(zones);
        for (const auto& zone : direct) visit(zone);
    } else {
        for (const auto& zone : zones) visit(*zone);
    }
}

void visit_cpu_zones(Trace& trace, const Visitor& visitor) {
    auto& worker = trace.worker();
    uint64_t next_id = 0;
    for (const auto* thread : worker.GetThreadData()) {
        visit_cpu_zone_vector(trace, worker, thread->timeline, thread->id, 0, std::nullopt,
                              {}, next_id, visitor);
    }
}

void visit_gpu_zone_vector(Trace& trace, tracy::Worker& worker, const tracy::GpuCtxData& context,
                           const uint64_t context_index, const uint64_t thread,
                           const tracy::Vector<tracy::short_ptr<tracy::GpuEvent>>& zones,
                           const uint32_t depth, const std::optional<uint64_t> parent_id,
                           const std::vector<std::string>& ancestors, uint64_t& next_id,
                           const Visitor& visitor) {
    const auto visit = [&](const tracy::GpuEvent& zone) {
        const auto id = next_id++;
        const auto name = safe(worker.GetZoneName(zone));
        Record record{trace.normalize(zone.GpuStart()), trace.normalize(worker.GetZoneEnd(zone)),
                      trace.input().label,
                      "gpu-context:" + std::to_string(context_index) + "/thread:" + std::to_string(thread),
                      Kind::GpuZone, id};
        record.fields = {{"zone_id", id}, {"depth", static_cast<uint64_t>(depth)},
                         {"name", name}, {"thread", thread},
                         {"thread_name", safe(worker.GetThreadName(thread))},
                         {"context", context_index},
                         {"context_name", context.name.Active() ? safe(worker.GetString(context.name)) : ""},
                         {"has_calibration", context.hasCalibration},
                         {"gpu_period", static_cast<double>(context.period)},
                         {"cpu_start_ns", trace.normalize(zone.CpuStart())},
                         {"cpu_end_ns", trace.normalize(zone.CpuEnd())}};
        if (parent_id) record.fields.emplace_back("parent_id", *parent_id);
        if (!ancestors.empty()) {
            std::string joined;
            for (const auto& ancestor : ancestors) { if (!joined.empty()) joined += " / "; joined += ancestor; }
            record.fields.emplace_back("ancestors", std::move(joined));
            record.fields.emplace_back("parent_name", ancestors.back());
            record.ancestor_names = ancestors;
        }
        append_fields(record.fields, source_location_fields(worker, zone.SrcLoc()));
        add_callstack(worker, record.fields, zone.callstack.Val());
        if (const auto notes = context.notes.find(zone.query_id); notes != context.notes.end()) {
            std::string text;
            for (const auto& [note_id, value] : notes->second) {
                if (!text.empty()) text += ',';
                const auto name_it = context.noteNames.find(note_id);
                text += name_it == context.noteNames.end() ? std::to_string(note_id) : safe(worker.GetString(name_it->second));
                text += '=' + std::to_string(value);
            }
            record.fields.emplace_back("note", std::move(text));
        }
        visitor(std::move(record));
        if (zone.Child() >= 0) {
            auto next_ancestors = ancestors;
            next_ancestors.push_back(name);
            visit_gpu_zone_vector(trace, worker, context, context_index, thread,
                                  worker.GetGpuChildren(zone.Child()), depth + 1, id,
                                  next_ancestors, next_id, visitor);
        }
    };
    if (zones.is_magic()) {
        const auto& direct = reinterpret_cast<const tracy::Vector<tracy::GpuEvent>&>(zones);
        for (const auto& zone : direct) visit(zone);
    } else {
        for (const auto& zone : zones) visit(*zone);
    }
}

void visit_gpu_zones(Trace& trace, const Visitor& visitor) {
    auto& worker = trace.worker();
    uint64_t context_index = 0, next_id = 0;
    for (const auto* context : worker.GetGpuData()) {
        for (const auto& [thread, data] : context->threadData) {
            visit_gpu_zone_vector(trace, worker, *context, context_index, thread, data.timeline,
                                  0, std::nullopt, {}, next_id, visitor);
        }
        ++context_index;
    }
}

void visit_frames(Trace& trace, const Visitor& visitor, const bool images_only) {
    auto& worker = trace.worker();
    uint64_t frame_set_index = 0;
    for (const auto* frame_set : worker.GetFrames()) {
        const auto source = "frame-set:" + std::to_string(frame_set_index++);
        const auto name = frame_set_name(worker, frame_set->name);
        for (size_t index = 0; index < frame_set->frames.size(); ++index) {
            const auto& frame = frame_set->frames[index];
            const auto begin = worker.GetFrameBegin(*frame_set, index);
            if (!images_only) {
                const auto end = worker.GetFrameEnd(*frame_set, index);
                Record record{trace.normalize(begin), trace.normalize(end), trace.input().label,
                              source, Kind::Frame, index};
                record.fields = {{"frame_set", name}, {"frame_index", static_cast<uint64_t>(index)},
                                 {"continuous", frame_set->continuous != 0},
                                 {"complete", frame_set->continuous != 0 || frame.end >= 0}};
                visitor(std::move(record));
            } else if (frame.frameImage >= 0) {
                const auto* image = worker.GetFrameImage(*frame_set, index);
                if (!image) continue;
                Record record{trace.normalize(begin), {}, trace.input().label, source,
                              Kind::FrameImage, index};
                record.fields = {{"frame_set", name}, {"frame_index", static_cast<uint64_t>(index)},
                                 {"width", static_cast<uint64_t>(image->w)},
                                 {"height", static_cast<uint64_t>(image->h)},
                                 {"flip", image->flip != 0},
                                 {"compressed_size", static_cast<uint64_t>(image->csz)}};
                visitor(std::move(record));
            }
        }
    }
}

void visit_locks(Trace& trace, const Visitor& visitor) {
    auto& worker = trace.worker();
    for (const auto& [lock_id, lock] : worker.GetLockMap()) {
        uint64_t sequence = 0;
        const auto custom_name = lock->customName.Active()
            ? safe(worker.GetString(lock->customName))
            : safe(worker.GetZoneName(worker.GetSourceLocation(lock->srcloc)));
        for (const auto& item : lock->timeline) {
            const auto& event = *item.ptr;
            const auto thread = event.thread < lock->threadList.size() ? lock->threadList[event.thread] : uint64_t{0};
            Record record{trace.normalize(event.Time()), {}, trace.input().label,
                          "lock:" + std::to_string(lock_id), Kind::Lock, sequence++};
            const auto owner = item.lockingThread < lock->threadList.size()
                ? lock->threadList[item.lockingThread] : uint64_t{0};
            std::string waiters;
            for (size_t bit = 0; bit < lock->threadList.size() && bit < 64; ++bit) {
                if ((item.waitList & (uint64_t{1} << bit)) == 0) continue;
                if (!waiters.empty()) waiters += ',';
                waiters += std::to_string(lock->threadList[bit]);
            }
            record.fields = {{"name", custom_name}, {"operation", lock_operation(event.type)},
                             {"thread", thread}, {"thread_name", safe(worker.GetThreadName(thread))},
                             {"owner_thread", owner}, {"lock_count", static_cast<uint64_t>(item.lockCount)},
                             {"waiters", std::move(waiters)}, {"contended", lock->isContended}};
            if (event.SrcLoc() != 0) {
                append_fields(record.fields, source_location_fields(worker, event.SrcLoc()));
            }
            visitor(std::move(record));
        }
    }
}

void visit_memory(Trace& trace, const Visitor& visitor) {
    auto& worker = trace.worker();
    uint64_t pool_index = 0;
    for (const auto& [name_ref, memory] : worker.GetMemNameMap()) {
        const auto source = "memory-pool:" + std::to_string(pool_index++);
        const auto pool = name_ref == 0 ? std::string{"default"} : safe(worker.GetString(name_ref));
        uint64_t sequence = 0;
        for (const auto& event : memory->data) {
            const auto alloc_thread = worker.DecompressThread(event.ThreadAlloc());
            Record allocation{trace.normalize(event.TimeAlloc()), {}, trace.input().label, source,
                              Kind::Memory, sequence++};
            allocation.fields = {{"pool", pool}, {"operation", std::string{"alloc"}},
                                 {"address", event.Ptr()}, {"size", event.Size()},
                                 {"live", event.TimeFree() < 0},
                                 {"thread", alloc_thread},
                                 {"thread_name", safe(worker.GetThreadName(alloc_thread))}};
            add_callstack(worker, allocation.fields, event.CsAlloc());
            visitor(std::move(allocation));
            if (event.TimeFree() >= 0) {
                const auto free_thread = worker.DecompressThread(event.ThreadFree());
                Record release{trace.normalize(event.TimeFree()), {}, trace.input().label, source,
                               Kind::Memory, sequence++};
                release.fields = {{"pool", pool}, {"operation", std::string{"free"}},
                                  {"address", event.Ptr()}, {"size", event.Size()},
                                  {"thread", free_thread},
                                  {"thread_name", safe(worker.GetThreadName(free_thread))}};
                add_callstack(worker, release.fields, event.csFree.Val());
                visitor(std::move(release));
            }
        }
    }
}

void visit_samples(Trace& trace, const Visitor& visitor) {
    auto& worker = trace.worker();
    for (const auto* thread : worker.GetThreadData()) {
        uint64_t sequence = 0;
        const auto emit = [&](const tracy::SampleData& sample, const bool context_switch) {
            Record record{trace.normalize(sample.time.Val()), {}, trace.input().label,
                          "thread:" + std::to_string(thread->id), Kind::Sample, sequence++};
            record.fields = {{"thread", thread->id}, {"thread_name", safe(worker.GetThreadName(thread->id))},
                             {"context_switch", context_switch}};
            add_callstack(worker, record.fields, sample.callstack.Val());
            visitor(std::move(record));
        };
        for (const auto& sample : thread->samples) emit(sample, false);
        for (const auto& sample : thread->ctxSwitchSamples) emit(sample, true);
    }
}

#ifndef TRACY_NO_STATISTICS
void visit_ghost_vector(Trace& trace, tracy::Worker& worker,
                        const tracy::Vector<tracy::GhostZone>& zones, const uint64_t thread,
                        const uint32_t depth, const std::optional<uint64_t> parent,
                        uint64_t& sequence, const Visitor& visitor) {
    for (const auto& zone : zones) {
        const auto id = sequence++;
        Record record{trace.normalize(zone.start.Val()), trace.normalize(zone.end.Val()),
                      trace.input().label, "thread:" + std::to_string(thread),
                      Kind::GhostZone, id};
        record.fields = {{"zone_id", id}, {"depth", static_cast<uint64_t>(depth)},
                         {"thread", thread}, {"thread_name", safe(worker.GetThreadName(thread))},
                         {"frame_id", static_cast<uint64_t>(zone.frame.Val())}, {"derived", true}};
        const auto& ghost_key = worker.GetGhostFrame(zone.frame);
        if (const auto* frame_data = worker.GetCallstackFrame(ghost_key.frame);
            frame_data && ghost_key.inlineFrame < frame_data->size) {
            const auto& frame = frame_data->data.get()[ghost_key.inlineFrame];
            record.fields.emplace_back("symbol", safe(worker.GetString(frame.name)));
            record.fields.emplace_back("file", safe(worker.GetString(frame.file)));
            record.fields.emplace_back("line", static_cast<uint64_t>(frame.line));
        }
        if (parent) record.fields.emplace_back("parent_id", *parent);
        visitor(std::move(record));
        if (zone.child >= 0) {
            visit_ghost_vector(trace, worker, worker.GetGhostChildren(zone.child), thread,
                               depth + 1, id, sequence, visitor);
        }
    }
}
#endif

void visit_ghost_zones(Trace& trace, const Visitor& visitor) {
#ifndef TRACY_NO_STATISTICS
    auto& worker = trace.worker();
    for (const auto* thread : worker.GetThreadData()) {
        uint64_t sequence = 0;
        visit_ghost_vector(trace, worker, thread->ghostZones, thread->id, 0,
                           std::nullopt, sequence, visitor);
    }
#else
    static_cast<void>(trace); static_cast<void>(visitor);
#endif
}

void visit_context_switches(Trace& trace, const Visitor& visitor) {
    auto& worker = trace.worker();
    for (const auto& [thread, stats] : worker.GetCpuThreadData()) {
        static_cast<void>(stats);
        const auto* context = worker.GetContextSwitchData(thread);
        if (!context) continue;
        uint64_t sequence = 0;
        for (const auto& event : context->v) {
            const auto end = event.IsEndValid() ? event.End() : event.Start();
            Record record{trace.normalize(event.Start()), trace.normalize(end), trace.input().label,
                          "thread:" + std::to_string(thread), Kind::ContextSwitch, sequence++};
            record.fields = {{"thread", thread}, {"thread_name", safe(worker.GetThreadName(thread))},
                             {"process", worker.GetPidFromTid(thread)},
                             {"cpu", static_cast<uint64_t>(event.Cpu())},
                             {"wakeup_cpu", static_cast<uint64_t>(event.WakeupCpu())},
                             {"reason", static_cast<int64_t>(event.Reason())},
                             {"state", static_cast<int64_t>(event.State())},
                             {"complete", event.IsEndValid()}};
            if (event.WakeupVal() >= 0) {
                record.fields.emplace_back("wakeup_timestamp_ns", trace.normalize(event.WakeupVal()));
            }
            visitor(std::move(record));
        }
    }
}

void visit_cpu_slices(Trace& trace, const Visitor& visitor) {
    auto& worker = trace.worker();
    for (int cpu = 0; cpu < worker.GetCpuDataCpuCount(); ++cpu) {
        uint64_t sequence = 0;
        for (const auto& event : worker.GetCpuData()[cpu].cs) {
            const auto thread = worker.DecompressThreadExternal(event.Thread());
            const auto end = event.IsEndValid() ? event.End() : event.Start();
            Record record{trace.normalize(event.Start()), trace.normalize(end), trace.input().label,
                          "cpu:" + std::to_string(cpu), Kind::CpuSlice, sequence++};
            record.fields = {{"cpu", static_cast<uint64_t>(cpu)}, {"thread", thread},
                             {"thread_name", safe(worker.GetThreadName(thread))},
                             {"process", worker.GetPidFromTid(thread)}, {"complete", event.IsEndValid()}};
            visitor(std::move(record));
        }
    }
}

void visit_cpu_usage(Trace& trace, const Visitor& visitor) {
#ifndef TRACY_NO_STATISTICS
    uint64_t sequence = 0;
    for (const auto& usage : trace.worker().GetCpuUsage()) {
        Record record{trace.normalize(usage.Time()), {}, trace.input().label, "capture",
                      Kind::CpuUsage, sequence++};
        record.fields = {{"own", static_cast<uint64_t>(usage.Own())},
                         {"other", static_cast<uint64_t>(usage.Other())}, {"derived", true}};
        visitor(std::move(record));
    }
#else
    static_cast<void>(trace); static_cast<void>(visitor);
#endif
}

std::string symbol_for(tracy::Worker& worker, const uint64_t address) {
    uint32_t offset = 0;
    const auto symbol_address = worker.GetSymbolForAddress(address, offset);
    const auto* symbol = worker.GetSymbolData(symbol_address);
    return symbol ? safe(worker.GetString(symbol->name)) : std::string{};
}

void visit_hardware_samples(Trace& trace, const Visitor& visitor) {
    auto& worker = trace.worker();
    struct Counter { const char* name; const tracy::SortedVector<tracy::Int48, tracy::Int48Sort> tracy::HwSampleData::*member; };
    constexpr std::array counters{
        Counter{"cycles", &tracy::HwSampleData::cycles},
        Counter{"retired", &tracy::HwSampleData::retired},
        Counter{"cache-ref", &tracy::HwSampleData::cacheRef},
        Counter{"cache-miss", &tracy::HwSampleData::cacheMiss},
        Counter{"branch-retired", &tracy::HwSampleData::branchRetired},
        Counter{"branch-miss", &tracy::HwSampleData::branchMiss},
    };
    for (const auto& [address, samples] : worker.GetHwSampleMapForQuery()) {
        for (const auto& counter : counters) {
            uint64_t sequence = 0;
            for (const auto& timestamp : samples.*(counter.member)) {
                Record record{trace.normalize(timestamp.Val()), {}, trace.input().label,
                              "hardware-counter:" + std::string{counter.name}, Kind::HardwareSample,
                              sequence++};
                record.fields = {{"counter", std::string{counter.name}}, {"address", address}};
                if (expand_symbols) record.fields.emplace_back("symbol", symbol_for(worker, address));
                visitor(std::move(record));
            }
        }
    }
}

void visit_crash(Trace& trace, const Visitor& visitor) {
    auto& worker = trace.worker();
    const auto& crash = worker.GetCrashEvent();
    if (crash.thread == 0) return;
    Record record{trace.normalize(crash.time), {}, trace.input().label, "capture", Kind::Crash, 0};
    record.fields = {{"thread", crash.thread}, {"thread_name", safe(worker.GetThreadName(crash.thread))},
                     {"message", safe(worker.GetString(crash.message))}};
    add_callstack(worker, record.fields, crash.callstack);
    visitor(std::move(record));
}

void visit_kind(Trace& trace, const Kind kind, const Visitor& visitor) {
    switch (kind) {
    case Kind::Message: visit_messages(trace, visitor); break;
    case Kind::Plot: visit_plots(trace, visitor); break;
    case Kind::CpuZone: visit_cpu_zones(trace, visitor); break;
    case Kind::GpuZone: visit_gpu_zones(trace, visitor); break;
    case Kind::Frame: visit_frames(trace, visitor, false); break;
    case Kind::FrameImage: visit_frames(trace, visitor, true); break;
    case Kind::Lock: visit_locks(trace, visitor); break;
    case Kind::Memory: visit_memory(trace, visitor); break;
    case Kind::Sample: visit_samples(trace, visitor); break;
    case Kind::GhostZone: visit_ghost_zones(trace, visitor); break;
    case Kind::ContextSwitch: visit_context_switches(trace, visitor); break;
    case Kind::CpuSlice: visit_cpu_slices(trace, visitor); break;
    case Kind::CpuUsage: visit_cpu_usage(trace, visitor); break;
    case Kind::HardwareSample: visit_hardware_samples(trace, visitor); break;
    case Kind::Crash: visit_crash(trace, visitor); break;
    }
}

class Matcher {
public:
    explicit Matcher(const Options& options) : options_{options} {
        const auto flags = std::regex::ECMAScript |
            (options.ignore_case ? std::regex::icase : std::regex_constants::syntax_option_type{});
        auto compile_all = [&](const std::vector<std::string>& patterns, std::vector<std::regex>& target) {
            for (const auto& pattern : patterns) {
                try { target.emplace_back(pattern, flags); }
                catch (const std::regex_error& error) { throw std::runtime_error{"invalid regular expression '" + pattern + "': " + error.what()}; }
            }
        };
        compile_all(options.source_regexes, source_regexes_);
        compile_all(options.threads, threads_);
        compile_all(options.gpu_contexts, gpu_contexts_);
        compile_all(options.plots, plots_);
        compile_all(options.frame_sets, frame_sets_);
        compile_all(options.locks, locks_);
        compile_all(options.memory_pools, memory_pools_);
        if (options.zone_parent) zone_parent_.emplace(*options.zone_parent, flags);
        if (options.zone_ancestor) zone_ancestor_.emplace(*options.zone_ancestor, flags);
        if (options.stack_frame) stack_frame_.emplace(*options.stack_frame, flags);
        for (const auto& filter : options.filters) {
            try { filters_.push_back({filter.kind, filter.field, std::regex{filter.pattern, flags}}); }
            catch (const std::regex_error& error) { throw std::runtime_error{"invalid regular expression '" + filter.pattern + "': " + error.what()}; }
        }
        validate_fields();
        const bool zone_structure = options.root_zones || options.zone_depth || options.zone_parent || options.zone_ancestor;
        if (zone_structure) {
            for (const auto kind : options.kinds) {
                if (kind != Kind::CpuZone && kind != Kind::GpuZone && kind != Kind::GhostZone) {
                    throw std::runtime_error{"zone structure options require zone kinds"};
                }
                if ((options.zone_parent || options.zone_ancestor) && kind == Kind::GhostZone) {
                    throw std::runtime_error{"parent-name and ancestor-name filters are not available for ghost zones"};
                }
            }
        }
        if (options.time.active) {
            for (const auto kind : options.kinds) {
                if (!adapter_for(kind).interval) {
                    throw std::runtime_error{"--active is not valid for point kind " + std::string{kind_name(kind)}};
                }
            }
        }
    }

    bool matches(const Record& record) const {
        if (!options_.source_ids.empty() && std::find(options_.source_ids.begin(), options_.source_ids.end(), record.source) == options_.source_ids.end()) return false;
        if (!matches_any(record.source, source_regexes_)) return false;
        if (!options_.source_types.empty() &&
            std::find(options_.source_types.begin(), options_.source_types.end(),
                      adapter_for(record.kind).source_type) == options_.source_types.end()) return false;
        if (!matches_either(field_string(record, "thread"), field_string(record, "thread_name"), threads_)) return false;
        if (!matches_either(field_string(record, "context"), field_string(record, "context_name"), gpu_contexts_)) return false;
        if (!plots_.empty() &&
            (record.kind != Kind::Plot || !matches_any(field_string(record, "name"), plots_))) return false;
        if (!matches_any(field_string(record, "frame_set"), frame_sets_)) return false;
        if (!locks_.empty() &&
            (record.kind != Kind::Lock || !matches_either(record.source, field_string(record, "name"), locks_))) return false;
        if (!memory_pools_.empty() &&
            (record.kind != Kind::Memory || !matches_either(record.source, field_string(record, "pool"), memory_pools_))) return false;
        if (!options_.cpus.empty()) {
            const auto cpu = field_string(record, "cpu");
            bool found = false; for (const auto value : options_.cpus) if (cpu == std::to_string(value)) found = true;
            if (!found) return false;
        }
        if (options_.root_zones && find_field(record, "depth") && field_string(record, "depth") != "0") return false;
        if (options_.zone_depth) {
            const auto* value = find_field(record, "depth");
            if (!value) return false;
            const auto depth = std::get<uint64_t>(*value);
            if (depth < options_.zone_depth->first || depth > options_.zone_depth->second) return false;
        }
        if (zone_parent_ && !std::regex_search(field_string(record, "parent_name"), *zone_parent_)) return false;
        if (zone_ancestor_ && !std::any_of(record.ancestor_names.begin(), record.ancestor_names.end(),
                                           [&](const auto& name) { return std::regex_search(name, *zone_ancestor_); })) return false;
        if (stack_frame_ && !std::regex_search(field_string(record, "callstack"), *stack_frame_)) return false;
        for (const auto& filter : filters_) {
            if (filter.kind && *filter.kind != record.kind) continue;
            if (!std::regex_search(field_string(record, filter.field), filter.regex)) return false;
        }
        return true;
    }

private:
    struct Filter { std::optional<Kind> kind; std::string field; std::regex regex; };

    static bool matches_any(const std::string& value, const std::vector<std::regex>& patterns) {
        if (patterns.empty()) return true;
        return std::any_of(patterns.begin(), patterns.end(), [&](const auto& pattern) { return std::regex_search(value, pattern); });
    }

    static bool matches_either(const std::string& first, const std::string& second,
                               const std::vector<std::regex>& patterns) {
        return patterns.empty() || matches_any(first, patterns) || matches_any(second, patterns);
    }

    void validate_fields() const {
        for (const auto& filter : options_.filters) {
            const auto validates = [&](const Kind kind) {
                const auto fields = adapter_for(kind).filter_fields;
                return std::find(fields.begin(), fields.end(), filter.field) != fields.end();
            };
            if (filter.kind) {
                if (!validates(*filter.kind)) throw std::runtime_error{"field '" + filter.field + "' is not valid for " + std::string{kind_name(*filter.kind)}};
            } else {
                for (const auto kind : options_.kinds) {
                    if (!validates(kind)) throw std::runtime_error{"unscoped field '" + filter.field + "' is not valid for " + std::string{kind_name(kind)}};
                }
            }
        }
    }

    const Options& options_;
    std::vector<std::regex> source_regexes_, threads_, gpu_contexts_, plots_, frame_sets_, locks_, memory_pools_;
    std::optional<std::regex> zone_parent_, zone_ancestor_, stack_frame_;
    std::vector<Filter> filters_;
};

bool temporal_range_match(const Record& record, const Options& options, const int64_t trace_end) {
    const bool explicit_range = options.time.from_ns || options.time.to_ns ||
                                options.time.from_start || options.time.to_end;
    if (!explicit_range) return true;
    const auto from = options.time.from_ns.value_or(0);
    const auto to = options.time.to_ns.value_or(trace_end);
    const auto end = record.end_timestamp_ns.value_or(record.timestamp_ns);
    switch (options.time.range_match) {
    case RangeMatch::Overlap: return record.timestamp_ns <= to && end >= from;
    case RangeMatch::Start: return record.timestamp_ns >= from && record.timestamp_ns <= to;
    case RangeMatch::Contained: return record.timestamp_ns >= from && end <= to;
    }
    return false;
}

std::string identity(const Record& record) {
    return record.trace + '\n' + record.source + '\n' + std::string{kind_name(record.kind)} + '\n' + std::to_string(record.sequence);
}

size_t trace_order(const Options& options, const std::string& trace) {
    const auto found = std::find_if(options.traces.begin(), options.traces.end(),
                                    [&](const auto& item) { return item.label == trace; });
    return static_cast<size_t>(found - options.traces.begin());
}

bool record_less(const Options& options, const Record& left, const Record& right) {
    if (left.timestamp_ns != right.timestamp_ns) return left.timestamp_ns < right.timestamp_ns;
    const auto left_trace = trace_order(options, left.trace);
    const auto right_trace = trace_order(options, right.trace);
    if (left_trace != right_trace) return left_trace < right_trace;
    if (left.kind != right.kind) return static_cast<int>(left.kind) < static_cast<int>(right.kind);
    if (left.source != right.source) return left.source < right.source;
    return left.sequence < right.sequence;
}

class ExternalSorter {
public:
    ExternalSorter(const Options& options, std::ostream& output)
        : options_{options}, output_{output} {}

    ~ExternalSorter() {
        for (auto* file : runs_) if (file) std::fclose(file);
    }

    void add(Record&& record) {
        ++record_count_;
        chunk_.push_back(std::move(record));
        if (chunk_.size() >= 8192) flush_chunk();
    }

    bool finish() {
        flush_chunk();
        struct Node {
            int64_t timestamp;
            uint32_t trace;
            uint8_t kind;
            uint64_t sequence;
            std::string source;
            std::string line;
            size_t run;
        };
        const auto later = [](const Node& left, const Node& right) {
            if (left.timestamp != right.timestamp) return left.timestamp > right.timestamp;
            if (left.trace != right.trace) return left.trace > right.trace;
            if (left.kind != right.kind) return left.kind > right.kind;
            if (left.source != right.source) return left.source > right.source;
            return left.sequence > right.sequence;
        };
        std::priority_queue<Node, std::vector<Node>, decltype(later)> heap{later};
        for (size_t index = 0; index < runs_.size(); ++index) {
            std::rewind(runs_[index]);
            Node node; node.run = index;
            if (read_node(runs_[index], node)) heap.push(std::move(node));
        }
        uint64_t emitted = 0;
        while (!heap.empty() && (!options_.limit || emitted < *options_.limit)) {
            auto node = heap.top();
            heap.pop();
            output_ << node.line;
            if (!output_) throw OutputError{"failed to write output"};
            ++emitted;
            Node next; next.run = node.run;
            if (read_node(runs_[node.run], next)) heap.push(std::move(next));
        }
        verify_output(output_);
        return emitted < record_count_;
    }

private:
    template<typename T>
    static void write_value(std::FILE* file, const T& value) {
        if (std::fwrite(&value, sizeof(value), 1, file) != 1) throw OutputError{"failed to write query sort run"};
    }

    template<typename T>
    static bool read_value(std::FILE* file, T& value, const bool allow_eof = false) {
        if (std::fread(&value, sizeof(value), 1, file) == 1) return true;
        if (allow_eof && std::feof(file)) return false;
        throw OutputError{"failed to read query sort run"};
    }

    void flush_chunk() {
        if (chunk_.empty()) return;
        std::sort(chunk_.begin(), chunk_.end(), [&](const auto& left, const auto& right) {
            return record_less(options_, left, right);
        });
        auto* file = std::tmpfile();
        if (!file) throw OutputError{"cannot create temporary query sort file"};
        runs_.push_back(file);
        for (const auto& record : chunk_) {
            std::ostringstream formatted;
            emit_record(formatted, options_.format, record);
            const auto line = formatted.str();
            const auto trace = static_cast<uint32_t>(trace_order(options_, record.trace));
            const auto kind = static_cast<uint8_t>(record.kind);
            const auto source_size = static_cast<uint32_t>(record.source.size());
            const auto line_size = static_cast<uint32_t>(line.size());
            write_value(file, record.timestamp_ns);
            write_value(file, trace);
            write_value(file, kind);
            write_value(file, record.sequence);
            write_value(file, source_size);
            write_value(file, line_size);
            if (std::fwrite(record.source.data(), 1, source_size, file) != source_size ||
                std::fwrite(line.data(), 1, line_size, file) != line_size) {
                throw OutputError{"failed to write query sort run"};
            }
        }
        chunk_.clear();
    }

    template<typename Node>
    static bool read_node(std::FILE* file, Node& node) {
        if (!read_value(file, node.timestamp, true)) return false;
        uint32_t source_size = 0, line_size = 0;
        read_value(file, node.trace);
        read_value(file, node.kind);
        read_value(file, node.sequence);
        read_value(file, source_size);
        read_value(file, line_size);
        node.source.resize(source_size);
        node.line.resize(line_size);
        if (std::fread(node.source.data(), 1, source_size, file) != source_size ||
            std::fread(node.line.data(), 1, line_size, file) != line_size) {
            throw OutputError{"failed to read query sort run"};
        }
        return true;
    }

    const Options& options_;
    std::ostream& output_;
    std::vector<Record> chunk_;
    std::vector<std::FILE*> runs_;
    uint64_t record_count_ = 0;
};

}  // namespace

void run_query(const Options& options, std::vector<Trace>& traces,
               std::ostream& output, std::ostream& diagnostics) {
    expand_callstacks = options.detail == Detail::Full || options.stack_frame.has_value();
    expand_symbols = options.detail == Detail::Full;
    for (const auto& filter : options.filters) {
        if (filter.field == "callstack" || filter.field == "symbol" || filter.field == "function" || filter.field == "file") {
            expand_callstacks = true;
            if (filter.field == "symbol" || filter.field == "function" || filter.field == "file") expand_symbols = true;
        }
    }
    Matcher matcher{options};
    std::vector<Record> records;
    ExternalSorter sorter{options, output};
    std::unordered_map<std::string, Record> latest;
    std::unordered_map<std::string, Record> next;
    std::set<std::string> point_ids;
    uint64_t streaming_count = 0;
    std::map<std::string, std::pair<Fields, uint64_t>> streaming_groups;

    const auto count_record = [&](const Record& record) {
        ++streaming_count;
        if (options.group_by.empty()) return;
        std::string key;
        Fields fields;
        for (const auto& group : options.group_by) {
            std::string value;
            if (group == "trace") value = record.trace;
            else if (group == "source") value = record.source;
            else value = kind_name(record.kind);
            key += value + '\n';
            fields.emplace_back(group, value);
        }
        auto& entry = streaming_groups[key];
        if (entry.second == 0) entry.first = std::move(fields);
        ++entry.second;
    };

    for (auto& trace : traces) {
        for (const auto kind : options.kinds) {
            visit_kind(trace, kind, [&](Record&& record) {
                if (!matcher.matches(record)) return;
                if (options.time.at_ns) {
                    const auto key = record.trace + '\n' + record.source + '\n' + std::string{kind_name(record.kind)};
                    if (options.time.active && record.end_timestamp_ns &&
                        record.timestamp_ns <= *options.time.at_ns && *record.end_timestamp_ns >= *options.time.at_ns) {
                        const auto id = identity(record); if (point_ids.insert(id).second) records.push_back(record);
                    }
                    if (options.time.latest && record.timestamp_ns <= *options.time.at_ns) {
                        const auto found = latest.find(key);
                        if (found == latest.end() || found->second.timestamp_ns < record.timestamp_ns) latest.insert_or_assign(key, record);
                    }
                    if (options.time.next && record.timestamp_ns >= *options.time.at_ns) {
                        const auto found = next.find(key);
                        if (found == next.end() || found->second.timestamp_ns > record.timestamp_ns) next.insert_or_assign(key, record);
                    }
                } else if (temporal_range_match(record, options, trace.normalize(trace.last_time()))) {
                    if (options.count) count_record(record);
                    else sorter.add(std::move(record));
                }
            });
        }
    }

    if (!options.time.at_ns && !options.count) {
        const auto truncated = sorter.finish();
        if (truncated && !options.quiet) diagnostics << "tracy-query: output truncated by --limit\n";
        return;
    }

    if (options.count && !options.time.at_ns) {
        if (options.group_by.empty()) {
            emit_object(output, options.format, {{"count", streaming_count}});
        } else {
            for (auto& [key, group] : streaming_groups) {
                static_cast<void>(key);
                group.first.emplace_back("count", group.second);
                emit_object(output, options.format, group.first);
            }
        }
        verify_output(output);
        return;
    }

    for (auto& [key, record] : latest) {
        static_cast<void>(key);
        if (point_ids.insert(identity(record)).second) records.push_back(std::move(record));
    }
    for (auto& [key, record] : next) {
        static_cast<void>(key);
        if (point_ids.insert(identity(record)).second) records.push_back(std::move(record));
    }

    std::sort(records.begin(), records.end(), [&](const Record& left, const Record& right) {
        return record_less(options, left, right);
    });

    if (options.count) {
        if (options.group_by.empty()) {
            emit_object(output, options.format, {{"count", static_cast<uint64_t>(records.size())}});
        } else {
            std::map<std::string, std::pair<Fields, uint64_t>> groups;
            for (const auto& record : records) {
                std::string key;
                Fields fields;
                for (const auto& group : options.group_by) {
                    std::string value;
                    if (group == "trace") value = record.trace;
                    else if (group == "source") value = record.source;
                    else value = kind_name(record.kind);
                    key += value + '\n'; fields.emplace_back(group, value);
                }
                auto& entry = groups[key];
                if (entry.second == 0) entry.first = std::move(fields);
                ++entry.second;
            }
            for (auto& [key, group] : groups) {
                static_cast<void>(key); group.first.emplace_back("count", group.second);
                emit_object(output, options.format, group.first);
            }
        }
    } else {
        uint64_t emitted = 0;
        for (const auto& record : records) {
            if (options.limit && emitted >= *options.limit) break;
            emit_record(output, options.format, record); ++emitted;
        }
        if (emitted < records.size() && !options.quiet) {
            diagnostics << "tracy-query: output truncated by --limit\n";
        }
    }
    verify_output(output);
}

}  // namespace tracy_query
