
#include "clsSocketList.h"
#include "clsTCPSocket.h"
#include <cstdio>

// saghfe roshd list (fd haye bozorgtar rad mishan)
static constexpr int SOCKET_LIST_HARD_LIMIT = 4 * 1024 * 1024;

uint32_t SocketList::genIDCounter() const
{
    return m_genIDCounter;
}

uint32_t SocketList::count() const
{
    return m_activeConnectionList.size();
}

uint32_t SocketList::countOfType(SockTypes type) const
{
    return type == IS_TCP_SOCKET ? m_tcpCount : 0;
}


uint32_t SocketList::maximumSize() const
{
    return m_list.size();
}

std::vector<SockInfo *> *SocketList::list()
{
    return &m_list;
}

void SocketList::forEachActive(std::function<void (SockInfo *)> callback) {
    m_activeConnectionList.for_each(callback);
}

// slot ha lazy sakhte mishan: hafeze motenaseb ba bishtarin fd-e estefade shode,
// na ba ulimit (1M slot * 56B * tedad shard).
SocketList::SocketList(int MaxFD):m_list(MaxFD > 0 ? MaxFD : 1024, nullptr) {
}

SocketList::~SocketList()
{
    for (int i = 0; i < (int)m_list.size(); i++) {
        delete m_list[i];
    }
}

// fd ha dar kol process moshtarak hastan; agar fd az size list bozorgtar shod
// (masalan ulimit bad az sakhte Server bala rafte) list roshd mikone.
// SockInfo ha ba pointer negah dashte mishan pas adres ghabli ha sabet mimone.
bool SocketList::grow(int fd)
{
    if (fd >= SOCKET_LIST_HARD_LIMIT)
        return false;

    size_t newSize = m_list.size();
    while (newSize <= (size_t)fd)
        newSize *= 2;

    m_list.resize(newSize, nullptr);
    return true;
}

uint32_t SocketList::nextGenID()
{
    m_genIDCounter++;
    if(m_genIDCounter == 0)
        m_genIDCounter = 1;
    return m_genIDCounter;
}

SockInfo *SocketList::add(int fd, SockTypes sockType, void* ptr) {
    if (fd < 0){
        return nullptr;
    }

    if (fd >= (int)m_list.size() && !grow(fd)){
        return nullptr;
    }

    if (!m_list[fd])
        m_list[fd] = new SockInfo;

    SockInfo *pSocketInfo = m_list[fd];
    pSocketInfo->fd = fd;
    pSocketInfo->type = sockType;
    if (sockType == IS_TCP_SOCKET)
        m_tcpCount++;
    pSocketInfo->socketBasePtr = ptr;
    pSocketInfo->genID = nextGenID();

    //add client to active list
    m_activeConnectionList.push_back(pSocketInfo);

    return pSocketInfo;
}

SockInfo *SocketList::get(int fd, uint32_t sockgenId) {
    if (fd < 0 || fd >= (int)m_list.size())
        return nullptr;

    SockInfo* pSocketInfo = m_list[fd];
    if(!pSocketInfo || pSocketInfo->type == IS_NOT_SET || pSocketInfo->genID != sockgenId){
        return nullptr;
    }

    return pSocketInfo;
}

void SocketList::remove(int fd) {
    if (fd < 0 || fd >= (int)m_list.size())
        return;

    SockInfo* pSocketInfo = m_list[fd];
    if (pSocketInfo) {
        if (pSocketInfo->type == IS_TCP_SOCKET && m_tcpCount > 0)
            m_tcpCount--;

        //remove from active list
        m_activeConnectionList.remove(pSocketInfo);

        pSocketInfo->fd = -1;
        pSocketInfo->socketBasePtr = nullptr;

        // event haye ghadimi in fd (dar haman batch) ba genID jadid match nemishan
        pSocketInfo->genID = nextGenID();
        pSocketInfo->type = IS_NOT_SET;
    }
}

