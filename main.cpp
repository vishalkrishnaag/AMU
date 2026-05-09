#include "VM.hpp"
#include "Parser.hpp"
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <vector>

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " <file.intense|file.in10s> [entry=main] [tapes=4] [--debug] [--step]\n"
              << "       " << prog << " --repl [file.intense|file.in10s] [tapes=4] [--debug]\n";
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

static bool hasSourceExtension(const std::string& path) {
    return (path.size() >= 8 && path.substr(path.size() - 8) == ".intense")
        || (path.size() >= 6 && path.substr(path.size() - 6) == ".in10s");
}

class ScopedRawTerminal {
private:
    termios original{};
    bool enabled = false;

public:
    ScopedRawTerminal() {
        if (!isatty(STDIN_FILENO))
            return;
        if (tcgetattr(STDIN_FILENO, &original) != 0)
            return;

        termios raw = original;
        raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
        raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;

        enabled = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
    }

    ~ScopedRawTerminal() {
        if (enabled)
            tcsetattr(STDIN_FILENO, TCSANOW, &original);
    }

    bool isEnabled() const { return enabled; }
};

static void redrawReplLine(const std::string& prompt, const std::string& line) {
    std::cout << "\r\033[2K" << prompt << line;
    std::cout.flush();
}

static bool readReplLine(
    const std::string& prompt,
    std::string& line,
    const std::vector<std::string>& history
) {
    line.clear();
    if (!isatty(STDIN_FILENO)) {
        std::cout << prompt;
        return static_cast<bool>(std::getline(std::cin, line));
    }

    ScopedRawTerminal raw;
    if (!raw.isEnabled()) {
        std::cout << prompt;
        return static_cast<bool>(std::getline(std::cin, line));
    }

    size_t historyIndex = history.size();
    std::cout << prompt;
    std::cout.flush();

    while (true) {
        char c = '\0';
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n == 0)
            return false;
        if (n < 0)
            continue;

        if (c == '\r' || c == '\n') {
            std::cout << "\n";
            return true;
        }

        if (c == 3) {
            std::cout << "\n";
            line = ":quit";
            return true;
        }

        if (c == 127 || c == '\b') {
            if (!line.empty()) {
                line.pop_back();
                redrawReplLine(prompt, line);
            }
            continue;
        }

        if (c == 27) {
            char seq[2] = {'\0', '\0'};
            if (read(STDIN_FILENO, &seq[0], 1) <= 0)
                continue;
            if (read(STDIN_FILENO, &seq[1], 1) <= 0)
                continue;

            if (seq[0] == '[' && seq[1] == 'A') {
                if (historyIndex > 0) {
                    --historyIndex;
                    line = history[historyIndex];
                    redrawReplLine(prompt, line);
                }
            } else if (seq[0] == '[' && seq[1] == 'B') {
                if (historyIndex + 1 < history.size()) {
                    ++historyIndex;
                    line = history[historyIndex];
                } else {
                    historyIndex = history.size();
                    line.clear();
                }
                redrawReplLine(prompt, line);
            }
            continue;
        }

        if (c >= 32 && c != 127) {
            line.push_back(c);
            std::cout << c;
            std::cout.flush();
        }
    }
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
        << "  :tapes         show active tape state\n"
        << "  :quit / :exit  leave REPL and optionally save session as .in10s\n"
        << "\n"
        << "Enter one instruction per line, for example:\n"
        << "  SET \"hello\"\n"
        << "  MOVE 1\n"
        << "  PRINT_TAPE 2\n"
        << "  SET (SET \"generated\" ; PRINT)\n"
        << "  EXEC\n"
        << "\n";
}

static void saveReplSession(const std::vector<std::string>& history) {
    std::cout << "Save entered code as .in10s? [y/N]: ";
    std::string answer;
    std::getline(std::cin, answer);
    answer = trim(answer);
    if (answer != "y" && answer != "Y" && answer != "yes" && answer != "YES")
        return;

    std::cout << "Output path [session.in10s]: ";
    std::string path;
    std::getline(std::cin, path);
    path = trim(path);
    if (path.empty()) path = "session.in10s";
    if (!hasSourceExtension(path))
        path += ".in10s";

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
    std::string file = "/tmp/intense_repl_boot.in10s";
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
        if (!readReplLine("in10s> ", line, history))
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
            if (ins.opcode == "PRINT_TAPE")
                continue;
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
    std::string entry = "main";
    int tapeCount = 4;
    bool debugMode = false;
    bool stepMode = false;

    int arg = 2;
    if (arg < argc && !looksLikeFlag(argv[arg]))
        entry = argv[arg++];
    if (arg < argc && !looksLikeFlag(argv[arg]))
        tapeCount = std::stoi(argv[arg++]);

    for (int i = arg; i < argc; ++i) {
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
