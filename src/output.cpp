#include "tracy_query/output.hpp"

#include <cmath>
#include <iomanip>
#include <ostream>
#include <sstream>

namespace tracy_query {
namespace {

void emit_json_value(std::ostream& output, const Value& value) {
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) output << "null";
        else if constexpr (std::is_same_v<T, bool>) output << (item ? "true" : "false");
        else if constexpr (std::is_same_v<T, std::string>) output << '"' << json_escape(item) << '"';
        else if constexpr (std::is_same_v<T, double>) {
            if (std::isfinite(item)) output << std::setprecision(17) << item;
            else output << "null";
        } else output << item;
    }, value);
}

void emit_text_value(std::ostream& output, const Value& value) {
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) output << "null";
        else if constexpr (std::is_same_v<T, bool>) output << (item ? "true" : "false");
        else if constexpr (std::is_same_v<T, std::string>) output << '"' << json_escape(item) << '"';
        else if constexpr (std::is_same_v<T, double>) {
            if (std::isfinite(item)) output << std::setprecision(17) << item;
            else output << "null";
        } else output << item;
    }, value);
}

void append(Fields& target, const Fields& source) {
    target.insert(target.end(), source.begin(), source.end());
}

}  // namespace

std::string json_escape(const std::string_view value) {
    std::ostringstream output;
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20) {
                output << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<unsigned>(character) << std::dec;
            } else output << static_cast<char>(character);
        }
    }
    return output.str();
}

void emit_object(std::ostream& output, const OutputFormat format, const Fields& fields) {
    if (format == OutputFormat::JsonLines) {
        output << '{';
        bool first = true;
        for (const auto& [name, value] : fields) {
            if (!first) output << ',';
            first = false;
            output << '"' << json_escape(name) << "\":";
            emit_json_value(output, value);
        }
        output << "}\n";
    } else {
        bool first = true;
        for (const auto& [name, value] : fields) {
            if (!first) output << ' ';
            first = false;
            output << name << '=';
            emit_text_value(output, value);
        }
        output << '\n';
    }
}

void emit_record(std::ostream& output, const OutputFormat format, const Record& record) {
    Fields fields{{"timestamp_ns", record.timestamp_ns}};
    if (record.end_timestamp_ns) {
        fields.emplace_back("end_timestamp_ns", *record.end_timestamp_ns);
        fields.emplace_back("duration_ns", *record.end_timestamp_ns - record.timestamp_ns);
    }
    fields.emplace_back("trace", record.trace);
    fields.emplace_back("source", record.source);
    fields.emplace_back("kind", std::string{kind_name(record.kind)});
    append(fields, record.fields);
    emit_object(output, format, fields);
}

void emit_source(std::ostream& output, const OutputFormat format, const Source& source) {
    Fields fields{{"trace", source.trace}, {"source", source.id}, {"source_type", source.type}};
    if (!source.name.empty()) fields.emplace_back("name", source.name);
    std::string kinds;
    for (const auto kind : source.kinds) {
        if (!kinds.empty()) kinds += ',';
        kinds += kind_name(kind);
    }
    fields.emplace_back("kinds", std::move(kinds));
    for (const auto& count : source.counts) fields.push_back(count);
    if (source.first_timestamp_ns) fields.emplace_back("first_timestamp_ns", *source.first_timestamp_ns);
    if (source.last_timestamp_ns) fields.emplace_back("last_timestamp_ns", *source.last_timestamp_ns);
    emit_object(output, format, fields);
}

void verify_output(std::ostream& output) {
    output.flush();
    if (!output) throw OutputError{"failed to write output"};
}

}  // namespace tracy_query
