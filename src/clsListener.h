#ifndef LISTENER_H
#define LISTENER_H

#include "clsEpollReactor.h"

// ============================== Listener =====================================
// Yek listener TCP roye yek reactor ke mishe jodagane start/stop kard
// (Server::AddNewListener hamin kar ro roye hame shard ha anjam mide).
//
//   Listener* l = new Listener(reactor);
//   l->start(8080, "0.0.0.0", myAcceptFactory, ctx);
//   ...
//   l->stop();  delete l;
class Listener
{
public:
    explicit Listener(EpollReactor* reactor = nullptr);
    ~Listener();

    void setReactor(EpollReactor* reactor) { m_pReactor = reactor; }

    // bindIP: nullptr/"*" = hame interface ha (IPv4+IPv6), "0.0.0.0" = hame IPv4, ya adres moshakhas
    bool start(int port, const char* bindIP, EpollReactor::acceptCallback cb, void* ctx);
    void stop();

    bool isListening() const { return m_fd != -1; }
    int fd() const { return m_fd; }
    int port() const { return m_port; }

    // ghadimi
    bool Addlisten(int port) { return start(port, nullptr, nullptr, nullptr); }

private:
    EpollReactor* m_pReactor = nullptr;
    int m_fd = -1;
    int m_port = 0;
};

#endif // LISTENER_H
