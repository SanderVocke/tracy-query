#include "tracy_query/model.hpp"

#include <array>

namespace tracy_query {
namespace {

constexpr std::array kKinds{
    Kind::Message,       Kind::Plot,         Kind::CpuZone,
    Kind::GpuZone,       Kind::Frame,        Kind::FrameImage,
    Kind::Lock,          Kind::Memory,       Kind::Sample,
    Kind::GhostZone,     Kind::ContextSwitch, Kind::CpuSlice,
    Kind::CpuUsage,      Kind::HardwareSample, Kind::Crash,
};

}  // namespace

std::string_view kind_name(const Kind kind) {
    switch (kind) {
    case Kind::Message: return "message";
    case Kind::Plot: return "plot";
    case Kind::CpuZone: return "cpu-zone";
    case Kind::GpuZone: return "gpu-zone";
    case Kind::Frame: return "frame";
    case Kind::FrameImage: return "frame-image";
    case Kind::Lock: return "lock";
    case Kind::Memory: return "memory";
    case Kind::Sample: return "sample";
    case Kind::GhostZone: return "ghost-zone";
    case Kind::ContextSwitch: return "context-switch";
    case Kind::CpuSlice: return "cpu-slice";
    case Kind::CpuUsage: return "cpu-usage";
    case Kind::HardwareSample: return "hardware-sample";
    case Kind::Crash: return "crash";
    }
    return "unknown";
}

std::optional<Kind> parse_kind(const std::string_view text) {
    for (const auto kind : kKinds) {
        if (kind_name(kind) == text) return kind;
    }
    return std::nullopt;
}

const std::vector<Kind>& all_kinds() {
    static const std::vector<Kind> kinds{kKinds.begin(), kKinds.end()};
    return kinds;
}

}  // namespace tracy_query
