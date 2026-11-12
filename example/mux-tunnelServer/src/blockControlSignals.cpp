#include "blockControlSignals.h"

#include <pthread.h>

sigset_t blockControlSignals()
{
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    return signals;
}
