#ifndef BLOCKCONTROLSIGNALS_H
#define BLOCKCONTROLSIGNALS_H

#include <csignal>

// Blocks SIGINT, SIGTERM and SIGUSR1 in the calling thread and returns the set, so that threads
// created afterwards inherit the mask and the main thread can read the signals from a signalfd.
sigset_t blockControlSignals();

#endif // BLOCKCONTROLSIGNALS_H
