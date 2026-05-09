#include "VM.hpp"
#include <iostream>
#include <string>

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " <file.intense> [entry=main] [tapes=4] [--debug] [--step]\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) { usage(argv[0]); return 1; }

    std::string file  = argv[1];
    std::string entry = argc > 2 ? argv[2] : "main";
    int  tapeCount    = argc > 3 ? std::stoi(argv[3]) : 4;
    bool debugMode    = false;
    bool stepMode     = false;

    for (int i = 4; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--debug") debugMode = true;
        if (a == "--step")  stepMode  = true;
    }

    try {
        VM vm(file, tapeCount, debugMode, stepMode);
        vm.run(entry);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
