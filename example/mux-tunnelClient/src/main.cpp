#include "ClientApplication.h"
#include "ClientCommandLine.h"
#include "ClientConfig.h"

int main(int argc, char** argv)
{
    ClientConfig config;

    switch (ClientCommandLine::parse(argc, argv, config)) {
    case CommandLineOutcome::ShowedHelp:
        return 0;
    case CommandLineOutcome::Invalid:
        return 2;
    case CommandLineOutcome::Run:
        break;
    }

    ClientApplication application(config);
    return application.run();
}
