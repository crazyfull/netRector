#ifndef CLSTIMERMANAGER_H
#define CLSTIMERMANAGER_H
#include <functional>
#include <map>
#include <set>
#include <chrono>
#include "SocketContext.h"

class EpollReactor;
class TimerManager {
public:
    using Callback = std::function<void()>;
    using TimerId = int;

    TimerManager();
    ~TimerManager();

    TimerId addTimer(int interval_ms, Callback cb, bool singleShot = false);
    void removeTimer(TimerId id);
    void setInterval(TimerId id, int interval_ms);
    void setReactor(EpollReactor *r);
    void onTick();

    EpollReactor *getReactor() const;

private:

    void updateTimerFd();
    int fd() const;

    struct TimerInfo {
        Callback callback;
        std::chrono::milliseconds interval;
        std::chrono::steady_clock::time_point nextExpiration;
        bool singleShot;
    };

    EpollReactor* m_pReactor = nullptr;
    struct SocketContext m_SocketContext {};
    std::map<TimerId, TimerInfo> m_timers;
    std::multiset<std::pair<std::chrono::steady_clock::time_point, TimerId>> m_expirations;
    TimerId m_nextId = 0;
};
#endif // CLSTIMERMANAGER_H

