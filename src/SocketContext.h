#ifndef SOCKETCONTEXT_H
#define SOCKETCONTEXT_H
// ============================== SocketContext (low-level) ====================
// Owns fd, epoll event and send queue; used via composition by high-level handlers.
// buffer khandan dige per-socket nist: har reactor yek buffer moshtarak dare
// (EpollReactor::readBuffer) chon data dar haman callback masraf mishe.
#include "clsIntrusiveList.h"
#include "clsSendQueue.h"
#include <cstdint>
#include <cstdio>
#include <sys/epoll.h>
#include <chrono>

struct SocketContext
{
    int fd { -1 };
    uint16_t port { 0 };
    struct epoll_event ev {};
    SendQueue writeQueue;      // zanjire-ye chunk; khali = bedone heap
    uint64_t lastActive {};

};
#endif // SOCKETCONTEXT_H
