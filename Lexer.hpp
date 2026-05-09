#pragma once

#include <vector>
#include <string>

class Lexer {
public:
    static std::vector<std::string> tokenize(const std::string& line);
};