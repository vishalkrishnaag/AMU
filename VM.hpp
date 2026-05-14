#pragma once

#include "Tape.hpp"
#include "FunctionLoader.hpp"
#include "NlpService.hpp"
#include <filesystem>
#include <stack>
#include <unordered_map>
#include <utility>

struct Frame {
    std::string functionName;
    size_t pc = 0;
    std::vector<Value> args;        // old inline-arg system (ARG n)
    std::vector<Value> savedArgRegs; // snapshot of argRegs taken at CALL time
};

class VM {
private:
    std::vector<Tape> tapes;
    int activeTape = 0;
    bool debugMode = false;
    bool stepMode = false;
    bool halted = false;
    FunctionLoader loader;
    NlpService nlp;
    std::stack<Frame> callStack;
    std::vector<Value> argRegs;     // argument register file for SETARG/GETARG
    void* dbConn = nullptr;          // optional PostgreSQL connection loaded through libpq at runtime
    std::unordered_map<std::string, int> tapeNameCache;
    std::unordered_map<std::string, std::pair<int, long long>> tapeFunctionCache;
    int ensureTapeIndex(long long index, const std::string& opcode);
    void syncTapeMetadata(int tapeIndex);
    void syncAllTapeMetadata();
    int resolveTapeSelector(const std::string& selector, const std::string& opcode);
    bool resolveTapeFunction(const std::string& target, int& tapeIndex, long long& cellIndex);
    bool callTapeFunction(int tapeIndex, long long cellIndex, const std::string& displayName);

public:
    VM(
        const std::string& file,
        int tapeCount,
        bool debug,
        bool step = false
    );
    ~VM();

    void run(const std::string& entry);
    void runBlock(const Code& code);
    void loadSource(const std::string& source, const std::filesystem::path& baseDir);
    void execute(const Instruction& ins);
    void debug(const Instruction& ins);
    std::string tapeSnapshot(int radius = 3, int tapeIndex = -1) const;
    std::string cellSnapshot(long long cellIndex, int tapeIndex = -1) const;
    long long tapePointer(int tapeIndex = -1) const;
    bool isHalted() const { return halted; }
    void setDebugMode(bool enable) { debugMode = enable; }
    void setStepMode(bool enable) { stepMode = enable; }
};
