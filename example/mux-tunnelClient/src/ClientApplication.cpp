#include "ClientApplication.h"

#include "LocalSocksConnection.h"
#include "TunnelPool.h"
#include "blockTerminationSignals.h"
#include "raiseFileDescriptorLimit.h"

#include "clsLog.h"
#include "clsServer.h"

#include <algorithm>
#include <csignal>
#include <memory>
#include <vector>

using namespace netrector;

ClientApplication::ClientApplication(const ClientConfig& config)
    : m_config(config)
    , m_handshake(m_config)
{
}

TCPSocket* ClientApplication::createLocalConnection(void* context)
{
    ClientApplication* application = static_cast<ClientApplication*>(context);
    return new LocalSocksConnection(application->m_config);
}

int ClientApplication::run()
{
    // Must happen before the reactor threads exist so that they inherit the signal mask.
    sigset_t signals = blockTerminationSignals();

    setLogLevel(m_config.verbose ? LOG_DEBUG : LOG_INFO);

    const int maxConnections = raiseFileDescriptorLimit();

    // The tunnels are divided between the reactor threads; there is no point in a thread without one.
    const int threads = std::max(1, std::min(m_config.threads, m_config.tunnelCount));

    // The pools are declared before the server so that they outlive it: during Server::stop()
    // the sockets are closed and call back into their pool.
    std::vector<std::unique_ptr<TunnelPool>> pools;

    Server server(maxConnections, threads);
    server.setUseGarbageCollector(false);

    for (int i = 0; i < threads; i++) {
        const size_t tunnelCount = static_cast<size_t>(m_config.tunnelCount / threads + (i < m_config.tunnelCount % threads ? 1 : 0));
        EpollReactor* reactor = server.getShard(i);

        pools.push_back(std::make_unique<TunnelPool>(*reactor, m_config, m_handshake, tunnelCount));
        TunnelPool* pool = pools.back().get();

        reactor->setUserData(pool);
        reactor->post([pool]() { pool->start(); });     // runs on the reactor thread
    }

    if (!server.AddNewListener(m_config.listenPort, m_config.listenAddress.c_str(), &ClientApplication::createLocalConnection, this)) {
        NR_LOG(LOG_ERROR, "[client] cannot listen on %s:%u", m_config.listenAddress.c_str(), m_config.listenPort);
        return 1;
    }

    if (!server.start()) {
        NR_LOG(LOG_ERROR, "[client] cannot start the reactor threads");
        return 1;
    }

    NR_LOG(LOG_INFO, "[client] SOCKS5 on %s:%u, %d tunnel(s) to %s:%u over %d thread(s)",
           m_config.listenAddress.c_str(), m_config.listenPort, m_config.tunnelCount,
           m_config.cdnAddress.c_str(), m_config.cdnPort, threads);

    int signalNumber = 0;
    sigwait(&signals, &signalNumber);
    NR_LOG(LOG_INFO, "[client] signal %d received, shutting down", signalNumber);

    for (std::unique_ptr<TunnelPool>& pool : pools)
        pool->stop();       // no reconnects while the sockets are closed
    server.stop();
    return 0;
}
