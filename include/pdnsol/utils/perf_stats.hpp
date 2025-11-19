#ifndef PERF_STATS_HPP
#define PERF_STATS_HPP

#include <chrono>
#include <cstdio>

namespace perfstats {

#if defined(_WIN32)
    #define PERF_STATS_PLATFORM_WINDOWS 1
#elif defined(__linux__)
    #define PERF_STATS_PLATFORM_LINUX 1
#else
    #error "perfstats currently supports only Windows and Linux."
#endif

class ProgramStats {
public:
    explicit ProgramStats(const char* label = "perf");
    ~ProgramStats();

#if PERF_STATS_PLATFORM_WINDOWS
    using Tick = unsigned long long;

    Tick mCpuStartUser   = 0;
    Tick mCpuStartKernel = 0;

    void initCpuStart();
    double getCpuSeconds() const;
    double getMaxRssMb() const;

#elif PERF_STATS_PLATFORM_LINUX
    struct CpuTimes {
        double userSec = 0.0;
        double sysSec  = 0.0;
    };

    CpuTimes mCpuStart{};

    void initCpuStart();
    double getCpuSeconds() const;
    double getMaxRssMb() const;
#endif

private:
    const char* mLabel;
    std::chrono::steady_clock::time_point mWallStart;
};

} // namespace perfstats

// Convenience macro: automatically creates a ProgramStats object
// Put PERF_STATS("run"); at the top of main() to time the whole program.

#define PERF_STATS_UNIQUE_NAME2(x, y) x##y
#define PERF_STATS_UNIQUE_NAME(x, y)  PERF_STATS_UNIQUE_NAME2(x, y)

#define PERF_STATS(label) \
    ::perfstats::ProgramStats PERF_STATS_UNIQUE_NAME(_perfstats_, __LINE__){label}

#endif // PERF_STATS_HPP
