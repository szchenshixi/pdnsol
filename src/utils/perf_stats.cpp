#include "pdnsol/utils/perf_stats.hpp"

#include "pdnsol/utils/logging.hpp"

#if PERF_STATS_PLATFORM_WINDOWS
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <psapi.h>
#include <windows.h>
#if defined(_MSC_VER)
#pragma comment(lib, "psapi.lib")
#endif
#elif PERF_STATS_PLATFORM_LINUX
#include <sys/resource.h>
#include <sys/time.h>
#endif

namespace perfstats {
using namespace pdnsol;

ProgramStats::ProgramStats(const char* label)
    : mLabel(label)
    , mWallStart(std::chrono::steady_clock::now()) {
    initCpuStart();
}

ProgramStats::~ProgramStats() {
    const auto wallEnd = std::chrono::steady_clock::now();
    const double wallSec =
      std::chrono::duration_cast<std::chrono::duration<double>>(wallEnd -
                                                                mWallStart)
        .count();

    const double cpuSec = getCpuSeconds();
    const double maxRssMb = getMaxRssMb();

    // [label] wall_time=...s cpu_time=...s peakMemUsage=...MB
    PDN_INFO("[%s] wall_time=%.3fs cpu_time=%.3fs peakMemUsage=%.1fMB\n",
            mLabel,
            wallSec,
            cpuSec,
            maxRssMb);
}

#if PERF_STATS_PLATFORM_WINDOWS

namespace {
using Tick = ProgramStats::Tick;

Tick filetimeToTicks(const FILETIME& fileTime) {
    ULARGE_INTEGER ui;
    ui.LowPart = fileTime.dwLowDateTime;
    ui.HighPart = fileTime.dwHighDateTime;
    return static_cast<Tick>(ui.QuadPart);
}
} // namespace

void ProgramStats::initCpuStart() {
    FILETIME createTime{}, exitTime{}, kernelTime{}, userTime{};
    if (GetProcessTimes(GetCurrentProcess(),
                        &createTime,
                        &exitTime,
                        &kernelTime,
                        &userTime)) {
        mCpuStartKernel = filetimeToTicks(kernelTime);
        mCpuStartUser = filetimeToTicks(userTime);
    }
}

double ProgramStats::getCpuSeconds() const {
    FILETIME createTime{}, exitTime{}, kernelTime{}, userTime{};
    if (GetProcessTimes(GetCurrentProcess(),
                        &createTime,
                        &exitTime,
                        &kernelTime,
                        &userTime)) {
        const Tick kernel = filetimeToTicks(kernelTime) - mCpuStartKernel;
        const Tick user = filetimeToTicks(userTime) - mCpuStartUser;
        // FILETIME units: 100-ns ticks
        return static_cast<double>(kernel + user) * 1e-7;
    }
    return 0.0;
}

double ProgramStats::getMaxRssMb() const {
    PROCESS_MEMORY_COUNTERS_EX processMemoryCounters;
    if (GetProcessMemoryInfo(
          GetCurrentProcess(),
          reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&processMemoryCounters),
          sizeof(processMemoryCounters))) {
        // PeakWorkingSetSize is in bytes
        return static_cast<double>(processMemoryCounters.PeakWorkingSetSize) /
               (1024.0 * 1024.0);
    }
    return 0.0;
}

#elif PERF_STATS_PLATFORM_LINUX

namespace {
ProgramStats::CpuTimes readCpuTimes() {
    ProgramStats::CpuTimes cpuTimes;
    rusage resourceUsage{};
    if (getrusage(RUSAGE_SELF, &resourceUsage) == 0) {
        cpuTimes.userSec =
          resourceUsage.ru_utime.tv_sec + resourceUsage.ru_utime.tv_usec / 1e6;
        cpuTimes.sysSec =
          resourceUsage.ru_stime.tv_sec + resourceUsage.ru_stime.tv_usec / 1e6;
    }
    return cpuTimes;
}
} // namespace

void ProgramStats::initCpuStart() { mCpuStart = readCpuTimes(); }

double ProgramStats::getCpuSeconds() const {
    const CpuTimes endTimes = readCpuTimes();
    return (endTimes.userSec - mCpuStart.userSec) +
           (endTimes.sysSec - mCpuStart.sysSec);
}

double ProgramStats::getMaxRssMb() const {
    rusage resourceUsage{};
    if (getrusage(RUSAGE_SELF, &resourceUsage) == 0) {
        // On Linux, ru_maxrss is in kilobytes
        return static_cast<double>(resourceUsage.ru_maxrss) / 1024.0;
    }
    return 0.0;
}

#endif // PERF_STATS_PLATFORM_*

} // namespace perfstats
