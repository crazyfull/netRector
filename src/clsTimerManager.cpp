#include "clsTimerManager.h"
#include "clsSocketList.h"
#include "clsEpollReactor.h"
#include "clsTCPSocket.h"
//#include <sys/timerfd.h>
//#include <unistd.h>
//#include <iostream>  // For perror, or use printf if preferred

TimerManager::TimerManager() {
    m_SocketContext.fd = -1;
}

TimerManager::~TimerManager() {
    if (m_SocketContext.fd != -1) {
        if (m_pReactor) {
            m_pReactor->del_fd(m_SocketContext.fd, true);
        }
        ::close(m_SocketContext.fd);
        m_SocketContext.fd = -1;
    }
}

void TimerManager::setReactor(EpollReactor *r) {
    m_pReactor = r;
    if (!m_pReactor)
        return;

    if (m_SocketContext.fd == -1) {
        m_SocketContext.fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (m_SocketContext.fd == -1) {
            perror("timerfd_create");
            return;
        }

        m_SocketContext.ev.events = EPOLLIN;
        bool ret = m_pReactor->register_fd(m_SocketContext.fd, &m_SocketContext.ev, IS_TIMER_MANAGER_SOCKET, this);
        if (!ret) {
            ::close(m_SocketContext.fd);
            m_SocketContext.fd = -1;
        }
    }
}

TimerManager::TimerId TimerManager::addTimer(int interval_ms, Callback cb, bool singleShot) {
    if (!m_pReactor || m_SocketContext.fd == -1) {
        return -1;  // Invalid
    }

    if (interval_ms < 1)
        interval_ms = 1;

    TimerId id = m_nextId++;
    auto now = std::chrono::steady_clock::now();
    auto expiration = now + std::chrono::milliseconds(interval_ms);

    m_timers[id] = {std::move(cb), std::chrono::milliseconds(interval_ms), expiration, singleShot};
    m_expirations.insert({expiration, id});

    updateTimerFd();
    return id;
}

void TimerManager::removeTimer(TimerId id) {
    auto it = m_timers.find(id);
    if (it == m_timers.end()) return;

    // Remove from expirations
    auto range = m_expirations.equal_range({it->second.nextExpiration, id});
    for (auto eit = range.first; eit != range.second; ++eit) {
        if (eit->second == id) {
            m_expirations.erase(eit);
            break;
        }
    }

    m_timers.erase(it);
    updateTimerFd();
}

void TimerManager::setInterval(TimerId id, int interval_ms) {
    auto it = m_timers.find(id);
    if (it == m_timers.end()) return;

    // Remove old expiration
    auto range = m_expirations.equal_range({it->second.nextExpiration, id});
    for (auto eit = range.first; eit != range.second; ++eit) {
        if (eit->second == id) {
            m_expirations.erase(eit);
            break;
        }
    }

    // Update and add new expiration
    auto now = std::chrono::steady_clock::now();
    it->second.interval = std::chrono::milliseconds(interval_ms);
    it->second.nextExpiration = now + it->second.interval;
    m_expirations.insert({it->second.nextExpiration, id});

    updateTimerFd();
}

void TimerManager::onTick() {
    uint64_t expirations;
    ssize_t r = read(m_SocketContext.fd, &expirations, sizeof(expirations));  // Clear the fd
    (void)r;

    auto now = std::chrono::steady_clock::now();
    while (!m_expirations.empty()) {
        auto first = *m_expirations.begin();
        if (first.first > now)
            break;

        m_expirations.erase(m_expirations.begin());

        TimerId id = first.second;
        auto it = m_timers.find(id);
        if (it == m_timers.end())
            continue;

        // callback momkene removeTimer/setInterval/addTimer seda bezane, pas ghabl az
        // ejra vaziat timer nahaiy mishe va bad az an dige be iterator dast nemizanim.
        Callback cb;
        if (it->second.singleShot) {
            cb = std::move(it->second.callback);
            m_timers.erase(it);
        } else {
            it->second.nextExpiration = now + it->second.interval;
            m_expirations.insert({it->second.nextExpiration, id});
            cb = it->second.callback;
        }

        if (cb)
            cb();
    }

    updateTimerFd();
}

void TimerManager::updateTimerFd() {

    itimerspec new_value{};
    if (m_expirations.empty()) {
        // Disarm
        new_value.it_value.tv_sec = 0;
        new_value.it_value.tv_nsec = 0;
    } else {
        auto first = m_expirations.begin()->first;
        auto now = std::chrono::steady_clock::now();
        auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(first - now).count();

        if (duration_ns <= 0) {
            // Already expired, set to minimal
            new_value.it_value.tv_sec = 0;
            new_value.it_value.tv_nsec = 1;
        } else {
            new_value.it_value.tv_sec = duration_ns / 1000000000;
            new_value.it_value.tv_nsec = duration_ns % 1000000000;
        }
    }
    if (m_SocketContext.fd == -1)
        return;

    if (timerfd_settime(m_SocketContext.fd, 0, &new_value, nullptr) == -1) {
        perror("timerfd_settime");
    }
}


int TimerManager::fd() const {
    return m_SocketContext.fd;
}

EpollReactor *TimerManager::getReactor() const
{
    return m_pReactor;
}
