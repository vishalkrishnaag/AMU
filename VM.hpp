#pragma once

#include "Tape.hpp"
#include "FunctionLoader.hpp"
#include "NlpService.hpp"
#include <stack>

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
    FunctionLoader loader;
    NlpService nlp;
    std::stack<Frame> callStack;
    std::vector<Value> argRegs;     // argument register file for SETARG/GETARG
    void* dbConn = nullptr;          // optional PostgreSQL connection loaded through libpq at runtime

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
    void execute(const Instruction& ins);
    void debug(const Instruction& ins);
    std::string tapeSnapshot(int radius = 3, int tapeIndex = -1) const;
    void setStepMode(bool enable) { stepMode = enable; }
};
