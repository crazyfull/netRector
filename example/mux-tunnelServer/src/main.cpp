#include "ServerApplication.h"
#include "ServerCommandLine.h"
#include "ServerConfig.h"

int main(int argc, char** argv)
{
    ServerConfig config;

    switch (ServerCommandLine::parse(argc, argv, config)) {
    case CommandLineOutcome::ShowedHelp:
        return 0;
    case CommandLineOutcome::Invalid:
        return 2;
    case CommandLineOutcome::Run:
        break;
    }

    ServerApplication application(config);
    return application.run();
}
