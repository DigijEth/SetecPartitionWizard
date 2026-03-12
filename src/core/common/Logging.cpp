#include "Logging.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QMutex>
#include <QTextStream>

#include <fstream>

namespace spw
{
namespace log
{

static Level s_minLevel = Level::Info;
static std::ofstream s_logFile;
static QMutex s_mutex;

static const char* levelToString(Level level)
{
    switch (level)
    {
    case Level::Trace:
        return "TRACE";
    case Level::Debug:
        return "DEBUG";
    case Level::Info:
        return "INFO ";
    case Level::Warn:
        return "WARN ";
    case Level::Error:
        return "ERROR";
    case Level::Critical:
        return "CRIT ";
    }
    return "?????";
}

static void logImpl(Level level, const char* msg)
{
    if (level < s_minLevel)
        return;

    QMutexLocker lock(&s_mutex);

    auto timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    auto line = QString("[%1] [%2] %3").arg(timestamp, levelToString(level), msg);

    // Write to debug output
    qDebug().noquote() << line;

    // Write to file if open
    if (s_logFile.is_open())
    {
        s_logFile << line.toStdString() << "\n";
        s_logFile.flush();
    }
}

void init(const std::string& logFilePath)
{
    if (!logFilePath.empty())
    {
        s_logFile.open(logFilePath, std::ios::app);
    }
    info("Setec Partition Wizard logging initialized");
}

void shutdown()
{
    info("Logging shutdown");
    if (s_logFile.is_open())
    {
        s_logFile.close();
    }
}

void setLevel(Level level) { s_minLevel = level; }

void trace(const char* msg) { logImpl(Level::Trace, msg); }
void debug(const char* msg) { logImpl(Level::Debug, msg); }
void info(const char* msg) { logImpl(Level::Info, msg); }
void warn(const char* msg) { logImpl(Level::Warn, msg); }
void error(const char* msg) { logImpl(Level::Error, msg); }
void critical(const char* msg) { logImpl(Level::Critical, msg); }

void trace(const QString& msg) { logImpl(Level::Trace, msg.toUtf8().constData()); }
void debug(const QString& msg) { logImpl(Level::Debug, msg.toUtf8().constData()); }
void info(const QString& msg) { logImpl(Level::Info, msg.toUtf8().constData()); }
void warn(const QString& msg) { logImpl(Level::Warn, msg.toUtf8().constData()); }
void error(const QString& msg) { logImpl(Level::Error, msg.toUtf8().constData()); }
void critical(const QString& msg) { logImpl(Level::Critical, msg.toUtf8().constData()); }

} // namespace log
} // namespace spw
