#include "Lexer.hpp"
#include <cctype>

namespace {

bool isWhitespace(char c) {
    return std::isspace(static_cast<unsigned char>(c));
}

std::string readQuotedToken(const std::string& line, size_t& i, char quoteChar) {
    std::string token;
    token.push_back(line[i++]);
    bool escaped = false;

    while (i < line.size()) {
        char c = line[i++];
        token.push_back(c);

        if (escaped) {
            escaped = false;
            continue;
        }

        if (c == '\\') {
            escaped = true;
            continue;
        }

        if (c == quoteChar)
            break;
    }

    return token;
}

std::string readStructuredToken(const std::string& line, size_t& i, char openChar) {
    char closeChar = openChar == '{' ? '}' : openChar == '[' ? ']' : ')';
    std::string token;
    int depth = 0;
    bool inString = false;
    bool escaped = false;

    while (i < line.size()) {
        char c = line[i++];
        token.push_back(c);

        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }

        if (c == '"') {
            inString = true;
            continue;
        }

        if (c == openChar) {
            depth++;
        } else if (c == closeChar) {
            depth--;
            if (depth == 0)
                break;
        }
    }

    return token;
}

}

std::vector<std::string> Lexer::tokenize(const std::string& line) {

    std::vector<std::string> tokens;
    size_t i = 0;

    while (i < line.size()) {
        while (i < line.size() && isWhitespace(line[i]))
            i++;

        if (i >= line.size())
            break;

        char c = line[i];

        if (c == '#')   // rest of line is a comment
            break;

        if (c == '"' || c == '\'') {
            tokens.push_back(readQuotedToken(line, i, c));
            continue;
        }

        if (c == '{' || c == '[' || c == '(') {
            tokens.push_back(readStructuredToken(line, i, c));
            continue;
        }

        std::string token;
        while (i < line.size() && !isWhitespace(line[i])) {
            token.push_back(line[i++]);
        }

        if (!token.empty())
            tokens.push_back(std::move(token));
    }

    return tokens;
}