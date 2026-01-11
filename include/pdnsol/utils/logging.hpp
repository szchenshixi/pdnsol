#pragma once
#include <mutex>
#include <string>

namespace pdnsol {
// User API
#define PDN_DEBUG(...)      \
    Logger::instance().log( \
      Logger::Level::Debug, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define PDN_INFO(...)       \
    Logger::instance().log( \
      Logger::Level::Info, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define PDN_WARN(...)       \
    Logger::instance().log( \
      Logger::Level::Warning, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define PDN_ERROR(...)      \
    Logger::instance().log( \
      Logger::Level::Error, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define PDN_FATAL(...) \
    Logger::instance().fatal(__FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

#define PDN_DEBUG_IF(condition, ...) \
    if (condition)                   \
    Logger::instance().log(          \
      Logger::Level::Debug, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define PDN_INFO_IF(condition, ...) \
    if (condition)                  \
    Logger::instance().log(         \
      Logger::Level::Info, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define PDN_WARN_IF(condition, ...) \
    if (condition)                  \
    Logger::instance().log(         \
      Logger::Level::Warning, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define PDN_ERROR_IF(condition, ...) \
    if (condition)                   \
    Logger::instance().log(          \
      Logger::Level::Error, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define PDN_FATAL_IF(condition, ...) \
    if (condition)                   \
    Logger::instance().fatal(__FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

// Debug-time assertion. Disabled when NDEBUG is defined (release builds).
#ifndef NDEBUG
void assertFail(const char* expr, const char* file, int line,
                const char* function);

void assertFail(const char* expr, const char* file, int line,
                const char* function, const char* fmt, ...);
#define PDN_ASSERT(condition, ...)                                          \
    do {                                                                    \
        if (!(condition)) {                                                 \
            ssAssertFail(                                                   \
              #condition, __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__); \
        }                                                                   \
    } while (0)

#else
// In release builds, assertions are compiled out
#define PDN_ASSERT(condition, ...) ((void)0)
#endif // NDEBUG

class Logger {
  public:
    enum class Level { None = 0, Fatal, Error, Warning, Info, Debug, Verbose };

    static Logger& instance();

    // Initialize the logging facility (Only need to invoke once)
    void init(Level              level   = Level::Debug,
              const std::string& logPath = "app.log",
              size_t maxFileSize = 5 * 1024 * 1024, int maxFiles = 3,
              bool enableConsole = true);
    // Logger API with line and file infomation (For macro use)
    void log(Level level, const char* file, int line, const char* function,
             const char* format, ...) const;
    void fatal(const char* file, int line, const char* function,
               const char* format, ...) const;

  private:
    Logger()  = default;
    ~Logger() = default;

    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;

    mutable std::mutex mMutex;
    bool               mInitialized = false;
};
} // namespace pdnsol
