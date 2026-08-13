#include "tracy_query/adapter.hpp"

#include <array>
#include <stdexcept>

namespace tracy_query {
namespace {

using Fields = std::span<const std::string_view>;

constexpr std::array<std::string_view, 6> message_fields{"trace", "source", "kind", "text", "thread", "color"};
constexpr std::array<std::string_view, 6> plot_fields{"trace", "source", "kind", "name", "plot_type", "format"};
constexpr std::array<std::string_view, 9> cpu_zone_fields{"trace", "source", "kind", "name", "text", "function", "file", "thread", "color"};
constexpr std::array<std::string_view, 9> gpu_zone_fields{"trace", "source", "kind", "name", "function", "file", "thread", "context", "note"};
constexpr std::array<std::string_view, 4> frame_fields{"trace", "source", "kind", "frame_set"};
constexpr std::array<std::string_view, 8> lock_fields{"trace", "source", "kind", "name", "operation", "thread", "function", "file"};
constexpr std::array<std::string_view, 7> memory_fields{"trace", "source", "kind", "pool", "operation", "thread", "symbol"};
constexpr std::array<std::string_view, 8> sample_fields{"trace", "source", "kind", "thread", "symbol", "function", "file", "counter"};
constexpr std::array<std::string_view, 9> scheduler_fields{"trace", "source", "kind", "thread", "process", "cpu", "reason", "state", "derived"};
constexpr std::array<std::string_view, 6> crash_fields{"trace", "source", "kind", "message", "thread", "symbol"};

constexpr std::array<AdapterDescriptor, 15> adapters{{
    {Kind::Message, "thread", false, false, Fields{message_fields}},
    {Kind::Plot, "plot", false, false, Fields{plot_fields}},
    {Kind::CpuZone, "thread", true, true, Fields{cpu_zone_fields}},
    {Kind::GpuZone, "gpu-thread", true, true, Fields{gpu_zone_fields}},
    {Kind::Frame, "frame-set", true, false, Fields{frame_fields}},
    {Kind::FrameImage, "frame-set", false, false, Fields{frame_fields}},
    {Kind::Lock, "lock", false, false, Fields{lock_fields}},
    {Kind::Memory, "memory-pool", false, false, Fields{memory_fields}},
    {Kind::Sample, "thread", false, false, Fields{sample_fields}},
    {Kind::GhostZone, "thread", true, true, Fields{sample_fields}},
    {Kind::ContextSwitch, "thread", true, false, Fields{scheduler_fields}},
    {Kind::CpuSlice, "cpu", true, false, Fields{scheduler_fields}},
    {Kind::CpuUsage, "capture", false, false, Fields{scheduler_fields}},
    {Kind::HardwareSample, "hardware-counter", false, false, Fields{sample_fields}},
    {Kind::Crash, "capture", false, false, Fields{crash_fields}},
}};

}  // namespace

const AdapterDescriptor& adapter_for(const Kind kind) {
    for (const auto& adapter : adapters) {
        if (adapter.kind == kind) return adapter;
    }
    throw std::logic_error{"unknown adapter kind"};
}

std::span<const AdapterDescriptor> adapter_registry() { return adapters; }

}  // namespace tracy_query
