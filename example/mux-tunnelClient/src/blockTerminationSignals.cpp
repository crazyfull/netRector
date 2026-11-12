#include "blockTerminationSignals.h"

#include <pthread.h>

sigset_t blockTerminationSignals()
{
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    return signals;
}
