#include "VM.hpp"
#include "Parser.hpp"
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " <file.intense|file.in10> [entry=main] [tapes=4] [--debug] [--step]\n"
              << "       " << prog << " --repl [file.intense|file.in10] [tapes=4] [--debug]\n";
}

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static bool looksLikeFlag(const std::string& s) {
    return s.size() >= 2 && s[0] == '-' && s[1] == '-';
}

static void writeReplBootFile(const std::string& path) {
    std::ofstream out(path);
    out << "main:\n"
        << "    RET\n";
}

static void printReplHelp() {
    std::cout
        << "Intense symbolic tape REPL\n"
        << "Commands:\n"
        << "  :help          show this help\n"
        << "  :tapes         show current tape state\n"
        << "  :quit / :exit  leave REPL and optionally save session as .in10\n"
        << "\n"
        << "Enter one instruction per line, for example:\n"
        << "  SET \"hello\"\n"
        << "  MOVE 1\n"
        << "  SET (SET \"generated\" ; PRINT)\n"
        << "  EXEC\n"
        << "\n";
}

static void saveReplSession(const std::vector<std::string>& history) {
    std::cout << "Save entered code as .in10? [y/N]: ";
    std::string answer;
    std::getline(std::cin, answer);
    answer = trim(answer);
    if (answer != "y" && answer != "Y" && answer != "yes" && answer != "YES")
        return;

    std::cout << "Output path [session.in10]: ";
    std::string path;
    std::getline(std::cin, path);
    path = trim(path);
    if (path.empty()) path = "session.in10";
    if (path.size() < 5 || path.substr(path.size() - 5) != ".in10")
        path += ".in10";

    std::ofstream out(path);
    if (!out) {
        std::cerr << "Could not write " << path << "\n";
        return;
    }

    out << "main:\n";
    for (const auto& line : history)
        out << "    " << line << "\n";
    out << "    RET\n";
    std::cout << "Saved " << path << "\n";
}

static int runRepl(int argc, char* argv[]) {
    std::string file = "/tmp/intense_repl_boot.in10";
    int tapeCount = 4;
    bool debugMode = false;

    int arg = 2;
    if (arg < argc && !looksLikeFlag(argv[arg])) {
        file = argv[arg++];
    } else {
        writeReplBootFile(file);
    }

    if (arg < argc && !looksLikeFlag(argv[arg]))
        tapeCount = std::stoi(argv[arg++]);

    for (; arg < argc; ++arg) {
        std::string a = argv[arg];
        if (a == "--debug") debugMode = true;
    }

    VM vm(file, tapeCount, debugMode, false);
    std::vector<std::string> history;

    printReplHelp();
    std::cout << vm.tapeSnapshot();

    std::string line;
    while (true) {
        std::cout << "in10> ";
        if (!std::getline(std::cin, line))
            break;
        line = trim(line);
        if (line.empty())
            continue;

        if (line == ":quit" || line == ":exit") {
            break;
        }
        if (line == ":help") {
            printReplHelp();
            continue;
        }
        if (line == ":tapes") {
            std::cout << vm.tapeSnapshot();
            continue;
        }

        try {
            Instruction ins = Parser::parse(line, static_cast<int>(history.size() + 1));
            if (ins.opcode.empty())
                continue;
            vm.execute(ins);
            history.push_back(line);
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
        }

        std::cout << vm.tapeSnapshot();
    }

    saveReplSession(history);
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) { usage(argv[0]); return 1; }

    if (std::string(argv[1]) == "--repl") {
        try {
            return runRepl(argc, argv);
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
    }

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
