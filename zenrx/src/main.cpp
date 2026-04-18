#include "App.h"
#include "Log.h"
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv)
{
    // Pre-scan for --log-file before App constructor (which parses config and logs)
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
            zenrx::Log::initFile(argv[++i]);
            break;
        }
    }

    zenrx::App app(argc, argv);
    int ret = app.run();
    zenrx::Log::closeFile();
    return ret;
}
