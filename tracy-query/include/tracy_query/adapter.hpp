#pragma once

#include <span>
#include <string_view>

#include "tracy_query/model.hpp"

namespace tracy_query {

struct AdapterDescriptor {
    Kind kind;
    std::string_view source_type;
    bool interval;
    bool nested;
    std::span<const std::string_view> filter_fields;
};

const AdapterDescriptor& adapter_for(Kind kind);
std::span<const AdapterDescriptor> adapter_registry();

}  // namespace tracy_query
