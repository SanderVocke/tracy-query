#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include "tracy_query/model.hpp"

namespace tracy {
class FileRead;
class Worker;
}  // namespace tracy

namespace tracy_query {

class Trace {
public:
    explicit Trace(TraceInput input);
    ~Trace();
    Trace(Trace&&) noexcept;
    Trace& operator=(Trace&&) noexcept;
    Trace(const Trace&) = delete;
    Trace& operator=(const Trace&) = delete;

    const TraceInput& input() const;
    const tracy::Worker& worker() const;
    tracy::Worker& worker();
    int64_t first_time() const;
    int64_t last_time() const;
    int64_t normalize(int64_t timestamp) const;

private:
    TraceInput input_;
    std::unique_ptr<tracy::FileRead> file_;
    std::unique_ptr<tracy::Worker> worker_;
    int64_t first_time_ = 0;
};

class TraceLoadError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace tracy_query
