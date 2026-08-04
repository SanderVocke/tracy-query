#pragma once

#include <iosfwd>
#include <vector>

#include "tracy_query/model.hpp"
#include "tracy_query/trace.hpp"

namespace tracy_query {

void run_query(const Options& options, std::vector<Trace>& traces,
               std::ostream& output, std::ostream& diagnostics);

}  // namespace tracy_query
