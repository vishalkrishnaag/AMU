#include "VM.hpp"
#include "Parser.hpp"
#include "Lexer.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <vector>

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " <file.intense|file.in10s> [entry=main] [tapes=4] [--debug] [--step]\n"
              << "       " << prog << " --repl [file.intense|file.in10s] [tapes=4] [--debug] [--step]\n";
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

static void redrawReplLine(const std::string& prompt, const std::string& line, size_t cursor) {
    std::cout << "\r\033[2K" << prompt << line;
    if (cursor < line.size())
        std::cout << "\033[" << (line.size() - cursor) << "D";
    std::cout.flush();
}

static bool readByte(char& c) {
    return read(STDIN_FILENO, &c, 1) > 0;
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
    std::string draft;
    size_t cursor = 0;
    std::cout << prompt;
    std::cout.flush();

    auto redraw = [&]() { redrawReplLine(prompt, line, cursor); };
    auto insertChar = [&](char ch) {
        line.insert(line.begin() + static_cast<std::ptrdiff_t>(cursor), ch);
        ++cursor;
        redraw();
    };
    auto replaceFromHistory = [&](const std::string& text) {
        line = text;
        cursor = line.size();
        redraw();
    };

    while (true) {
        char c = '\0';
        if (!readByte(c))
            return false;

        if (c == '\r' || c == '\n') {
            std::cout << "\n";
            return true;
        }

        if (c == 3) {
            std::cout << "\n";
            line = ":quit";
            return true;
        }

        if (c == 1) {
            cursor = 0;
            redraw();
            continue;
        }

        if (c == 4) {
            if (line.empty()) {
                std::cout << "\n";
                return false;
            }
            if (cursor < line.size()) {
                line.erase(line.begin() + static_cast<std::ptrdiff_t>(cursor));
                redraw();
            }
            continue;
        }

        if (c == 5) {
            cursor = line.size();
            redraw();
            continue;
        }

        if (c == 11) {
            line.erase(cursor);
            redraw();
            continue;
        }

        if (c == 12) {
            std::cout << "\033[2J\033[H";
            redraw();
            continue;
        }

        if (c == 21) {
            line.erase(0, cursor);
            cursor = 0;
            redraw();
            continue;
        }

        if (c == 23) {
            while (cursor > 0 && std::isspace(static_cast<unsigned char>(line[cursor - 1]))) {
                line.erase(line.begin() + static_cast<std::ptrdiff_t>(cursor - 1));
                --cursor;
            }
            while (cursor > 0 && !std::isspace(static_cast<unsigned char>(line[cursor - 1]))) {
                line.erase(line.begin() + static_cast<std::ptrdiff_t>(cursor - 1));
                --cursor;
            }
            redraw();
            continue;
        }

        if (c == 127 || c == '\b') {
            if (cursor > 0) {
                line.erase(line.begin() + static_cast<std::ptrdiff_t>(cursor - 1));
                --cursor;
                redraw();
            }
            continue;
        }

        if (c == '\t') {
            for (int i = 0; i < 4; ++i)
                insertChar(' ');
            continue;
        }

        if (c == 27) {
            char first = '\0';
            if (!readByte(first))
                continue;

            if (first == '[') {
                char second = '\0';
                if (!readByte(second))
                    continue;

                if (second >= '0' && second <= '9') {
                    char third = '\0';
                    if (!readByte(third))
                        continue;
                    if (second == '3' && third == '~') {
                        if (cursor < line.size()) {
                            line.erase(line.begin() + static_cast<std::ptrdiff_t>(cursor));
                            redraw();
                        }
                    } else if ((second == '1' || second == '7') && third == '~') {
                        cursor = 0;
                        redraw();
                    } else if ((second == '4' || second == '8') && third == '~') {
                        cursor = line.size();
                        redraw();
                    }
                    continue;
                }

                if (second == 'A') {
                    if (historyIndex == history.size())
                        draft = line;
                    if (historyIndex > 0) {
                        --historyIndex;
                        replaceFromHistory(history[historyIndex]);
                    }
                } else if (second == 'B') {
                    if (historyIndex + 1 < history.size()) {
                        ++historyIndex;
                        replaceFromHistory(history[historyIndex]);
                    } else {
                        historyIndex = history.size();
                        replaceFromHistory(draft);
                    }
                } else if (second == 'C') {
                    if (cursor < line.size()) {
                        ++cursor;
                        redraw();
                    }
                } else if (second == 'D') {
                    if (cursor > 0) {
                        --cursor;
                        redraw();
                    }
                } else if (second == 'H') {
                    cursor = 0;
                    redraw();
                } else if (second == 'F') {
                    cursor = line.size();
                    redraw();
                }
            } else if (first == 'O') {
                char second = '\0';
                if (!readByte(second))
                    continue;
                if (second == 'H') {
                    cursor = 0;
                    redraw();
                } else if (second == 'F') {
                    cursor = line.size();
                    redraw();
                }
            }
            continue;
        }

        if (c >= 32 && c != 127) {
            insertChar(c);
        }
    }
}

