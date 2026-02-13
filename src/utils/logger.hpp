#pragma once

#include <mutex>
#include <string>

namespace flashkv {

// Thread-safe, dependency-free logger.
// Format: [HH:MM:SS] [LEVEL] message
// INFO/DEBUG go to stdout, WARNING/ERROR go to stderr.
class Logger {
public:
    enum class Level { INFO, WARNING, ERROR, DEBUG };

    static void log(Level level, const std::string& message);
    static void info(const std::string& msg);
    static void warning(const std::string& msg);
    static void error(const std::string& msg);
    static void debug(const std::string& msg);

    // DEBUG messages are dropped unless debug mode is enabled.
    static void set_debug_mode(bool enabled);
    static bool debug_mode();

private:
    static std::mutex log_mutex;
    static bool debug_enabled;

    static std::string format_timestamp();
    static std::string level_to_string(Level level);
};

}  // namespace flashkv
