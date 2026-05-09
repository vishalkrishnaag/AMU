#pragma once

#include "Instruction.hpp"
#include <vector>
#include <string>

class Parser {
public:
    static Instruction parse(
        const std::string& line,
        int lineNumber
    );
};