static bool needsMoreInput(const std::string& text) {
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    bool inComment = false;
    char quoteChar = '\0';

    for (char c : text) {
        if (inComment) {
            if (c == '\n')
                inComment = false;
            continue;
        }
        if (escaped) {
            escaped = false;
            continue;
        }
        if (inString) {
            if (c == '\\') escaped = true;
            else if (c == quoteChar) inString = false;
            continue;
        }
        if (c == '"' || c == '\'') {
            inString = true;
            quoteChar = c;
            continue;
        }
        if (c == '#') {
            inComment = true;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') {
            ++depth;
        } else if (c == ')' || c == ']' || c == '}') {
            if (depth > 0) --depth;
        }
    }

    std::string stripped = trim(text);
    return inString || depth > 0 || (!stripped.empty() && stripped.back() == '\\');
}

static bool readReplEntry(
    const std::string& prompt,
    std::string& entry,
    const std::vector<std::string>& history
) {
    std::string line;
    if (!readReplLine(prompt, line, history))
        return false;

    entry = line;
    while (!trim(entry).empty() && trim(entry)[0] != ':' && needsMoreInput(entry)) {
        if (!readReplLine("...> ", line, history))
            return false;
        entry += "\n";
        entry += line;
    }

    return true;
}

static std::vector<std::string> splitReplStatements(const std::string& source) {
    std::vector<std::string> statements;
    std::string current;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    bool inComment = false;
    char quoteChar = '\0';

    for (char c : source) {
        if (inComment) {
            if (c == '\n') {
                statements.push_back(current);
                current.clear();
                inComment = false;
            } else {
                current.push_back(c);
            }
            continue;
        }

        if (escaped) {
            current.push_back(c);
            escaped = false;
            continue;
        }

        if (inString) {
            current.push_back(c);
            if (c == '\\') escaped = true;
            else if (c == quoteChar) inString = false;
            continue;
        }

        if (c == '"' || c == '\'') {
            inString = true;
            quoteChar = c;
            current.push_back(c);
            continue;
        }

        if (c == '#') {
            inComment = true;
            current.push_back(c);
            continue;
        }

        if (c == '(' || c == '[' || c == '{') {
            ++depth;
            current.push_back(c);
            continue;
        }

        if (c == ')' || c == ']' || c == '}') {
            if (depth > 0) --depth;
            current.push_back(c);
            continue;
        }

        if ((c == ';' || c == '\n') && depth == 0) {
            statements.push_back(current);
            current.clear();
            continue;
        }

        current.push_back(c);
    }

    if (!current.empty())
        statements.push_back(current);
    return statements;
}

static bool isLabelOnlyLine(const std::string& line) {
    auto tokens = Lexer::tokenize(line);
    return tokens.size() == 1 && !tokens[0].empty() && tokens[0].back() == ':';
}

static std::string flattenStatementForHistory(std::string statement) {
    for (char& c : statement) {
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
    }
    return trim(statement);
}

static bool executeReplSource(
    VM& vm,
    const std::string& source,
    std::vector<std::string>& history,
    bool& lastWasPrintTape
) {
    bool executedAny = false;
    lastWasPrintTape = false;

    for (const auto& rawStatement : splitReplStatements(source)) {
        std::string statement = trim(rawStatement);
        if (statement.empty() || isLabelOnlyLine(statement))
            continue;

        Instruction ins = Parser::parse(statement, static_cast<int>(history.size() + 1));
        if (ins.opcode.empty())
            continue;

        vm.execute(ins);
        history.push_back(flattenStatementForHistory(statement));
        executedAny = true;
        lastWasPrintTape = ins.opcode == "PRINT_TAPE";
    }

    return executedAny;
}

static bool parseLongLong(const std::string& text, long long& value) {
    try {
        size_t consumed = 0;
        value = std::stoll(text, &consumed);
        return consumed == text.size();
    } catch (...) {
        return false;
    }
}

static int parseTapeCountArg(const std::string& text) {
    long long value = 0;
    if (!parseLongLong(text, value) || value < 1 || value > std::numeric_limits<int>::max())
        throw std::runtime_error("tapes must be a positive integer, got '" + text + "'");
    return static_cast<int>(value);
}

