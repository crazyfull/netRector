#ifndef RAISEFILEDESCRIPTORLIMIT_H
#define RAISEFILEDESCRIPTORLIMIT_H

// Raises the soft RLIMIT_NOFILE to the hard limit and returns the effective limit
// (capped at 1048576) so it can be used as the initial fd table size.
int raiseFileDescriptorLimit();

#endif // RAISEFILEDESCRIPTORLIMIT_H
