#ifndef SERVERCOMMANDLINE_H
#define SERVERCOMMANDLINE_H

#include "CommandLineOutcome.h"
#include "ServerConfig.h"

// Command line options of mux-tunnelServer; they override the static defaults.
class ServerCommandLine {
public:
    static CommandLineOutcome parse(int argc, char** argv, ServerConfig& config);
    static void printUsage(const char* program);
};

#endif // SERVERCOMMANDLINE_H
