#include "utils/logger.hpp"

#include <chrono>
#include <ctime>
#include <iostream>

namespace flashkv {

std::mutex Logger::log_mutex;
bool Logger::debug_enabled = false;

void Logger::set_debug_mode(bool enabled) {
    std::lock_guard<std::mutex> lock(log_mutex);
    debug_enabled = enabled;
}

bool Logger::debug_mode() {
    std::lock_guard<std::mutex> lock(log_mutex);
    return debug_enabled;
}

std::string Logger::level_to_string(Level level) {
    switch (level) {
        case Level::INFO:    return "INFO";
        case Level::WARNING: return "WARNING";
        case Level::ERROR:   return "ERROR";
        case Level::DEBUG:   return "DEBUG";
    }
    return "UNKNOWN";
}

std::string Logger::format_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);

    std::tm tm_buf{};
    // localtime_r is the thread-safe variant; the caller already holds the lock
    // but keeping this reentrant avoids surprises if that ever changes.
    localtime_r(&t, &tm_buf);

    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm_buf);
    return std::string(buf);
}

void Logger::log(Level level, const std::string& message) {
    std::lock_guard<std::mutex> lock(log_mutex);

    if (level == Level::DEBUG && !debug_enabled) return;

    std::ostream& out = (level == Level::WARNING || level == Level::ERROR)
                            ? std::cerr
                            : std::cout;

    out << "[" << format_timestamp() << "] "
        << "[" << level_to_string(level) << "] "
        << message << std::endl;
}

void Logger::info(const std::string& msg)    { log(Level::INFO, msg); }
void Logger::warning(const std::string& msg) { log(Level::WARNING, msg); }
void Logger::error(const std::string& msg)   { log(Level::ERROR, msg); }
void Logger::debug(const std::string& msg)   { log(Level::DEBUG, msg); }

}  // namespace flashkv
