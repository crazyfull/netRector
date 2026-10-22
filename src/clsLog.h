#ifndef CLSLOG_H
#define CLSLOG_H
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cerrno>

// ============================== Log ==========================================
// Hame-ye payam haye ketabkhane az inja rad mishan. App mitone ba setLogHandler
// an ha ro be system log khodesh befreste; default: stderr.
//   netrector::setLogHandler([](int level, const char* msg) { ... });
namespace netrector {

enum LogLevel { LOG_ERROR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DEBUG = 3 };

typedef void (*LogHandler)(int level, const char* message);

void setLogHandler(LogHandler handler);
void setLogLevel(int maxLevel);     // payam haye ba level bozorgtar chap nemishan (default: LOG_INFO)
void logMessage(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
bool logEnabled(int level);

} // namespace netrector

#define NR_LOG(level, ...) do { if (netrector::logEnabled(level)) netrector::logMessage(level, __VA_ARGS__); } while (0)
#define NR_PERROR(what) NR_LOG(netrector::LOG_ERROR, "%s: %s", what, strerror(errno))

#endif // CLSLOG_H
