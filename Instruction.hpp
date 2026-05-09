#pragma once

#include <string>
#include <vector>

struct Instruction {
    std::string opcode;
    std::vector<std::string> args;
    int line = 0;

    bool operator==(const Instruction& o) const {
        return opcode == o.opcode && args == o.args;
    }
};
