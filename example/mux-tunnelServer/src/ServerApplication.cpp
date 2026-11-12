#include "ServerApplication.h"

#include "ServerTunnel.h"
#include "blockTerminationSignals.h"
#include "raiseFileDescriptorLimit.h"

#include "clsLog.h"
#include "clsServer.h"

#include <algorithm>
#include <csignal>
#include <thread>

using namespace netrector;

ServerApplication::ServerApplication(const ServerConfig& config)
    : m_config(config)
    , m_handshake(m_config)
{
}

TCPSocket* ServerApplication::createTunnel(void* context)
{
    ServerApplication* application = static_cast<ServerApplication*>(context);
    return new ServerTunnel(application->m_config, application->m_handshake);
}

int ServerApplication::run()
{
    // Must happen before the reactor threads exist so that they inherit the signal mask.
    sigset_t signals = blockTerminationSignals();

    setLogLevel(m_config.verbose ? LOG_DEBUG : LOG_INFO);

    const int maxConnections = raiseFileDescriptorLimit();
    const int threads = m_config.threads > 0
        ? m_config.threads
        : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

    Server server(maxConnections, threads);
    server.setUseGarbageCollector(false);

    if (!server.AddNewListener(m_config.listenPort, m_config.listenAddress.c_str(), &ServerApplication::createTunnel, this)) {
        NR_LOG(LOG_ERROR, "[server] cannot listen on %s:%u", m_config.listenAddress.c_str(), m_config.listenPort);
        return 1;
    }

    if (!server.start()) {
        NR_LOG(LOG_ERROR, "[server] cannot start the reactor threads");
        return 1;
    }

    NR_LOG(LOG_INFO, "[server] listening on %s:%u (%d threads, max %d fds)",
           m_config.listenAddress.c_str(), m_config.listenPort, threads, maxConnections);

    int signalNumber = 0;
    sigwait(&signals, &signalNumber);
    NR_LOG(LOG_INFO, "[server] signal %d received, shutting down", signalNumber);

    server.stop();
    return 0;
}
