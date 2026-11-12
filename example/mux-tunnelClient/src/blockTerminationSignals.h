#ifndef BLOCKTERMINATIONSIGNALS_H
#define BLOCKTERMINATIONSIGNALS_H

#include <csignal>

// Blocks SIGINT and SIGTERM in the calling thread and returns the set, so that threads created
// afterwards inherit the mask and the main thread can wait for the signals with sigwait().
sigset_t blockTerminationSignals();

#endif // BLOCKTERMINATIONSIGNALS_H