static void printBuffer(const std::vector<std::string>& buffer) {
    if (buffer.empty()) {
        std::cout << "Buffer is empty.\n";
        return;
    }

    for (size_t i = 0; i < buffer.size(); ++i)
        std::cout << (i + 1) << "  " << buffer[i] << "\n";
}

static bool collectPasteBlock(std::vector<std::string>& buffer) {
    std::cout << "Paste code. Finish with :end, cancel with :cancel.\n";
    std::vector<std::string> next;
    std::string line;
    while (std::getline(std::cin, line)) {
        std::string stripped = trim(line);
        if (stripped == ":cancel") {
            std::cout << "Paste canceled.\n";
            return false;
        }
        if (stripped == ":end" || stripped == ".")
            break;
        next.push_back(line);
    }

    buffer = std::move(next);
    std::cout << "Buffered " << buffer.size() << " line(s). Use :show, :edit, :insert, :delete, :run.\n";
    return true;
}

static bool readToggle(const std::string& value, bool& enabled) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (lower == "on" || lower == "true" || lower == "1") {
        enabled = true;
        return true;
    }
    if (lower == "off" || lower == "false" || lower == "0") {
        enabled = false;
        return true;
    }
    return false;
}

static bool startsWithCommand(const std::string& line, const std::string& command) {
    return line == command || (line.size() > command.size()
        && line.compare(0, command.size(), command) == 0
        && std::isspace(static_cast<unsigned char>(line[command.size()])));
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
        << "  :help                 show this help\n"
        << "  :tapes [tape] [rad]   show colored tape tree\n"
        << "  :cell [idx] [tape]    show a full cell value\n"
        << "  :debug on|off         toggle debug lines\n"
        << "  :step on|off          pause before each instruction when debug is on\n"
        << "  :paste                paste code into an editable buffer; finish with :end\n"
        << "  :show                 show buffered code with line numbers\n"
        << "  :edit N code          replace buffer line N\n"
        << "  :insert N code        insert before buffer line N\n"
        << "  :delete N             delete buffer line N\n"
        << "  :run                  execute buffered code\n"
        << "  :clear                clear buffered code\n"
        << "  :quit / :exit         leave REPL and optionally save session as .in10s\n"
        << "\n"
        << "Line editor: arrows, Home/End, Delete, Ctrl-A/E, Ctrl-K/U/W, Ctrl-L.\n"
        << "Enter one instruction, a semicolon chain, or a multi-line block, for example:\n"
        << "  SET \"hello\"\n"
        << "  MOVE 1 ; PRINT\n"
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
    bool stepMode = false;

    int arg = 2;
    if (arg < argc && !looksLikeFlag(argv[arg])) {
        file = argv[arg++];
    } else {
        writeReplBootFile(file);
    }

    if (arg < argc && !looksLikeFlag(argv[arg]))
        tapeCount = parseTapeCountArg(argv[arg++]);

    for (; arg < argc; ++arg) {
        std::string a = argv[arg];
        if (a == "--debug") debugMode = true;
        if (a == "--step") stepMode = true;
    }

    VM vm(file, tapeCount, debugMode, stepMode);
    std::vector<std::string> history;
    std::vector<std::string> buffer;

    printReplHelp();
    std::cout << vm.tapeSnapshot();

    std::string entry;
    while (true) {
        if (!readReplEntry("in10s> ", entry, history))
            break;
        std::string line = trim(entry);
        if (line.empty())
            continue;

        if (line == ":quit" || line == ":exit") {
            break;
        }
        if (line == ":help") {
            printReplHelp();
            continue;
        }

        if (startsWithCommand(line, ":tapes")) {
            std::istringstream in(line);
            std::string command;
            std::string tapeText;
            std::string radiusText;
            int tapeIndex = -1;
            int radius = 3;
            in >> command;
            if (in >> tapeText) {
                long long parsed = 0;
                if (!parseLongLong(tapeText, parsed)) {
                    std::cerr << "Usage: :tapes [tape] [radius]\n";
                    continue;
                }
                tapeIndex = static_cast<int>(parsed);
            }
            if (in >> radiusText) {
                long long parsed = 0;
                if (!parseLongLong(radiusText, parsed) || parsed < 1) {
                    std::cerr << "Usage: :tapes [tape] [radius]\n";
                    continue;
                }
                radius = static_cast<int>(parsed);
            }
            try {
                std::cout << vm.tapeSnapshot(radius, tapeIndex);
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << "\n";
            }
            continue;
        }

        if (startsWithCommand(line, ":cell")) {
            std::istringstream in(line);
            std::string command;
            std::string cellText;
            std::string tapeText;
            long long cellIndex = 0;
            int tapeIndex = -1;
            in >> command;
            if (in >> cellText) {
                if (!parseLongLong(cellText, cellIndex)) {
                    std::cerr << "Usage: :cell [cell] [tape]\n";
                    continue;
                }
            } else {
                try {
                    cellIndex = vm.tapePointer();
                } catch (const std::exception& e) {
                    std::cerr << "Error: " << e.what() << "\n";
                    continue;
                }
            }
            if (in >> tapeText) {
                long long parsed = 0;
                if (!parseLongLong(tapeText, parsed)) {
                    std::cerr << "Usage: :cell [cell] [tape]\n";
                    continue;
                }
                tapeIndex = static_cast<int>(parsed);
            }
            try {
                std::cout << vm.cellSnapshot(cellIndex, tapeIndex);
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << "\n";
            }
            continue;
        }

        if (startsWithCommand(line, ":debug")) {
            std::istringstream in(line);
            std::string command;
            std::string value;
            in >> command >> value;
            if (!readToggle(value, debugMode)) {
                std::cerr << "Usage: :debug on|off\n";
                continue;
            }
            vm.setDebugMode(debugMode);
            std::cout << "Debug " << (debugMode ? "on" : "off") << ".\n";
            continue;
        }

        if (startsWithCommand(line, ":step")) {
            std::istringstream in(line);
            std::string command;
            std::string value;
            in >> command >> value;
            if (!readToggle(value, stepMode)) {
                std::cerr << "Usage: :step on|off\n";
                continue;
            }
            vm.setStepMode(stepMode);
            std::cout << "Step " << (stepMode ? "on" : "off") << ".\n";
            continue;
        }

        if (line == ":paste") {
            collectPasteBlock(buffer);
            continue;
        }

        if (line == ":show") {
            printBuffer(buffer);
            continue;
        }

        if (line == ":clear") {
            buffer.clear();
            std::cout << "Buffer cleared.\n";
            continue;
        }

        if (startsWithCommand(line, ":edit")) {
            std::istringstream in(line);
            std::string command;
            size_t index = 0;
            in >> command >> index;
            std::string replacement;
            std::getline(in, replacement);
            replacement = trim(replacement);
            if (index == 0 || index > buffer.size() || replacement.empty()) {
                std::cerr << "Usage: :edit N code\n";
                continue;
            }
            buffer[index - 1] = replacement;
            printBuffer(buffer);
            continue;
        }

        if (startsWithCommand(line, ":insert")) {
            std::istringstream in(line);
            std::string command;
            size_t index = 0;
            in >> command >> index;
            std::string inserted;
            std::getline(in, inserted);
            inserted = trim(inserted);
            if (index == 0 || index > buffer.size() + 1 || inserted.empty()) {
                std::cerr << "Usage: :insert N code\n";
                continue;
            }
            buffer.insert(buffer.begin() + static_cast<std::ptrdiff_t>(index - 1), inserted);
            printBuffer(buffer);
            continue;
        }

        if (startsWithCommand(line, ":delete")) {
            std::istringstream in(line);
            std::string command;
            size_t index = 0;
            in >> command >> index;
            if (index == 0 || index > buffer.size()) {
                std::cerr << "Usage: :delete N\n";
                continue;
            }
            buffer.erase(buffer.begin() + static_cast<std::ptrdiff_t>(index - 1));
            printBuffer(buffer);
            continue;
        }

        if (line == ":run") {
            if (buffer.empty()) {
                std::cerr << "Buffer is empty. Use :paste first.\n";
                continue;
            }
            std::ostringstream source;
            for (const auto& bufferedLine : buffer)
                source << bufferedLine << "\n";
            try {
                bool lastWasPrintTape = false;
                bool ran = executeReplSource(vm, source.str(), history, lastWasPrintTape);
                if (ran && !lastWasPrintTape)
                    std::cout << vm.tapeSnapshot();
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << "\n";
                std::cout << vm.tapeSnapshot();
            }
            continue;
        }

        try {
            bool lastWasPrintTape = false;
            bool ran = executeReplSource(vm, entry, history, lastWasPrintTape);
            if (!ran)
                continue;
            if (ran && lastWasPrintTape)
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

    try {
        int arg = 2;
        if (arg < argc && !looksLikeFlag(argv[arg]))
            entry = argv[arg++];
        if (arg < argc && !looksLikeFlag(argv[arg]))
            tapeCount = parseTapeCountArg(argv[arg++]);

        for (int i = arg; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--debug") debugMode = true;
            if (a == "--step")  stepMode  = true;
        }

        VM vm(file, tapeCount, debugMode, stepMode);
        vm.run(entry);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
