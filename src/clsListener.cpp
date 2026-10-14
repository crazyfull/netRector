#include "clsListener.h"

Listener::Listener(EpollReactor* reactor) : m_pReactor(reactor) {}

Listener::~Listener()
{
    stop();
}

bool Listener::start(int port, const char* bindIP, EpollReactor::acceptCallback cb, void* ctx)
{
    if (!m_pReactor || m_fd != -1)
        return false;

    m_fd = m_pReactor->add_listener_fd(port, bindIP, cb, ctx);
    if (m_fd == -1)
        return false;

    m_port = port;
    return true;
}

void Listener::stop()
{
    if (m_fd != -1 && m_pReactor) {
        m_pReactor->remove_listener(m_fd);
        m_fd = -1;
    }
}
