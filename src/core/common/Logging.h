#pragma once

#include <QString>
#include <string>

namespace spw
{
namespace log
{

enum class Level
{
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
};

// Initialize logging (call once from main)
void init(const std::string& logFilePath = "");

// Shutdown logging
void shutdown();

// Set minimum log level
void setLevel(Level level);

// Log functions
void trace(const char* msg);
void debug(const char* msg);
void info(const char* msg);
void warn(const char* msg);
void error(const char* msg);
void critical(const char* msg);

// QString overloads
void trace(const QString& msg);
void debug(const QString& msg);
void info(const QString& msg);
void warn(const QString& msg);
void error(const QString& msg);
void critical(const QString& msg);

} // namespace log
} // namespace spw
