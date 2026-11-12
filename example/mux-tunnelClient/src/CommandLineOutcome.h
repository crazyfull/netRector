#ifndef COMMANDLINEOUTCOME_H
#define COMMANDLINEOUTCOME_H

enum class CommandLineOutcome {
    Run,            // options are valid, start the program
    ShowedHelp,     // usage was printed, exit successfully
    Invalid         // error was printed, exit with a failure code
};

#endif // COMMANDLINEOUTCOME_H
