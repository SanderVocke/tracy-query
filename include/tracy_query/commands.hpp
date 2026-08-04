#pragma once

#include <iosfwd>
#include <vector>

#include "tracy_query/model.hpp"
#include "tracy_query/trace.hpp"

namespace tracy_query {

int run_command(const Options& options, std::vector<Trace>& traces,
                std::ostream& output, std::ostream& diagnostics);

}  // namespace tracy_query
