#include "pdnsol/utils/logging.hpp"

#include <cstdarg>

#include <plog/Appenders/ColorConsoleAppender.h>
#include <plog/Appenders/RollingFileAppender.h>
#include <plog/Formatters/TxtFormatter.h>
#include <plog/Init.h>
#include <plog/Log.h>
#include <plog/Record.h>

namespace pdnsol {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::init(Level level, const std::string& logPath, size_t maxFileSize,
                  int maxFiles, bool enableConsole) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mInitialized) { return; }

    static plog::RollingFileAppender<plog::TxtFormatter> fileAppender(
      logPath.c_str(), maxFileSize, maxFiles);
    // static plog::ConsoleAppender<plog::TxtFormatter> consoleAppender;
    static plog::ColorConsoleAppender<plog::TxtFormatter> colorConsoleAppender;

    if (enableConsole) {
        plog::init(static_cast<plog::Severity>(level), &fileAppender)
          .addAppender(&colorConsoleAppender);
    } else {
        plog::init(static_cast<plog::Severity>(level), &fileAppender);
    }

    mInitialized = true;
}

void Logger::log(Level level, const char* file, int line, const char* function,
                 const char* format, ...) const {
    if (!mInitialized) { return; }
    va_list args;
    va_start(args, format);

    // Format the log message
    va_list argsCopy;
    va_copy(argsCopy, args);
    int length = vsnprintf(nullptr, 0, format, argsCopy);
    va_end(argsCopy);

    if (length <= 0) { return; }

    std::vector<char> buf(length + 1);
    vsnprintf(buf.data(), buf.size(), format, args);
    va_end(args);

    // Use the low-level plog API to log file and line information
    plog::Record record(static_cast<plog::Severity>(level),
                        function,
                        line,
                        file,
                        PLOG_GET_THIS(),
                        PLOG_DEFAULT_INSTANCE_ID);
    record << buf.data();
    *plog::get<PLOG_DEFAULT_INSTANCE_ID>() += record.ref();
}
} // namespace pdnsol
