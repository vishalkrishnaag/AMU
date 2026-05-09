#include "Parser.hpp"
#include "Lexer.hpp"
#include <algorithm>
#include <cctype>

static std::string normalizeOpcode(std::string opcode) {
    std::transform(opcode.begin(), opcode.end(), opcode.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return opcode;
}

Instruction Parser::parse(
    const std::string& line,
    int lineNumber
) {
    auto tokens = Lexer::tokenize(line);

    Instruction ins;
    ins.line = lineNumber;

    if (tokens.empty())
        return ins;

    ins.opcode = normalizeOpcode(tokens[0]);

    for (size_t i = 1; i < tokens.size(); i++)
        ins.args.push_back(tokens[i]);

    return ins;
}