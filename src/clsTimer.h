#ifndef CLSTIMER_H
#define CLSTIMER_H
#include "clsTCPSocket.h"
//#include "epoll.h"

namespace netrector {

class Timer {
public:
    using Callback = std::function<void()>;

    Timer();
    ~Timer();
    bool start(int interval_ms, Callback cb);
    bool singleShot(int interval_ms, Callback cb);
    void stop();
    void setInterval(int interval_ms);
    void onTick();
    int fd() const;
    void setReactor(EpollReactor *r);

    EpollReactor *getReactor() const;

private:
    bool startInternal(int interval_ms, Callback cb, bool singleShot);

private:
    EpollReactor* m_pReactor = nullptr;
    struct SocketContext m_SocketContext {};
    Callback m_callback;
    bool m_singleShotMode;
};


} // namespace netrector

#endif // CLSTIMER_H
