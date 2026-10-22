#ifndef CLSSOCKETLIST_H
#define CLSSOCKETLIST_H

#include <cstdint>
#include <functional>
#include <vector>
#include "clsIntrusiveList.h"


class TCPSocket;
enum SockTypes {
    IS_NOT_SET = 0,
    IS_TCP_LISTENER = 1,
    IS_TCP_SOCKET = 2,
    IS_UDP_LISTENER = 3,
    IS_UDP_SOCKET = 4,
    IS_TIMER_SOCKET = 5,
    IS_TIMER_MANAGER_SOCKET = 6,
    IS_DNS_LOOKUP_SOCKET = 7,
    IS_WAKEUP_SOCKET = 8
};

struct SockInfo {
    SockTypes type {IS_NOT_SET};
    void* socketBasePtr {nullptr};
    int fd{-1};
    uint32_t genID{0};
    IntrusiveLink active_link;
};

class SocketList
{
private:
    std::vector<SockInfo*> m_list;
    using ActiveSocketList = IntrusiveList<SockInfo, &SockInfo::active_link>;
    ActiveSocketList m_activeConnectionList;
    uint32_t m_genIDCounter{0};
    uint32_t m_tcpCount{0};

    bool grow(int fd);
    uint32_t nextGenID();

public:
    SocketList(int MaxFD);
    ~SocketList();

    SockInfo* add(int fd, SockTypes sockType, void *ptr);

    // get connection by fd
    SockInfo* get(int fd, uint32_t sockgenId);

    // remove connection
    void remove(int fd);
    uint32_t genIDCounter() const;
    uint32_t count() const;                 // hame-ye fd haye sabt shode (listener, timer, dns, ...)
    uint32_t countOfType(SockTypes type) const;   // felan faghat IS_TCP_SOCKET
    uint32_t maximumSize() const;
    std::vector<SockInfo*> *list();
    SockInfo* slot(size_t i) { return i < m_list.size() ? m_list[i] : nullptr; }
    void forEachActive(std::function<void(SockInfo*)> callback);
};

#endif // CLSSOCKETLIST_H
