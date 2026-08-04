#pragma once

#include <iosfwd>
#include <stdexcept>
#include <string>
#include <string_view>

#include "tracy_query/model.hpp"

namespace tracy_query {

class OutputError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string json_escape(std::string_view value);
void emit_object(std::ostream& output, OutputFormat format, const Fields& fields);
void emit_record(std::ostream& output, OutputFormat format, const Record& record);
void emit_source(std::ostream& output, OutputFormat format, const Source& source);
void verify_output(std::ostream& output);

}  // namespace tracy_query
