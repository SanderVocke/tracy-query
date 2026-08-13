#define TRACY_QUERY_FIXTURE_ACCESS
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <TracyFileWrite.hpp>
#include <TracyWorker.hpp>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    using W = tracy::Worker;
    std::vector<W::ImportEventTimeline> timeline{
        {4242, 1000, "fixture.cpu", "", false, "fixture.cpp", 10},
        {4242, 2000, "fixture.cpu", "", true, "fixture.cpp", 10},
    };
    std::vector<W::ImportEventMessages> messages;
    std::vector<W::ImportEventPlots> plots;
    std::unordered_map<uint64_t, std::string> names{{4242, "fixture-thread"}, {9001, "external-thread"}};
    W worker("fixture", "fixture-structural", timeline, messages, plots, names);
    auto& data = worker.GetMutableDataForFixture();

    auto* context = new tracy::GpuCtxData{};
    context->period = 1.0f;
    context->type = tracy::GpuContextType::Custom;
    context->hasPeriod = true;
    context->hasCalibration = true;
    context->name = tracy::StringIdx{};
    auto* gpu = new tracy::GpuEvent{};
    gpu->SetCpuStart(1100);
    gpu->SetCpuEnd(1800);
    gpu->SetGpuStart(1200);
    gpu->SetGpuEnd(1700);
    int16_t source_location = 0;
    for (auto* thread : data.threads) {
        if (thread->id != 4242 || thread->timeline.empty()) continue;
        if (thread->timeline.is_magic()) {
            source_location = reinterpret_cast<tracy::Vector<tracy::ZoneEvent>&>(thread->timeline).front().SrcLoc();
        } else {
            source_location = thread->timeline.front()->SrcLoc();
        }
    }
    gpu->SetSrcLoc(source_location);
    gpu->SetThread(data.localThreadCompress.CompressThread(4242));
    gpu->SetChild(-1);
    gpu->callstack.SetVal(0);
    gpu->query_id = 1;
    context->threadData[4242].timeline.push_back(gpu);
    context->threadData[4242].stack.clear();
    context->count = 1;
    data.gpuData.push_back(context);
    data.gpuCnt = 1;

    auto* switches = new tracy::ContextSwitch{};
    tracy::ContextSwitchData sw{};
    sw.SetStart(1150);
    sw.SetEnd(1750);
    sw.SetCpu(0);
    sw.SetWakeupCpu(0);
    sw.SetReason(tracy::ContextSwitchData::Wakeup);
    sw.SetState(0);
    sw.SetWakeup(1100);
    switches->v.push_back(sw);
    data.ctxSwitch[4242] = switches;
    data.cpuThreadData[4242] = tracy::CpuThreadData{600, 1, 0};

    tracy::ContextSwitchCpu slice{};
    slice.SetStartThread(1150, data.externalThreadCompress.CompressThread(9001));
    slice.SetEnd(1750);
    data.cpuData[0].cs.push_back(slice);
    data.cpuDataCount = 1;
    data.ctxUsage.push_back(tracy::ContextSwitchUsage{1150, 0, 1});
    data.ctxUsage.push_back(tracy::ContextSwitchUsage{1750, 0, 0});
    data.ctxUsageReady = true;
    data.lastTime = 2000;

    std::unique_ptr<tracy::FileWrite> file{tracy::FileWrite::Open(argv[1], tracy::FileCompression::Zstd, 3, 1)};
    if (!file) return 1;
    worker.Write(*file, false);
    return 0;
}
