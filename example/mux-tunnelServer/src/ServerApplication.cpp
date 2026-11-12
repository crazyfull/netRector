#include "ServerApplication.h"

#include "ServerTunnel.h"
#include "blockControlSignals.h"
#include "raiseFileDescriptorLimit.h"

#include "clsLog.h"
#include "clsServer.h"

#include <algorithm>
#include <cstdio>
#include <csignal>
#include <poll.h>
#include <string>
#include <sys/signalfd.h>
#include <thread>
#include <unistd.h>

using namespace netrector;

ServerApplication::ServerApplication(const ServerConfig& config)
    : m_config(config)
    , m_handshake(m_config)
{
}

TCPSocket* ServerApplication::createTunnel(void* context)
{
    ServerApplication* application = static_cast<ServerApplication*>(context);
    return new ServerTunnel(application->m_config, application->m_handshake, application->m_stats);
}

int ServerApplication::run()
{
    // Must happen before the reactor threads exist so that they inherit the signal mask.
    sigset_t signals = blockControlSignals();

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

    waitForShutdown(signals);

    server.stop();
    return 0;
}

// Runs on the main thread (no extra thread): waits on a signalfd and on stdin.
//   - the command "stats" (typed in the terminal) or SIGUSR1 prints the statistics
//   - SIGINT / SIGTERM return, which shuts the server down
void ServerApplication::waitForShutdown(const sigset_t& signals)
{
    const int signalFd = signalfd(-1, &signals, SFD_CLOEXEC);
    if (signalFd < 0) {
        NR_LOG(LOG_ERROR, "[server] signalfd failed, waiting with sigwait()");
        int signalNumber = 0;
        sigwait(&signals, &signalNumber);
        return;
    }

    std::printf("commands: stats, help   (SIGUSR1 also prints the statistics)\n");
    std::fflush(stdout);

    std::string input;
    bool stdinOpen = true;

    for (;;) {
        pollfd descriptors[2] = { { signalFd, POLLIN, 0 }, { STDIN_FILENO, POLLIN, 0 } };
        if (poll(descriptors, stdinOpen ? 2 : 1, -1) < 0)
            continue;

        if (descriptors[0].revents & POLLIN) {
            signalfd_siginfo info;
            if (read(signalFd, &info, sizeof(info)) != static_cast<ssize_t>(sizeof(info)))
                continue;

            if (info.ssi_signo == SIGUSR1) {
                printStats();
                continue;
            }

            NR_LOG(LOG_INFO, "[server] signal %u received, shutting down", info.ssi_signo);
            break;
        }

        if (stdinOpen && (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buffer[256];
            const ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count <= 0) {
                stdinOpen = false;      // no terminal (daemon, closed stdin): signals only
                continue;
            }

            input.append(buffer, static_cast<size_t>(count));
            size_t newline;
            while ((newline = input.find('\n')) != std::string::npos) {
                handleCommand(input.substr(0, newline));
                input.erase(0, newline + 1);
            }
        }
    }

    close(signalFd);
}

void ServerApplication::handleCommand(const std::string& command)
{
    if (command == "stats")
        printStats();
    else if (command == "help")
        std::printf("commands: stats, help\n");
    else if (!command.empty())
        std::printf("unknown command '%s' (try: stats)\n", command.c_str());
    std::fflush(stdout);
}

void ServerApplication::printStats()
{
    std::printf("%s", m_stats.report().c_str());
    std::fflush(stdout);
}
