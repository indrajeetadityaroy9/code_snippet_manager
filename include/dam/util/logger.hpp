#pragma once

#include <iostream>
#include <string>
#include <memory>

namespace dam {

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARNING = 2,
    ERROR = 3
};

class Logger {
public:
    virtual ~Logger() = default;

    virtual void log(LogLevel level, const std::string& message) = 0;

    void debug(const std::string& msg) { log(LogLevel::DEBUG, msg); }
    void info(const std::string& msg) { log(LogLevel::INFO, msg); }
    void warning(const std::string& msg) { log(LogLevel::WARNING, msg); }
    void error(const std::string& msg) { log(LogLevel::ERROR, msg); }

    void set_min_level(LogLevel level) { min_level_ = level; }
    LogLevel get_min_level() const { return min_level_; }

protected:
    LogLevel min_level_ = LogLevel::INFO;
};

class ConsoleLogger : public Logger {
public:
    void log(LogLevel level, const std::string& message) override {
        if (level < min_level_) return;

        const char* prefix = "";
        switch (level) {
            case LogLevel::DEBUG:   prefix = "[DEBUG] "; break;
            case LogLevel::INFO:    prefix = "[INFO] "; break;
            case LogLevel::WARNING: prefix = "[WARN] "; break;
            case LogLevel::ERROR:   prefix = "[ERROR] "; break;
        }

        std::cout << prefix << message << std::endl;
    }
};

class NullLogger : public Logger {
public:
    void log(LogLevel, const std::string&) override {}
};

}  // namespace dam
