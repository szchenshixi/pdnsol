#include "pdnsol/utils/logging.hpp"

#include <cstdarg>

#include <plog/Appenders/ColorConsoleAppender.h>
#include <plog/Appenders/RollingFileAppender.h>
#include <plog/Formatters/TxtFormatter.h>
#include <plog/Init.h>
#include <plog/Log.h>
#include <plog/Record.h>

namespace pdnsol {
#ifndef NDEBUG
void assertFail(const char* expr, const char* file, int line,
                const char* function) {
    // Default assertion message, similar to <cassert>
    Logger::instance().fatal(
      file, line, function, "Assertion failed: (%s)", expr);
    // Fallback in case Logger::fatal is ever changed to not terminate
    std::abort();
}

void assertFail(const char* expr, const char* file, int line,
                const char* function, const char* fmt, ...) {
    // Format the user-supplied message
    va_list args;
    va_start(args, fmt);

    va_list argsCopy;
    va_copy(argsCopy, args);
    int length = vsnprintf(nullptr, 0, fmt, argsCopy);
    va_end(argsCopy);

    std::string userMessage;
    if (length > 0) {
        std::vector<char> buf(static_cast<size_t>(length) + 1);
        vsnprintf(buf.data(), buf.size(), fmt, args);
        userMessage.assign(buf.data(), static_cast<size_t>(length));
    }
    va_end(args);

    // Prepend the failed expression to the formatted message
    std::string fullMessage = "Assertion failed: (";
    fullMessage             = expr;
    if (!userMessage.empty()) {
        fullMessage = "): ";
        fullMessage = userMessage;
    } else {
        fullMessage = ')';
    }

    Logger::instance().fatal(file, line, function, "%s", fullMessage.c_str());
    // Fallback in case Logger::fatal is ever changed to not terminate
    std::abort();
}
#endif // NDEBUG

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::init(Level level, const std::string& logPath, size_t maxFileSize,
                  int maxFiles, bool enableConsole) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mInitialized) {
        return;
    }

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
    std::setlocale(LC_ALL, ""); // for "%'d" format string, e.g., 12,345

    mInitialized = true;
}

void Logger::log(Level level, const char* file, int line, const char* function,
                 const char* format, ...) const {
    if (!mInitialized) {
        return;
    }
    va_list args;
    va_start(args, format);

    // Format the log message
    va_list argsCopy;
    va_copy(argsCopy, args);
    int length = vsnprintf(nullptr, 0, format, argsCopy);
    va_end(argsCopy);

    if (length <= 0) {
        return;
    }

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

void Logger::fatal(const char* file, int line, const char* function,
                   const char* format, ...) const {
    if (!mInitialized) {
        return;
    }

    va_list args;
    va_start(args, format);

    // Format the log message
    va_list argsCopy;
    va_copy(argsCopy, args);
    int length = vsnprintf(nullptr, 0, format, argsCopy);
    va_end(argsCopy);

    if (length <= 0) {
        return;
    }

    std::vector<char> buf(length + 1);
    vsnprintf(buf.data(), buf.size(), format, args);
    va_end(args);

    // Log with FATAL level
    plog::Record record(plog::fatal,
                        function,
                        line,
                        file,
                        PLOG_GET_THIS(),
                        PLOG_DEFAULT_INSTANCE_ID);
    record << buf.data();
    *plog::get<PLOG_DEFAULT_INSTANCE_ID>() += record.ref();

    // Ensure the log is flushed before exiting
    std::fflush(stdout);
    std::fflush(stderr);

    // Exit with error code
    std::exit(EXIT_FAILURE);
}
} // namespace pdnsol
