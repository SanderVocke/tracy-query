#include "tracy_query/trace.hpp"

#include "tracy_query/time.hpp"

#include <chrono>
#include <thread>
#include <utility>

#include <TracyFileRead.hpp>
#include <TracyWorker.hpp>

namespace tracy_query {
namespace {

std::string prefix(const TraceInput& input) {
    return "trace '" + input.path + "': ";
}

}  // namespace

Trace::Trace(TraceInput input) : input_{std::move(input)} {
    try {
        file_.reset(tracy::FileRead::Open(input_.path.c_str()));
        if (!file_) throw TraceLoadError{prefix(input_) + "cannot open file"};
        worker_ = std::make_unique<tracy::Worker>(*file_);
        // The file constructor starts Tracy's post-load indexing tasks. Those tasks
        // traverse the same packed vectors exposed by Worker, so do not expose the
        // capture until every index required by query adapters is stable.
        while (!worker_->AreSourceLocationZonesReady() ||
               !worker_->AreGpuSourceLocationZonesReady() ||
               !worker_->AreCallstackSamplesReady() ||
               !worker_->AreGhostZonesReady() ||
               !worker_->AreSymbolSamplesReady()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        first_time_ = worker_->GetFirstTime();
    } catch (const TraceLoadError&) {
        throw;
    } catch (const tracy::NotTracyDump&) {
        throw TraceLoadError{prefix(input_) + "not a Tracy trace file"};
    } catch (const tracy::UnsupportedVersion& error) {
        throw TraceLoadError{prefix(input_) + "unsupported trace version " +
                             std::to_string(error.version)};
    } catch (const tracy::LegacyVersion& error) {
        throw TraceLoadError{prefix(input_) + "legacy trace version " +
                             std::to_string(error.version) + " is not supported"};
    } catch (const tracy::LoadFailure& error) {
        throw TraceLoadError{prefix(input_) + "failed to load: " + error.msg};
    } catch (const tracy::FileReadError&) {
        throw TraceLoadError{prefix(input_) + "failed to read file"};
    } catch (const std::exception& error) {
        throw TraceLoadError{prefix(input_) + "failed to load: " + error.what()};
    }
}

Trace::~Trace() = default;
Trace::Trace(Trace&&) noexcept = default;
Trace& Trace::operator=(Trace&&) noexcept = default;

const TraceInput& Trace::input() const { return input_; }
const tracy::Worker& Trace::worker() const { return *worker_; }
tracy::Worker& Trace::worker() { return *worker_; }
int64_t Trace::first_time() const { return first_time_; }
int64_t Trace::last_time() const { return worker_->GetLastTime(); }
int64_t Trace::normalize(const int64_t timestamp) const {
    return normalize_timestamp(timestamp, first_time_);
}

}  // namespace tracy_query
