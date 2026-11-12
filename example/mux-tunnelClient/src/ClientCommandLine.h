#ifndef CLIENTCOMMANDLINE_H
#define CLIENTCOMMANDLINE_H

#include "ClientConfig.h"
#include "CommandLineOutcome.h"

// Command line options of mux-tunnelClient; they override the static defaults.
class ClientCommandLine {
public:
    static CommandLineOutcome parse(int argc, char** argv, ClientConfig& config);
    static void printUsage(const char* program);
};

#endif // CLIENTCOMMANDLINE_H
