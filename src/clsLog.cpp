#include "clsLog.h"
#include <atomic>

namespace netrector {

static void defaultHandler(int level, const char* message)
{
    static const char* names[] = {"error", "warn", "info", "debug"};
    fprintf(stderr, "netRector[%s] %s\n", names[level & 3], message);
}

static std::atomic<LogHandler> g_handler { defaultHandler };
static std::atomic<int> g_maxLevel { LOG_INFO };

void setLogHandler(LogHandler handler)
{
    g_handler.store(handler ? handler : defaultHandler);
}

void setLogLevel(int maxLevel)
{
    g_maxLevel.store(maxLevel);
}

bool logEnabled(int level)
{
    return level <= g_maxLevel.load(std::memory_order_relaxed);
}

void logMessage(int level, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_handler.load()(level, buf);
}

} // namespace netrector
