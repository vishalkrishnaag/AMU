#include "VM.hpp"
#include "Parser.hpp"
#include "Lexer.hpp"
#include <Eigen/Dense>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <numeric>
#include <vector>
#include <filesystem>
#include <dlfcn.h>
#include <unistd.h>
#include <limits>
#include <functional>

// ---------------------------------------------------------------------------
// Internal helpers (anonymous namespace)
// ---------------------------------------------------------------------------
namespace {

constexpr long long TAPE_FUNCTIONS_CELL = -1;
constexpr long long TAPE_NAME_CELL = -2;

// ---- String utilities ------------------------------------------------------

std::string stringifyValue(const Value& value);
const std::string& argOrThrow(const Instruction& ins, size_t index);
Value resolveOperand(const std::string& arg, Tape& tape);
long long integerValue(const Value& value, const std::string& opcode, const std::string& role);

bool isInteger(const std::string& token) {
    if (token.empty()) return false;
    size_t start = (token[0] == '+' || token[0] == '-') ? 1 : 0;
    if (start >= token.size()) return false;
    return std::all_of(token.begin() + start, token.end(),
                       [](unsigned char c) { return std::isdigit(c); });
}

bool isFloat(const std::string& token) {
    if (token.empty()) return false;
    size_t start = (token[0] == '+' || token[0] == '-') ? 1 : 0;
    if (start >= token.size()) return false;

    bool hasDecimal = false, hasDigits = false;

    for (size_t i = start; i < token.size(); ++i) {
        char c = token[i];
        if (std::isdigit(static_cast<unsigned char>(c))) { hasDigits = true; continue; }
        if (c == '.' && !hasDecimal)                      { hasDecimal = true; continue; }
        if ((c == 'e' || c == 'E') && i + 1 < token.size()) {
            size_t j = i + 1;
            if (token[j] == '+' || token[j] == '-') ++j;
            if (j == token.size()) return false;
            for (; j < token.size(); ++j)
                if (!std::isdigit(static_cast<unsigned char>(token[j]))) return false;
            return hasDigits;
        }
        return false;
    }
    return hasDigits && hasDecimal;
}

long long parseLongLongStrict(const std::string& text, const std::string& context, const std::string& role) {
    try {
        size_t consumed = 0;
        long long value = std::stoll(text, &consumed);
        if (consumed == text.size())
            return value;
    } catch (const std::out_of_range&) {
        throw std::runtime_error(context + ": " + role + " is out of range: '" + text + "'");
    } catch (const std::invalid_argument&) {
    }
    throw std::runtime_error(context + ": " + role + " must be an integer, got '" + text + "'");
}

int parseIntStrict(const std::string& text, const std::string& context, const std::string& role) {
    long long value = parseLongLongStrict(text, context, role);
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        throw std::runtime_error(context + ": " + role + " is out of int range: '" + text + "'");
    return static_cast<int>(value);
}

size_t parseSizeStrict(const std::string& text, const std::string& context, const std::string& role) {
    long long value = parseLongLongStrict(text, context, role);
    if (value < 0)
        throw std::runtime_error(context + ": " + role + " must be non-negative, got '" + text + "'");
    return static_cast<size_t>(value);
}

double parseDoubleStrict(const std::string& text, const std::string& context, const std::string& role) {
    try {
        size_t consumed = 0;
        double value = std::stod(text, &consumed);
        if (consumed == text.size())
            return value;
    } catch (const std::out_of_range&) {
        throw std::runtime_error(context + ": " + role + " is out of range: '" + text + "'");
    } catch (const std::invalid_argument&) {
    }
    throw std::runtime_error(context + ": " + role + " must be numeric, got '" + text + "'");
}

std::string toUpperStr(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return out;
}

std::string trimStr(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string quoteIntenseString(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            default:   out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

std::string answerLiteralToken(const Value& v) {
    switch (v.kind()) {
        case ValueKind::Nil:   return "nil";
        case ValueKind::Bool:  return std::get<bool>(v.data) ? "true" : "false";
        case ValueKind::Int:   return std::to_string(std::get<long long>(v.data));
        case ValueKind::Float: {
            std::ostringstream out;
            out << std::setprecision(17) << std::get<double>(v.data);
            return out.str();
        }
        case ValueKind::Char:  return quoteIntenseString(std::string(1, std::get<char>(v.data)));
        case ValueKind::Str:   return quoteIntenseString(std::get<std::string>(v.data));
        default:               return quoteIntenseString(stringifyValue(v));
    }
}

std::string fitColumn(std::string text, size_t width) {
    if (text.size() > width) {
        if (width > 3)
            text = text.substr(0, width - 3) + "...";
        else
            text = text.substr(0, width);
    }
    if (text.size() < width)
        text += std::string(width - text.size(), ' ');
    return text;
}

std::string unescapeString(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\\' && i + 1 < input.size()) {
            char next = input[++i];
            switch (next) {
                case 'n':  result.push_back('\n'); break;
                case 't':  result.push_back('\t'); break;
                case 'r':  result.push_back('\r'); break;
                case '\\': result.push_back('\\'); break;
                case '"':  result.push_back('"');  break;
                case '\'': result.push_back('\''); break;
                default:   result.push_back(next); break;
            }
        } else {
            result.push_back(input[i]);
        }
    }
    return result;
}

// ---- JSON parser -----------------------------------------------------------

void skipWhitespace(const std::string& text, size_t& pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
        ++pos;
}

Value parseJsonValue(const std::string& text, size_t& pos); // forward

Value parseJsonString(const std::string& text, size_t& pos) {
    if (pos >= text.size() || text[pos] != '"')
        throw std::runtime_error("Invalid JSON string");
    ++pos;
    std::string result;
    bool escaped = false;
    while (pos < text.size()) {
        char c = text[pos++];
        if (escaped) {
            switch (c) {
                case 'n':  result.push_back('\n'); break;
                case 't':  result.push_back('\t'); break;
                case 'r':  result.push_back('\r'); break;
                case '\\': result.push_back('\\'); break;
                case '"':  result.push_back('"');  break;
                case '\'': result.push_back('\''); break;
                default:   result.push_back(c);    break;
            }
            escaped = false;
            continue;
        }
        if (c == '\\') { escaped = true; continue; }
        if (c == '"')  return Value(std::move(result));
        result.push_back(c);
    }
    throw std::runtime_error("Unterminated JSON string");
}

Value parseJsonLiteral(const std::string& text, size_t& pos) {
    size_t start = pos;
    while (pos < text.size() &&
           !std::isspace(static_cast<unsigned char>(text[pos])) &&
           text[pos] != ',' && text[pos] != ']' && text[pos] != '}')
        ++pos;

    std::string token = text.substr(start, pos - start);
    std::string lower;
    for (char c : token)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    if (lower == "null" || lower == "nil") return Value();
    if (lower == "true")                   return Value(true);
    if (lower == "false")                  return Value(false);
    if (isInteger(token))                  return Value(parseLongLongStrict(token, "JSON", "integer literal"));
    if (isFloat(token))                    return Value(parseDoubleStrict(token, "JSON", "float literal"));
    return Value(std::move(token));
}

Value parseJsonArray(const std::string& text, size_t& pos) {
    if (pos >= text.size() || text[pos] != '[')
        throw std::runtime_error("Invalid JSON array");
    ++pos;
    skipWhitespace(text, pos);
    List result;
    if (pos < text.size() && text[pos] == ']') { ++pos; return Value(std::move(result)); }

    while (pos < text.size()) {
        result.push_back(parseJsonValue(text, pos));
        skipWhitespace(text, pos);
        if (pos >= text.size()) break;
        if (text[pos] == ']')  { ++pos; break; }
        if (text[pos] != ',')  throw std::runtime_error("Invalid JSON array delimiter");
        ++pos;
        skipWhitespace(text, pos);
    }
    return Value(std::move(result));
}

Value parseJsonObject(const std::string& text, size_t& pos) {
    if (pos >= text.size() || text[pos] != '{')
        throw std::runtime_error("Invalid JSON object");
    ++pos;
    skipWhitespace(text, pos);
    Map result;
    if (pos < text.size() && text[pos] == '}') { ++pos; return Value(std::move(result)); }

    while (pos < text.size()) {
        skipWhitespace(text, pos);
        if (pos >= text.size() || text[pos] != '"')
            throw std::runtime_error("Invalid JSON object key");

        Value keyValue = parseJsonString(text, pos);
        std::string key = std::get<std::string>(keyValue.data);

        skipWhitespace(text, pos);
        if (pos >= text.size() || text[pos] != ':')
            throw std::runtime_error("Invalid JSON object syntax");
        ++pos;
        skipWhitespace(text, pos);
        result.emplace(std::move(key), parseJsonValue(text, pos));
        skipWhitespace(text, pos);

        if (pos >= text.size()) break;
        if (text[pos] == '}')  { ++pos; break; }
        if (text[pos] != ',')  throw std::runtime_error("Invalid JSON object delimiter");
        ++pos;
        skipWhitespace(text, pos);
    }
    return Value(std::move(result));
}

Value parseJsonValue(const std::string& text, size_t& pos) {
    skipWhitespace(text, pos);
    if (pos >= text.size()) throw std::runtime_error("Unexpected end of JSON");

    char c = text[pos];
    if (c == '{') return parseJsonObject(text, pos);
    if (c == '[') return parseJsonArray(text, pos);
    if (c == '"') return parseJsonString(text, pos);
    // Fallback: unquoted token — handles true/false/null, numbers, _ wildcard, bare identifiers
    return parseJsonLiteral(text, pos);
}

// ---- Code block parser -----------------------------------------------------
// Splits "(instr1 ; instr2 ; ...)" into a Code value.
// Semicolons at depth 0 separate instructions; nesting is tracked.

std::vector<std::string> splitCodeStatements(const std::string& s) {
    std::vector<std::string> parts;
    std::string current;
    int depth = 0;
    bool inStr = false;
    char quoteChar = '\0';
    bool escaped = false;
    bool inComment = false;

    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];

        if (inComment) {
            if (c == '\n') {
                parts.push_back(current);
                current.clear();
                inComment = false;
            } else {
                current.push_back(c);
            }
            continue;
        }

        if (escaped) { current.push_back(c); escaped = false; continue; }
        if (inStr) {
            current.push_back(c);
            if (c == '\\') escaped = true;
            else if (c == quoteChar) inStr = false;
            continue;
        }
        if (c == '"' || c == '\'') {
            inStr = true;
            quoteChar = c;
            current.push_back(c);
            continue;
        }
        if (c == '#') {
            inComment = true;
            current.push_back(c);
            continue;
        }
        if (c == '(' || c == '[' || c == '{') { depth++; current.push_back(c); continue; }
        if (c == ')' || c == ']' || c == '}') {
            if (depth > 0) depth--;
            current.push_back(c);
            continue;
        }
        if ((c == ';' || c == '\n') && depth == 0) {
            parts.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) parts.push_back(current);
    return parts;
}

bool isLabelOnlyStatement(const std::string& statement) {
    auto tokens = Lexer::tokenize(statement);
    return tokens.size() == 1 && !tokens[0].empty() && tokens[0].back() == ':';
}

Code parseInstructionsFromSource(const std::string& source) {
    Code result;
    for (const auto& part : splitCodeStatements(source)) {
        std::string piece = trimStr(part);
        if (piece.empty() || isLabelOnlyStatement(piece)) continue;
        auto ins = Parser::parse(piece, 0);
        if (!ins.opcode.empty())
            result.push_back(ins);
    }
    return result;
}

Value parseCodeBlockToken(const std::string& token) {
    // token is "(MOVE 10 ; SET \"hello\" ; PRINT)"
    if (token.size() < 2 || token.front() != '(' || token.back() != ')')
        throw std::runtime_error("Invalid code block token");
    std::string inner = token.substr(1, token.size() - 2);
    return Value(parseInstructionsFromSource(inner));
}

Code parseCodeSource(const std::string& source) {
    std::string text = trimStr(source);
    if (text.size() >= 2 && text.front() == '(' && text.back() == ')') {
        Value parsed = parseCodeBlockToken(text);
        return std::get<Code>(parsed.data);
    }
    return parseInstructionsFromSource(text);
}

// ---- Token → Value ---------------------------------------------------------

Value parseValue(const std::string& token) {
    if (token.empty()) return Value();

    // Code block literal
    if (token.front() == '(')
        return parseCodeBlockToken(token);

    std::string lower;
    for (char c : token)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    if (lower == "null" || lower == "nil") return Value();
    if (lower == "true")                   return Value(true);
    if (lower == "false")                  return Value(false);

    if (token.size() >= 2 && token.front() == '"' && token.back() == '"')
        return Value(unescapeString(token.substr(1, token.size() - 2)));

    if (token.size() >= 2 && token.front() == '\'' && token.back() == '\'') {
        std::string inner = unescapeString(token.substr(1, token.size() - 2));
        if (inner.size() == 1) return Value(inner[0]);
        return Value(std::move(inner));
    }

    if (token.front() == '{' || token.front() == '[') {
        size_t pos = 0;
        return parseJsonValue(token, pos);
    }

    if (isInteger(token)) {
        try { return Value(parseLongLongStrict(token, "SET", "integer literal")); } catch (...) {}
    }
    if (isFloat(token)) {
        try { return Value(parseDoubleStrict(token, "SET", "float literal")); } catch (...) {}
    }

    return Value(token);
}

// ---- Tape-aware operand resolver -------------------------------------------
// "@N"  → read value from cell N of the given tape (tape reference)
// else  → fall through to parseValue (literal)

Value resolveOperand(const std::string& arg, Tape& tape) {
    if (arg.size() > 1 && arg[0] == '@') {
        std::string rest = arg.substr(1);
        if (isInteger(rest))
            return tape.cells[parseLongLongStrict(rest, "cell reference", "cell index")];
    }
    return parseValue(arg);
}

long long integerValue(const Value& value, const std::string& opcode, const std::string& role) {
    switch (value.kind()) {
        case ValueKind::Int:
            return std::get<long long>(value.data);
        case ValueKind::Float:
            return static_cast<long long>(std::get<double>(value.data));
        case ValueKind::Bool:
            return std::get<bool>(value.data) ? 1 : 0;
        case ValueKind::Char:
            return static_cast<long long>(std::get<char>(value.data));
        case ValueKind::Str: {
            const auto& s = std::get<std::string>(value.data);
            if (isInteger(s)) return parseLongLongStrict(s, opcode, role);
            if (isFloat(s))   return static_cast<long long>(parseDoubleStrict(s, opcode, role));
            break;
        }
        default:
            break;
    }
    throw std::runtime_error(opcode + ": " + role + " must be numeric");
}

long long integerOperand(const std::string& arg, Tape& tape, const std::string& opcode, const std::string& role) {
    return integerValue(resolveOperand(arg, tape), opcode, role);
}

int intOperand(const std::string& arg, Tape& tape, const std::string& opcode, const std::string& role) {
    long long value = integerOperand(arg, tape, opcode, role);
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        throw std::runtime_error(opcode + ": " + role + " is out of int range");
    return static_cast<int>(value);
}

size_t sizeOperand(const std::string& arg, Tape& tape, const std::string& opcode, const std::string& role) {
    long long value = integerOperand(arg, tape, opcode, role);
    if (value < 0)
        throw std::runtime_error(opcode + ": " + role + " must be non-negative");
    return static_cast<size_t>(value);
}

double doubleValue(const Value& value, const std::string& opcode, const std::string& role) {
    switch (value.kind()) {
        case ValueKind::Int:
            return static_cast<double>(std::get<long long>(value.data));
        case ValueKind::Float:
            return std::get<double>(value.data);
        case ValueKind::Bool:
            return std::get<bool>(value.data) ? 1.0 : 0.0;
        case ValueKind::Char:
            return static_cast<double>(std::get<char>(value.data));
        case ValueKind::Str: {
            const auto& s = std::get<std::string>(value.data);
            if (isInteger(s) || isFloat(s))
                return parseDoubleStrict(s, opcode, role);
            break;
        }
        default:
            break;
    }
    throw std::runtime_error(opcode + ": " + role + " must be numeric");
}

double doubleOperand(const std::string& arg, Tape& tape, const std::string& opcode, const std::string& role) {
    return doubleValue(resolveOperand(arg, tape), opcode, role);
}

std::string stringOperand(const std::string& arg, Tape& tape) {
    return stringifyValue(resolveOperand(arg, tape));
}

int checkedTapeIndex(long long raw, size_t tapeCount, const std::string& opcode) {
    if (raw < 0 || raw >= static_cast<long long>(tapeCount))
        throw std::runtime_error(opcode + ": tape index out of bounds: " + std::to_string(raw));
    return static_cast<int>(raw);
}

int tapeIndexArg(const Instruction& ins, size_t argIndex, Tape& tape, size_t tapeCount) {
    return checkedTapeIndex(integerOperand(argOrThrow(ins, argIndex), tape, ins.opcode, "tape index"),
                            tapeCount, ins.opcode);
}

long long cellIndexArgOrPtr(const Instruction& ins, size_t argIndex, Tape& tape, const Tape& targetTape) {
    if (ins.args.size() <= argIndex)
        return targetTape.ptr;
    return integerOperand(argOrThrow(ins, argIndex), tape, ins.opcode, "cell index");
}

// ---- Numeric coercion (for arithmetic / ML) --------------------------------

double numericValue(const Value& v) {
    switch (v.kind()) {
        case ValueKind::Int:   return static_cast<double>(std::get<long long>(v.data));
        case ValueKind::Float: return std::get<double>(v.data);
        case ValueKind::Bool:  return std::get<bool>(v.data) ? 1.0 : 0.0;
        case ValueKind::Char:  return static_cast<double>(std::get<char>(v.data));
        case ValueKind::Str: {
            const auto& s = std::get<std::string>(v.data);
            if (isInteger(s) || isFloat(s)) return parseDoubleStrict(s, "numeric coercion", "string value");
            return 0.0;
        }
        default: return 0.0;
    }
}

// ---- Truthiness ------------------------------------------------------------

bool isTruthy(const Value& v) {
    switch (v.kind()) {
        case ValueKind::Nil:   return false;
        case ValueKind::Bool:  return std::get<bool>(v.data);
        case ValueKind::Int:   return std::get<long long>(v.data) != 0;
        case ValueKind::Float: return std::get<double>(v.data) != 0.0;
        case ValueKind::Char:  return std::get<char>(v.data) != '\0';
        case ValueKind::Str:   return !std::get<std::string>(v.data).empty();
        case ValueKind::List:  return !std::get<List>(v.data).empty();
        case ValueKind::Map:   return !std::get<Map>(v.data).empty();
        case ValueKind::Code:  return !std::get<Code>(v.data).empty();
    }
    return false;
}

// ---- Stringify -------------------------------------------------------------

std::string quoteString(const std::string& value) {
    std::string out = "\"";
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            default:   out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

std::string stringifyJsonValue(const Value& value);

std::string stringifyJsonValue(const Value& value) {
    switch (value.kind()) {
        case ValueKind::Nil:   return "null";
        case ValueKind::Bool:  return std::get<bool>(value.data) ? "true" : "false";
        case ValueKind::Int:   return std::to_string(std::get<long long>(value.data));
        case ValueKind::Float: {
            std::ostringstream oss;
            oss << std::defaultfloat << std::get<double>(value.data);
            return oss.str();
        }
        case ValueKind::Char:  return quoteString(std::string(1, std::get<char>(value.data)));
        case ValueKind::Str:   return quoteString(std::get<std::string>(value.data));
        case ValueKind::List: {
            const List& list = std::get<List>(value.data);
            std::string out = "[";
            bool first = true;
            for (const auto& item : list) {
                if (!first) out += ",";
                first = false;
                out += stringifyJsonValue(item);
            }
            out += "]";
            return out;
        }
        case ValueKind::Map: {
            const Map& map = std::get<Map>(value.data);
            std::string out = "{";
            bool first = true;
            for (const auto& [k, v] : map) {
                if (!first) out += ",";
                first = false;
                out += quoteString(k);
                out += ':';
                out += stringifyJsonValue(v);
            }
            out += "}";
            return out;
        }
        case ValueKind::Code: {
            // Represent as array-of-arrays: [["OPCODE","arg1",...], ...]
            const Code& code = std::get<Code>(value.data);
            std::string out = "[";
            bool first = true;
            for (const auto& ins : code) {
                if (!first) out += ",";
                first = false;
                out += "[" + quoteString(ins.opcode);
                for (const auto& arg : ins.args)
                    out += "," + quoteString(arg);
                out += "]";
            }
            out += "]";
            return out;
        }
    }
    return "";
}

std::string stringifyValue(const Value& value) {
    switch (value.kind()) {
        case ValueKind::Str:   return std::get<std::string>(value.data);
        case ValueKind::Char:  return std::string(1, std::get<char>(value.data));
        case ValueKind::Bool:  return std::get<bool>(value.data) ? "true" : "false";
        case ValueKind::Int:   return std::to_string(std::get<long long>(value.data));
        case ValueKind::Float: {
            std::ostringstream oss;
            oss << std::defaultfloat << std::get<double>(value.data);
            return oss.str();
        }
        case ValueKind::Code: {
            // Human-readable: (OPCODE arg1 arg2 ; OPCODE2 ...)
            const Code& code = std::get<Code>(value.data);
            std::string out = "(";
            bool first = true;
            for (const auto& ins : code) {
                if (!first) out += " ; ";
                first = false;
                out += ins.opcode;
                for (const auto& arg : ins.args)
                    out += " " + arg;
            }
            out += ")";
            return out;
        }
        default: return stringifyJsonValue(value);
    }
}

std::string compactDisplayValue(const Value& value, size_t maxWidth = 14) {
    std::string out = stringifyValue(value);
    for (char& c : out) {
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
    }

    if (out.size() <= maxWidth)
        return out;
    if (maxWidth <= 3)
        return out.substr(0, maxWidth);
    return out.substr(0, maxWidth - 3) + "...";
}

bool ansiEnabled() {
    static bool enabled = [] {
        const char* term = std::getenv("TERM");
        return isatty(STDOUT_FILENO)
            && std::getenv("NO_COLOR") == nullptr
            && (!term || std::string(term) != "dumb");
    }();
    return enabled;
}

std::string ansiWrap(const std::string& text, const std::string& code) {
    if (!ansiEnabled() || text.empty()) return text;
    return "\033[" + code + "m" + text + "\033[0m";
}

std::string valueColor(ValueKind kind) {
    switch (kind) {
        case ValueKind::Nil:   return "90";
        case ValueKind::Bool:  return "35";
        case ValueKind::Int:   return "36";
        case ValueKind::Float: return "36";
        case ValueKind::Char:  return "32";
        case ValueKind::Str:   return "32";
        case ValueKind::List:  return "33";
        case ValueKind::Map:   return "33";
        case ValueKind::Code:  return "34";
    }
    return "0";
}

std::string colorValue(const Value& value, const std::string& text) {
    return ansiWrap(text, valueColor(value.kind()));
}

std::string colorHead(const std::string& text) {
    return ansiWrap(text, "1;31");
}

std::string colorMeta(const std::string& text) {
    return ansiWrap(text, "90");
}

std::string formatInstruction(const Instruction& ins) {
    std::string out = ins.opcode;
    for (const auto& arg : ins.args)
        out += " " + arg;
    return out;
}

// ---- Optional PostgreSQL/libpq bridge --------------------------------------

struct PgApi {
    bool attempted = false;
    void* handle = nullptr;

    using PGconn = void;
    using PGresult = void;

    PGconn* (*PQconnectdb)(const char*) = nullptr;
    int (*PQstatus)(const PGconn*) = nullptr;
    char* (*PQerrorMessage)(const PGconn*) = nullptr;
    void (*PQfinish)(PGconn*) = nullptr;
    PGresult* (*PQexec)(PGconn*, const char*) = nullptr;
    int (*PQresultStatus)(const PGresult*) = nullptr;
    char* (*PQresStatus)(int) = nullptr;
    int (*PQntuples)(const PGresult*) = nullptr;
    int (*PQnfields)(const PGresult*) = nullptr;
    char* (*PQfname)(const PGresult*, int) = nullptr;
    char* (*PQgetvalue)(const PGresult*, int, int) = nullptr;
    int (*PQgetisnull)(const PGresult*, int, int) = nullptr;
    char* (*PQcmdTuples)(PGresult*) = nullptr;
    char* (*PQescapeLiteral)(PGconn*, const char*, size_t) = nullptr;
    char* (*PQescapeIdentifier)(PGconn*, const char*, size_t) = nullptr;
    void (*PQfreemem)(void*) = nullptr;
    void (*PQclear)(PGresult*) = nullptr;

    ~PgApi() {
        if (handle)
            dlclose(handle);
    }

    template <typename T>
    bool loadSymbol(T& target, const char* name, std::string& error) {
        target = reinterpret_cast<T>(dlsym(handle, name));
        if (!target) {
            error = std::string("missing libpq symbol: ") + name;
            return false;
        }
        return true;
    }

    bool load(std::string& error) {
        if (handle) return true;
        if (attempted) {
            error = "libpq is not available; install PostgreSQL client libraries";
            return false;
        }
        attempted = true;

        handle = dlopen("libpq.so.5", RTLD_NOW | RTLD_LOCAL);
        if (!handle)
            handle = dlopen("libpq.so", RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            error = "libpq is not available; install PostgreSQL client libraries";
            return false;
        }

        bool ok = loadSymbol(PQconnectdb, "PQconnectdb", error)
            && loadSymbol(PQstatus, "PQstatus", error)
            && loadSymbol(PQerrorMessage, "PQerrorMessage", error)
            && loadSymbol(PQfinish, "PQfinish", error)
            && loadSymbol(PQexec, "PQexec", error)
            && loadSymbol(PQresultStatus, "PQresultStatus", error)
            && loadSymbol(PQresStatus, "PQresStatus", error)
            && loadSymbol(PQntuples, "PQntuples", error)
            && loadSymbol(PQnfields, "PQnfields", error)
            && loadSymbol(PQfname, "PQfname", error)
            && loadSymbol(PQgetvalue, "PQgetvalue", error)
            && loadSymbol(PQgetisnull, "PQgetisnull", error)
            && loadSymbol(PQcmdTuples, "PQcmdTuples", error)
            && loadSymbol(PQescapeLiteral, "PQescapeLiteral", error)
            && loadSymbol(PQescapeIdentifier, "PQescapeIdentifier", error)
            && loadSymbol(PQfreemem, "PQfreemem", error)
            && loadSymbol(PQclear, "PQclear", error);
        if (!ok) {
            dlclose(handle);
            handle = nullptr;
        }
        return ok;
    }
};

PgApi& pgApi() {
    static PgApi api;
    return api;
}

PgApi& requirePgApi() {
    std::string error;
    PgApi& api = pgApi();
    if (!api.load(error))
        throw std::runtime_error("PostgreSQL support unavailable: " + error);
    return api;
}

std::string pgError(PgApi& api, void* conn) {
    if (!conn) return "not connected";
    char* msg = api.PQerrorMessage(static_cast<PgApi::PGconn*>(conn));
    return msg ? trimStr(msg) : "";
}

void requireDbConnected(void* conn) {
    if (!conn)
        throw std::runtime_error("DB: not connected; call DBCONNECT first");
}

std::string sqlFromInstructionOrCell(const Instruction& ins, Tape& tape) {
    if (ins.args.empty())
        return stringifyValue(tape.current());
    return stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
}

std::string pgResultStatusName(PgApi& api, int status) {
    char* name = api.PQresStatus(status);
    return name ? std::string(name) : std::to_string(status);
}

Value pgRowsValue(PgApi& api, PgApi::PGresult* result) {
    int rowCount = api.PQntuples(result);
    int fieldCount = api.PQnfields(result);

    List rows;
    rows.reserve(static_cast<size_t>(rowCount));
    for (int row = 0; row < rowCount; ++row) {
        Map item;
        for (int col = 0; col < fieldCount; ++col) {
            char* name = api.PQfname(result, col);
            std::string key = name ? std::string(name) : ("column_" + std::to_string(col));
            if (api.PQgetisnull(result, row, col)) {
                item[key] = Value();
            } else {
                char* raw = api.PQgetvalue(result, row, col);
                item[key] = Value(std::string(raw ? raw : ""));
            }
        }
        rows.emplace_back(std::move(item));
    }

    return Value(std::move(rows));
}

Value pgQueryMap(PgApi& api, PgApi::PGresult* result, int status) {
    Map out;
    out["ok"] = Value(status == 1 || status == 2);
    out["status"] = Value(pgResultStatusName(api, status));
    out["row_count"] = Value(static_cast<long long>(api.PQntuples(result)));
    out["field_count"] = Value(static_cast<long long>(api.PQnfields(result)));

    char* cmdTuples = api.PQcmdTuples(result);
    out["rows_affected"] = Value(std::string(cmdTuples ? cmdTuples : ""));

    if (status == 2)
        out["rows"] = pgRowsValue(api, result);

    return Value(std::move(out));
}

Value pgRunSql(PgApi& api, void* conn, const std::string& sql, bool rowsOnly, const std::string& opcode) {
    PgApi::PGresult* result = api.PQexec(static_cast<PgApi::PGconn*>(conn), sql.c_str());
    if (!result)
        throw std::runtime_error(opcode + ": " + pgError(api, conn));

    int status = api.PQresultStatus(result);
    bool ok = status == 1 || status == 2; // PGRES_COMMAND_OK or PGRES_TUPLES_OK
    if (!ok) {
        std::string error = pgError(api, conn);
        api.PQclear(result);
        throw std::runtime_error(opcode + ": " + error);
    }

    Value out;
    if (rowsOnly) {
        if (status != 2) {
            api.PQclear(result);
            throw std::runtime_error(opcode + ": SQL did not return rows");
        }
        out = pgRowsValue(api, result);
    } else {
        out = pgQueryMap(api, result, status);
    }

    api.PQclear(result);
    return out;
}

std::string pgQuoteLiteral(PgApi& api, void* conn, const std::string& raw) {
    char* escaped = api.PQescapeLiteral(static_cast<PgApi::PGconn*>(conn), raw.c_str(), raw.size());
    if (!escaped)
        throw std::runtime_error("DB: failed to quote SQL literal: " + pgError(api, conn));
    std::string out(escaped);
    api.PQfreemem(escaped);
    return out;
}

std::string pgQuoteIdentifier(PgApi& api, void* conn, const std::string& raw) {
    char* escaped = api.PQescapeIdentifier(static_cast<PgApi::PGconn*>(conn), raw.c_str(), raw.size());
    if (!escaped)
        throw std::runtime_error("DB: failed to quote SQL identifier: " + pgError(api, conn));
    std::string out(escaped);
    api.PQfreemem(escaped);
    return out;
}

std::string pgValueLiteral(PgApi& api, void* conn, const Value& value) {
    switch (value.kind()) {
        case ValueKind::Nil:
            return "NULL";
        case ValueKind::Bool:
            return std::get<bool>(value.data) ? "TRUE" : "FALSE";
        case ValueKind::Int:
            return std::to_string(std::get<long long>(value.data));
        case ValueKind::Float: {
            std::ostringstream out;
            out << std::setprecision(17) << std::get<double>(value.data);
            return out.str();
        }
        case ValueKind::Char:
        case ValueKind::Str:
            return pgQuoteLiteral(api, conn, stringifyValue(value));
        case ValueKind::List:
        case ValueKind::Map:
        case ValueKind::Code:
            return pgQuoteLiteral(api, conn, stringifyJsonValue(value));
    }
    return "NULL";
}

const Map& requireMapValue(const Value& value, const std::string& opcode) {
    if (value.kind() != ValueKind::Map)
        throw std::runtime_error(opcode + ": value must be a map/object");
    return std::get<Map>(value.data);
}

Value dbMapArgOrCurrent(const Instruction& ins, size_t index, Tape& tape, const std::string& opcode) {
    if (ins.args.size() <= index)
        return tape.current();
    return resolveOperand(argOrThrow(ins, index), tape);
}

std::string buildInsertSql(PgApi& api, void* conn, const std::string& table, const Map& values) {
    if (values.empty())
        throw std::runtime_error("DBINSERT: map/object must not be empty");

    std::string columns;
    std::string literals;
    bool first = true;
    for (const auto& [key, value] : values) {
        if (!first) {
            columns += ", ";
            literals += ", ";
        }
        first = false;
        columns += pgQuoteIdentifier(api, conn, key);
        literals += pgValueLiteral(api, conn, value);
    }

    return "INSERT INTO " + pgQuoteIdentifier(api, conn, table) +
           " (" + columns + ") VALUES (" + literals + ")";
}

std::string buildUpdateSql(PgApi& api, void* conn, const std::string& table, const Map& values, const std::string& where) {
    if (values.empty())
        throw std::runtime_error("DBUPDATE: map/object must not be empty");
    if (trimStr(where).empty())
        throw std::runtime_error("DBUPDATE: WHERE clause is required");

    std::string assignments;
    bool first = true;
    for (const auto& [key, value] : values) {
        if (!first)
            assignments += ", ";
        first = false;
        assignments += pgQuoteIdentifier(api, conn, key) + " = " + pgValueLiteral(api, conn, value);
    }

    return "UPDATE " + pgQuoteIdentifier(api, conn, table) +
           " SET " + assignments + " WHERE " + where;
}

long long pgCommandRows(PgApi& api, void* conn, const std::string& sql, const std::string& opcode) {
    PgApi::PGresult* result = api.PQexec(static_cast<PgApi::PGconn*>(conn), sql.c_str());
    if (!result)
        throw std::runtime_error(opcode + ": " + pgError(api, conn));

    int status = api.PQresultStatus(result);
    if (status != 1) { // PGRES_COMMAND_OK
        std::string error = pgError(api, conn);
        api.PQclear(result);
        throw std::runtime_error(opcode + ": " + error);
    }

    char* rawRows = api.PQcmdTuples(result);
    long long rows = 0;
    if (rawRows && *rawRows) {
        try { rows = std::stoll(rawRows); } catch (...) { rows = 0; }
    }
    api.PQclear(result);
    return rows;
}

std::string sqlNull() {
    return "NULL";
}

std::string sqlFloatLiteral(double value) {
    if (!std::isfinite(value))
        throw std::runtime_error("DB_TAPE_INPUT: float values must be finite");
    std::ostringstream out;
    out << std::setprecision(17) << value;
    return out.str();
}

std::string sqlTextOrNull(PgApi& api, void* conn, const std::string& value) {
    return pgQuoteLiteral(api, conn, value);
}

struct TapeCellSql {
    std::string kind;
    std::string boolValue = sqlNull();
    std::string intValue = sqlNull();
    std::string floatValue = sqlNull();
    std::string charValue = sqlNull();
    std::string textValue = sqlNull();
    std::string codeText = sqlNull();
};

std::string pgColumnText(PgApi& api, PgApi::PGresult* result, int row, int col);

TapeCellSql tapeCellSql(PgApi& api, void* conn, const Value& value, const std::string& opcode) {
    TapeCellSql out;
    out.kind = pgQuoteLiteral(api, conn, kindName(value.kind()));

    switch (value.kind()) {
        case ValueKind::Nil:
            break;
        case ValueKind::Bool:
            out.boolValue = std::get<bool>(value.data) ? "TRUE" : "FALSE";
            break;
        case ValueKind::Int:
            out.intValue = std::to_string(std::get<long long>(value.data));
            break;
        case ValueKind::Float:
            out.floatValue = sqlFloatLiteral(std::get<double>(value.data));
            break;
        case ValueKind::Char:
            out.charValue = sqlTextOrNull(api, conn, std::string(1, std::get<char>(value.data)));
            break;
        case ValueKind::Str:
            out.textValue = sqlTextOrNull(api, conn, std::get<std::string>(value.data));
            break;
        case ValueKind::Code:
            out.codeText = sqlTextOrNull(api, conn, stringifyValue(value));
            break;
        case ValueKind::List:
        case ValueKind::Map:
            throw std::runtime_error(opcode + ": list/map cells are not stored as JSON; encode nested data across tape cells");
    }

    return out;
}

std::string buildTapeCellUpsertSql(
    PgApi& api,
    void* conn,
    const std::string& spaceKey,
    long long tapeIndex,
    long long cellIndex,
    const Value& value,
    const std::string& opcode
) {
    if (tapeIndex < 0)
        throw std::runtime_error(opcode + ": tape index must be non-negative");
    TapeCellSql cell = tapeCellSql(api, conn, value, opcode);

    return "INSERT INTO tape_cells "
           "(space_key, tape_index, cell_index, value_kind, bool_value, int_value, float_value, char_value, text_value, code_text, updated_at) VALUES (" +
           pgQuoteLiteral(api, conn, spaceKey) + ", " +
           std::to_string(tapeIndex) + ", " +
           std::to_string(cellIndex) + ", " +
           cell.kind + ", " +
           cell.boolValue + ", " +
           cell.intValue + ", " +
           cell.floatValue + ", " +
           cell.charValue + ", " +
           cell.textValue + ", " +
           cell.codeText + ", now()) "
           "ON CONFLICT (space_key, tape_index, cell_index) DO UPDATE SET "
           "value_kind = EXCLUDED.value_kind, "
           "bool_value = EXCLUDED.bool_value, "
           "int_value = EXCLUDED.int_value, "
           "float_value = EXCLUDED.float_value, "
           "char_value = EXCLUDED.char_value, "
           "text_value = EXCLUDED.text_value, "
           "code_text = EXCLUDED.code_text, "
           "updated_at = now()";
}

std::string tapeCellValueWhereSql(
    PgApi& api,
    void* conn,
    const Value& value,
    const std::string& opcode
) {
    switch (value.kind()) {
        case ValueKind::Nil:
            return "value_kind = 'nil'";
        case ValueKind::Bool:
            return std::string("value_kind = 'bool' AND bool_value IS ") +
                   (std::get<bool>(value.data) ? "TRUE" : "FALSE");
        case ValueKind::Int:
            return "value_kind = 'int' AND int_value = " + std::to_string(std::get<long long>(value.data));
        case ValueKind::Float:
            return "value_kind = 'float' AND float_value = " + sqlFloatLiteral(std::get<double>(value.data));
        case ValueKind::Char:
            return "value_kind = 'char' AND char_value = " +
                   pgQuoteLiteral(api, conn, std::string(1, std::get<char>(value.data)));
        case ValueKind::Str:
            return "value_kind = 'str' AND text_value = " +
                   pgQuoteLiteral(api, conn, std::get<std::string>(value.data));
        case ValueKind::Code:
            return "value_kind = 'code' AND code_text = " +
                   pgQuoteLiteral(api, conn, stringifyValue(value));
        case ValueKind::List:
        case ValueKind::Map:
            throw std::runtime_error(opcode + ": list/map cells are not searchable in tape_cells");
    }
    return "FALSE";
}

Value pgFindTapeSpace(
    PgApi& api,
    void* conn,
    long long tapeIndex,
    long long cellIndex,
    const Value& value,
    const std::string& opcode
) {
    if (tapeIndex < 0)
        throw std::runtime_error(opcode + ": tape index must be non-negative");

    std::string sql =
        "SELECT space_key FROM tape_cells WHERE tape_index = " +
        std::to_string(tapeIndex) +
        " AND cell_index = " +
        std::to_string(cellIndex) +
        " AND " +
        tapeCellValueWhereSql(api, conn, value, opcode) +
        " ORDER BY updated_at DESC LIMIT 1";

    PgApi::PGresult* result = api.PQexec(static_cast<PgApi::PGconn*>(conn), sql.c_str());
    if (!result)
        throw std::runtime_error(opcode + ": " + pgError(api, conn));

    int status = api.PQresultStatus(result);
    if (status != 2) {
        std::string error = pgError(api, conn);
        api.PQclear(result);
        throw std::runtime_error(opcode + ": " + error);
    }

    Value out;
    if (api.PQntuples(result) > 0)
        out = Value(pgColumnText(api, result, 0, 0));
    api.PQclear(result);
    return out;
}

void pgOpenTapeSpace(PgApi& api, void* conn, const std::string& spaceKey, const std::string& kind, const std::string& opcode) {
    std::string sql =
        "INSERT INTO tape_spaces (space_key, kind, updated_at) VALUES (" +
        pgQuoteLiteral(api, conn, spaceKey) + ", " +
        pgQuoteLiteral(api, conn, kind) + ", now()) "
        "ON CONFLICT (space_key) DO UPDATE SET kind = EXCLUDED.kind, updated_at = now()";
    pgCommandRows(api, conn, sql, opcode);
}

void pgEnsureTapeSpace(PgApi& api, void* conn, const std::string& spaceKey, const std::string& opcode) {
    std::string sql =
        "INSERT INTO tape_spaces (space_key, kind, updated_at) VALUES (" +
        pgQuoteLiteral(api, conn, spaceKey) + ", 'memory', now()) "
        "ON CONFLICT (space_key) DO NOTHING";
    pgCommandRows(api, conn, sql, opcode);
}

void pgStoreTapeCell(
    PgApi& api,
    void* conn,
    const std::string& spaceKey,
    long long tapeIndex,
    long long cellIndex,
    const Value& value,
    const std::string& opcode
) {
    pgEnsureTapeSpace(api, conn, spaceKey, opcode);
    pgCommandRows(api, conn, buildTapeCellUpsertSql(api, conn, spaceKey, tapeIndex, cellIndex, value, opcode), opcode);
}

std::string pgColumnText(PgApi& api, PgApi::PGresult* result, int row, int col) {
    if (api.PQgetisnull(result, row, col))
        return "";
    char* raw = api.PQgetvalue(result, row, col);
    return raw ? std::string(raw) : "";
}

Value pgTapeCellValue(PgApi& api, PgApi::PGresult* result, int row) {
    std::string kind = pgColumnText(api, result, row, 0);

    if (kind == "nil") return Value();
    if (kind == "bool") {
        std::string raw = pgColumnText(api, result, row, 1);
        return Value(raw == "t" || raw == "true" || raw == "1");
    }
    if (kind == "int") {
        std::string raw = pgColumnText(api, result, row, 2);
        return raw.empty() ? Value(0LL) : Value(parseLongLongStrict(raw, "DB_TAPE_OUTPUT", "stored int_value"));
    }
    if (kind == "float") {
        std::string raw = pgColumnText(api, result, row, 3);
        return raw.empty() ? Value(0.0) : Value(parseDoubleStrict(raw, "DB_TAPE_OUTPUT", "stored float_value"));
    }
    if (kind == "char") {
        std::string raw = pgColumnText(api, result, row, 4);
        return raw.empty() ? Value('\0') : Value(raw[0]);
    }
    if (kind == "str") {
        return Value(pgColumnText(api, result, row, 5));
    }
    if (kind == "code") {
        std::string raw = pgColumnText(api, result, row, 6);
        return parseValue(raw);
    }

    throw std::runtime_error("DB_TAPE_OUTPUT: unknown stored value_kind: " + kind);
}

bool pgFetchTapeCell(
    PgApi& api,
    void* conn,
    const std::string& spaceKey,
    long long tapeIndex,
    long long cellIndex,
    Value& out,
    const std::string& opcode
) {
    std::string sql =
        "SELECT value_kind, bool_value::text, int_value::text, float_value::text, char_value, text_value, code_text "
        "FROM tape_cells WHERE space_key = " + pgQuoteLiteral(api, conn, spaceKey) +
        " AND tape_index = " + std::to_string(tapeIndex) +
        " AND cell_index = " + std::to_string(cellIndex) +
        " LIMIT 1";

    PgApi::PGresult* result = api.PQexec(static_cast<PgApi::PGconn*>(conn), sql.c_str());
    if (!result)
        throw std::runtime_error(opcode + ": " + pgError(api, conn));

    int status = api.PQresultStatus(result);
    if (status != 2) { // PGRES_TUPLES_OK
        std::string error = pgError(api, conn);
        api.PQclear(result);
        throw std::runtime_error(opcode + ": " + error);
    }

    bool found = api.PQntuples(result) > 0;
    if (found)
        out = pgTapeCellValue(api, result, 0);
    api.PQclear(result);
    return found;
}

long long pgLoadTapeCells(
    PgApi& api,
    void* conn,
    const std::string& spaceKey,
    const std::string& tapeSelector,
    std::vector<Tape>& tapes,
    const std::function<int(long long, const std::string&)>& ensureTape,
    const std::string& opcode
) {
    std::string sql =
        "SELECT value_kind, bool_value::text, int_value::text, float_value::text, char_value, text_value, code_text, tape_index::text, cell_index::text "
        "FROM tape_cells WHERE space_key = " + pgQuoteLiteral(api, conn, spaceKey);
    if (tapeSelector != "*")
        sql += " AND tape_index = " + tapeSelector;
    sql += " ORDER BY tape_index, cell_index";

    PgApi::PGresult* result = api.PQexec(static_cast<PgApi::PGconn*>(conn), sql.c_str());
    if (!result)
        throw std::runtime_error(opcode + ": " + pgError(api, conn));

    int status = api.PQresultStatus(result);
    if (status != 2) {
        std::string error = pgError(api, conn);
        api.PQclear(result);
        throw std::runtime_error(opcode + ": " + error);
    }

    int rowCount = api.PQntuples(result);
    for (int row = 0; row < rowCount; ++row) {
        long long tapeIndex = parseLongLongStrict(pgColumnText(api, result, row, 7), opcode, "stored tape_index");
        long long cellIndex = parseLongLongStrict(pgColumnText(api, result, row, 8), opcode, "stored cell_index");
        int tapeSlot = ensureTape(tapeIndex, opcode);
        tapes[tapeSlot].cells[cellIndex] = pgTapeCellValue(api, result, row);
    }

    api.PQclear(result);
    return static_cast<long long>(rowCount);
}

long long pgInsertTapeEvent(
    PgApi& api,
    void* conn,
    const std::string& spaceKey,
    const std::string& eventType,
    long long tapeIndex,
    long long cellIndex,
    const std::string& opcodeName,
    const std::string& note,
    const std::string& opcode
) {
    pgEnsureTapeSpace(api, conn, spaceKey, opcode);
    std::string sql =
        "INSERT INTO tape_events (space_key, event_type, tape_index, cell_index, opcode, note) VALUES (" +
        pgQuoteLiteral(api, conn, spaceKey) + ", " +
        pgQuoteLiteral(api, conn, eventType) + ", " +
        std::to_string(tapeIndex) + ", " +
        std::to_string(cellIndex) + ", " +
        pgQuoteLiteral(api, conn, opcodeName) + ", " +
        pgQuoteLiteral(api, conn, note) + ") RETURNING id::text";

    PgApi::PGresult* result = api.PQexec(static_cast<PgApi::PGconn*>(conn), sql.c_str());
    if (!result)
        throw std::runtime_error(opcode + ": " + pgError(api, conn));

    int status = api.PQresultStatus(result);
    if (status != 2 || api.PQntuples(result) < 1) {
        std::string error = pgError(api, conn);
        api.PQclear(result);
        throw std::runtime_error(opcode + ": " + error);
    }

    long long id = parseLongLongStrict(pgColumnText(api, result, 0, 0), opcode, "returned event id");
    api.PQclear(result);
    return id;
}

// ---- Equality (strict type-and-value match) --------------------------------

bool valuesEqual(const Value& a, const Value& b) {
    if (a.kind() != b.kind()) return false;
    switch (a.kind()) {
        case ValueKind::Nil:   return true;
        case ValueKind::Bool:  return std::get<bool>(a.data)       == std::get<bool>(b.data);
        case ValueKind::Int:   return std::get<long long>(a.data)   == std::get<long long>(b.data);
        case ValueKind::Float: return std::get<double>(a.data)      == std::get<double>(b.data);
        case ValueKind::Char:  return std::get<char>(a.data)        == std::get<char>(b.data);
        case ValueKind::Str:   return std::get<std::string>(a.data) == std::get<std::string>(b.data);
        case ValueKind::List:  return std::get<List>(a.data)        == std::get<List>(b.data);
        case ValueKind::Map:   return std::get<Map>(a.data)         == std::get<Map>(b.data);
        case ValueKind::Code:  return std::get<Code>(a.data)        == std::get<Code>(b.data);
    }
    return false;
}

// ---- Pattern matching ------------------------------------------------------
// Pattern language:
//   Str "_"          → wildcard, matches anything
//   Str type-name    → type check ("int", "float", "str", "bool", "char", "list", "map", "nil", "code")
//   Str other        → equality check against Str cell
//   List [p1, p2, …] → structural match (same length, each element matches)
//   Map  {k: p, …}   → each key must exist and its value must match
//   anything else    → strict equality

bool matchPattern(const Value& val, const Value& pattern) {
    if (pattern.kind() == ValueKind::Str) {
        const auto& s = std::get<std::string>(pattern.data);
        if (s == "_")     return true;
        if (s == "nil")   return val.kind() == ValueKind::Nil;
        if (s == "bool")  return val.kind() == ValueKind::Bool;
        if (s == "int")   return val.kind() == ValueKind::Int;
        if (s == "float") return val.kind() == ValueKind::Float;
        if (s == "char")  return val.kind() == ValueKind::Char;
        if (s == "str")   return val.kind() == ValueKind::Str;
        if (s == "list")  return val.kind() == ValueKind::List;
        if (s == "map")   return val.kind() == ValueKind::Map;
        if (s == "code")  return val.kind() == ValueKind::Code;
        // string equality
        if (val.kind() == ValueKind::Str)
            return std::get<std::string>(val.data) == s;
        return false;
    }
    if (pattern.kind() == ValueKind::List) {
        if (val.kind() != ValueKind::List) return false;
        const auto& pl = std::get<List>(pattern.data);
        const auto& vl = std::get<List>(val.data);
        if (pl.size() != vl.size()) return false;
        for (size_t i = 0; i < pl.size(); ++i)
            if (!matchPattern(vl[i], pl[i])) return false;
        return true;
    }
    if (pattern.kind() == ValueKind::Map) {
        if (val.kind() != ValueKind::Map) return false;
        const auto& pm = std::get<Map>(pattern.data);
        const auto& vm = std::get<Map>(val.data);
        for (const auto& [k, vp] : pm) {
            auto it = vm.find(k);
            if (it == vm.end()) return false;
            if (!matchPattern(it->second, vp)) return false;
        }
        return true;
    }
    return valuesEqual(val, pattern);
}

// ---- Argument accessor -----------------------------------------------------

const std::string& argOrThrow(const Instruction& ins, size_t index) {
    if (index >= ins.args.size())
        throw std::runtime_error("Missing argument for instruction: " + ins.opcode);
    return ins.args[index];
}

// ---- Code manipulation helpers ---------------------------------------------

Instruction listToInstruction(const List& lst) {
    if (lst.empty()) throw std::runtime_error("Cannot convert empty list to instruction");
    Instruction ins;
    ins.opcode = toUpperStr(stringifyValue(lst[0]));
    for (size_t i = 1; i < lst.size(); ++i)
        ins.args.push_back(stringifyValue(lst[i]));
    return ins;
}

std::string normalizeJumpLabel(const Instruction& ins) {
    std::string label = argOrThrow(ins, 0);
    if (label.empty() || label[0] != '$')
        throw std::runtime_error(ins.opcode + ": jump labels must use $label syntax, got '" + label + "'");
    return toUpperStr(label);
}

// ---- ML helpers ------------------------------------------------------------

Eigen::VectorXd listToEigenVector(const List& list) {
    Eigen::VectorXd out(static_cast<Eigen::Index>(list.size()));
    for (Eigen::Index i = 0; i < out.size(); ++i)
        out(i) = numericValue(list[static_cast<size_t>(i)]);
    return out;
}

List eigenVectorToList(const Eigen::VectorXd& v) {
    List out;
    out.reserve(static_cast<size_t>(v.size()));
    for (Eigen::Index i = 0; i < v.size(); ++i)
        out.emplace_back(v(i));
    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// VM implementation
// ---------------------------------------------------------------------------

VM::VM(const std::string& file, int tapeCount, bool debug, bool step)
    : tapes(std::max(1, tapeCount)), debugMode(debug), stepMode(step), loader(file)
{}

VM::~VM() {
    if (dbConn) {
        PgApi& api = pgApi();
        if (api.PQfinish)
            api.PQfinish(static_cast<PgApi::PGconn*>(dbConn));
        dbConn = nullptr;
    }
}

int VM::ensureTapeIndex(long long index, const std::string& opcode) {
    if (index < 0)
        throw std::runtime_error(opcode + ": tape index must be non-negative: " + std::to_string(index));
    if (index > static_cast<long long>(std::numeric_limits<int>::max()))
        throw std::runtime_error(opcode + ": tape index is too large: " + std::to_string(index));
    size_t target = static_cast<size_t>(index);
    if (target >= tapes.size())
        tapes.resize(target + 1);
    return static_cast<int>(target);
}

void VM::syncTapeMetadata(int tapeIndex) {
    if (tapeIndex < 0 || tapeIndex >= static_cast<int>(tapes.size()))
        return;

    Tape& tape = tapes[static_cast<size_t>(tapeIndex)];

    auto nameIt = tape.cells.find(TAPE_NAME_CELL);
    if (nameIt == tape.cells.end() || nameIt->second.kind() == ValueKind::Nil) {
        tape.name.clear();
    } else if (nameIt->second.kind() == ValueKind::Str) {
        tape.name = std::get<std::string>(nameIt->second.data);
    } else {
        throw std::runtime_error("TAPEMODULE: tape name cell -2 must be a string");
    }

    auto cacheModuleName = [&](const std::string& moduleName) {
        if (!moduleName.empty())
            tapeNameCache[toUpperStr(moduleName)] = tapeIndex;
    };

    cacheModuleName("tape" + std::to_string(tapeIndex));
    cacheModuleName(tape.name);

    tape.functions.clear();
    auto fnIt = tape.cells.find(TAPE_FUNCTIONS_CELL);
    if (fnIt == tape.cells.end() || fnIt->second.kind() == ValueKind::Nil)
        return;
    if (fnIt->second.kind() != ValueKind::Map)
        throw std::runtime_error("TAPEMODULE: tape function directory cell -1 must be a map");

    const auto& functionMap = std::get<Map>(fnIt->second.data);
    for (const auto& [name, cellValue] : functionMap) {
        long long cell = integerValue(cellValue, "TAPEMODULE", "function cell index");
        std::string fnKey = toUpperStr(name);
        tape.functions[fnKey] = cell;

        tapeFunctionCache[toUpperStr("tape" + std::to_string(tapeIndex) + "." + name)] = {tapeIndex, cell};
        if (!tape.name.empty())
            tapeFunctionCache[toUpperStr(tape.name + "." + name)] = {tapeIndex, cell};
    }
}

void VM::syncAllTapeMetadata() {
    tapeNameCache.clear();
    tapeFunctionCache.clear();
    for (size_t i = 0; i < tapes.size(); ++i)
        syncTapeMetadata(static_cast<int>(i));
}

int VM::resolveTapeSelector(const std::string& selector, const std::string& opcode) {
    std::string value = trimStr(selector);
    if (value.empty())
        throw std::runtime_error(opcode + ": empty tape selector");

    if (isInteger(value))
        return ensureTapeIndex(parseLongLongStrict(value, opcode, "tape index"), opcode);

    std::string upper = toUpperStr(value);
    if (upper.rfind("TAPE", 0) == 0 && value.size() > 4) {
        std::string suffix = value.substr(4);
        if (isInteger(suffix))
            return ensureTapeIndex(parseLongLongStrict(suffix, opcode, "tape index"), opcode);
    }

    syncAllTapeMetadata();
    auto it = tapeNameCache.find(upper);
    if (it != tapeNameCache.end())
        return ensureTapeIndex(it->second, opcode);

    throw std::runtime_error(opcode + ": unknown tape selector: " + selector);
}

bool VM::resolveTapeFunction(const std::string& target, int& tapeIndex, long long& cellIndex) {
    if (target.empty())
        return false;

    syncAllTapeMetadata();

    size_t dot = target.find('.');
    if (dot != std::string::npos) {
        std::string moduleName = target.substr(0, dot);
        std::string functionName = target.substr(dot + 1);
        if (moduleName.empty() || functionName.empty())
            return false;

        std::string cacheKey = toUpperStr(target);
        auto cacheIt = tapeFunctionCache.find(cacheKey);
        if (cacheIt != tapeFunctionCache.end()) {
            tapeIndex = cacheIt->second.first;
            cellIndex = cacheIt->second.second;
            return true;
        }

        tapeIndex = resolveTapeSelector(moduleName, "CALL");
        if (isInteger(functionName)) {
            cellIndex = parseLongLongStrict(functionName, "CALL", "tape function cell");
            return true;
        }

        auto fnIt = tapes[static_cast<size_t>(tapeIndex)].functions.find(toUpperStr(functionName));
        if (fnIt == tapes[static_cast<size_t>(tapeIndex)].functions.end())
            return false;
        cellIndex = fnIt->second;
        tapeFunctionCache[cacheKey] = {tapeIndex, cellIndex};
        return true;
    }

    auto fnIt = tapes[static_cast<size_t>(activeTape)].functions.find(toUpperStr(target));
    if (fnIt != tapes[static_cast<size_t>(activeTape)].functions.end()) {
        tapeIndex = activeTape;
        cellIndex = fnIt->second;
        return true;
    }

    return false;
}

bool VM::callTapeFunction(int tapeIndex, long long cellIndex, const std::string& displayName) {
    ensureTapeIndex(tapeIndex, "CALL");
    auto& moduleTape = tapes[static_cast<size_t>(tapeIndex)];
    auto it = moduleTape.cells.find(cellIndex);
    if (it == moduleTape.cells.end() || it->second.kind() == ValueKind::Nil)
        throw std::runtime_error("CALL: tape function not found in " + displayName +
                                 " at cell " + std::to_string(cellIndex));

    Value functionValue = it->second;
    int previousTape = activeTape;
    long long previousModulePtr = tapes[static_cast<size_t>(tapeIndex)].ptr;
    tapes[static_cast<size_t>(tapeIndex)].ptr = 0;
    activeTape = tapeIndex;

    auto restoreCallContext = [&]() {
        activeTape = previousTape;
        if (tapeIndex >= 0 && tapeIndex < static_cast<int>(tapes.size()))
            tapes[static_cast<size_t>(tapeIndex)].ptr = previousModulePtr;
    };

    try {
        if (functionValue.kind() == ValueKind::Code) {
            runBlock(std::get<Code>(functionValue.data));
        } else if (functionValue.kind() == ValueKind::Str) {
            runBlock(parseCodeSource(std::get<std::string>(functionValue.data)));
        } else {
            throw std::runtime_error("CALL: tape function " + displayName +
                                     " must be code or string, got " +
                                         std::string(kindName(functionValue.kind())));
        }
    } catch (...) {
        restoreCallContext();
        throw;
    }
    restoreCallContext();
    return true;
}

void VM::debug(const Instruction& ins) {
    if (!debugMode) return;

    const Tape& tape = tapes[activeTape];
    const Value empty;
    auto currentIt = tape.cells.find(tape.ptr);
    const Value& current = currentIt == tape.cells.end() ? empty : currentIt->second;
    std::cout << "\n"
              << ansiWrap("[debug]", "1;34")
              << " tape" << activeTape
              << "@" << tape.ptr
              << " cells=" << tape.length()
              << " :: " << formatInstruction(ins)
              << "\n"
              << colorMeta("        current ")
              << kindName(current.kind())
              << colorMeta(" = ")
              << colorValue(current, compactDisplayValue(current, 80))
              << "\n";

    if (stepMode) {
        std::cout << "Press Enter to step...";
        std::cout.flush();
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        if (isatty(STDIN_FILENO)) {
            std::cout << "\r\033[2K";
        } else {
            std::cout << "\n";
        }
        std::cout.flush();
    }
}

std::string VM::tapeSnapshot(int radius, int tapeIndex) const {
    const int windowRadius = std::max(1, radius);
    const size_t maxOffscreenCells = 6;

    int shownTape = tapeIndex < 0 ? activeTape : tapeIndex;
    if (shownTape < 0 || shownTape >= static_cast<int>(tapes.size()))
        throw std::runtime_error("PRINT_TAPE index out of bounds: " + std::to_string(shownTape));

    std::ostringstream out;
    out << "\n"
        << ansiWrap("TAPE", "1;34")
        << " active=tape" << activeTape
        << " showing=tape" << shownTape
        << " count=" << tapes.size() << "\n";

    const Tape& tape = tapes[shownTape];
    long long start = tape.ptr - windowRadius;
    if (tape.ptr >= 0 && start < 0)
        start = 0;
    const long long end = start + (windowRadius * 2);

    out << (shownTape == activeTape ? colorHead("=> ") : "   ")
        << "tape" << shownTape
        << (tape.name.empty() ? "" : " name=" + tape.name)
        << " ptr=" << colorHead(std::to_string(tape.ptr))
        << " cells=" << tape.length();

    if (tape.cells.empty()) {
        out << " " << colorMeta("empty");
    }
    out << "\n";

    out << "   |-- window " << start << ".." << end << "\n";
    out << "   |-- cells\n";
    for (long long pos = start; pos <= end; ++pos) {
        auto it = tape.cells.find(pos);
        bool isHead = pos == tape.ptr;
        out << "   |   "
            << (pos == end ? "`-- " : "|-- ")
            << "[" << pos << "] ";
        if (isHead)
            out << colorHead("<HEAD> ");
        else
            out << colorMeta("       ");

        if (it == tape.cells.end()) {
            out << colorMeta(".");
        } else {
            out << colorMeta(std::string(kindName(it->second.kind())) + " ")
                << colorValue(it->second, compactDisplayValue(it->second, 56));
        }
        out << "\n";
    }

    std::vector<long long> offscreen;
    for (const auto& [pos, _] : tape.cells) {
        if (pos < start || pos > end)
            offscreen.push_back(pos);
    }
    std::sort(offscreen.begin(), offscreen.end());

    if (!offscreen.empty()) {
        out << "   `-- off-window ";
        size_t shown = std::min(maxOffscreenCells, offscreen.size());
        for (size_t i = 0; i < shown; ++i) {
            long long pos = offscreen[i];
            const Value& value = tape.cells.at(pos);
            out << "[" << pos << ":"
                << colorValue(value, compactDisplayValue(value, 18)) << "]";
            if (i + 1 < shown)
                out << " ";
        }
        if (offscreen.size() > shown)
            out << " ... +" << (offscreen.size() - shown) << " more";
        out << "\n";
    } else {
        out << "   `-- off-window " << colorMeta("empty") << "\n";
    }

    return out.str();
}

std::string VM::cellSnapshot(long long cellIndex, int tapeIndex) const {
    int shownTape = tapeIndex < 0 ? activeTape : tapeIndex;
    if (shownTape < 0 || shownTape >= static_cast<int>(tapes.size()))
        throw std::runtime_error("CELL index out of bounds: tape" + std::to_string(shownTape));

    const Tape& tape = tapes[shownTape];
    auto it = tape.cells.find(cellIndex);
    std::ostringstream out;
    out << "\n"
        << ansiWrap("CELL", "1;34")
        << " tape" << shownTape << "[" << cellIndex << "]";
    if (shownTape == activeTape && cellIndex == tape.ptr)
        out << " " << colorHead("<HEAD>");
    out << "\n";

    if (it == tape.cells.end()) {
        out << "   `-- " << colorMeta("nil / empty cell") << "\n";
        return out.str();
    }

    out << "   |-- type " << kindName(it->second.kind()) << "\n"
        << "   `-- value " << colorValue(it->second, stringifyValue(it->second)) << "\n";
    return out.str();
}

long long VM::tapePointer(int tapeIndex) const {
    int shownTape = tapeIndex < 0 ? activeTape : tapeIndex;
    if (shownTape < 0 || shownTape >= static_cast<int>(tapes.size()))
        throw std::runtime_error("tape index out of bounds: " + std::to_string(shownTape));
    return tapes[shownTape].ptr;
}

// ---------------------------------------------------------------------------
// execute — handles all opcodes except JMP/JMPIF/JMPNOT (those live in run)
// ---------------------------------------------------------------------------

void VM::execute(const Instruction& ins) {
    if (halted)
        return;

    debug(ins);

    auto& tape = tapes[activeTape];

    // ---- Tape navigation --------------------------------------------------

    if (ins.opcode == "MOVE") {
        if (ins.args.empty())
             tape.moveForward(static_cast<long long>(1));
        else
        tape.moveForward(integerOperand(argOrThrow(ins, 0), tape, ins.opcode, "distance"));
    }
    else if (ins.opcode == "BACK") {
        if (ins.args.empty())
             tape.moveBackward(static_cast<long long>(1));
        else
        tape.moveBackward(integerOperand(argOrThrow(ins, 0), tape, ins.opcode, "distance"));
    }

    // ---- Core cell ops ----------------------------------------------------

    else if (ins.opcode == "SET") {
        tape.current() = resolveOperand(argOrThrow(ins, 0), tape);
    }
    else if (ins.opcode == "PRINT") {
        std::cout << stringifyValue(tape.current()) << "\n";
    }
    else if (ins.opcode == "PRINTJ") {
        std::cout << stringifyJsonValue(tape.current()) << "\n";
    }
    else if (ins.opcode == "EXIT") {
        halted = true;
    }

    // ---- Type system ------------------------------------------------------

    else if (ins.opcode == "TYPE") {
        tape.current() = Value(std::string(kindName(tape.current().kind())));
    }
    else if (ins.opcode == "CAST") {
        std::string target = stringOperand(argOrThrow(ins, 0), tape);
        std::transform(target.begin(), target.end(), target.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        Value& cur = tape.current();

        if (target == "int") {
            cur = Value(static_cast<long long>(numericValue(cur)));
        }
        else if (target == "float") {
            cur = Value(numericValue(cur));
        }
        else if (target == "bool") {
            bool b = false;
            switch (cur.kind()) {
                case ValueKind::Nil:   b = false; break;
                case ValueKind::Bool:  b = std::get<bool>(cur.data); break;
                case ValueKind::Int:   b = std::get<long long>(cur.data) != 0; break;
                case ValueKind::Float: b = std::get<double>(cur.data)    != 0.0; break;
                case ValueKind::Char:  b = std::get<char>(cur.data)      != '\0'; break;
                case ValueKind::Str:   b = !std::get<std::string>(cur.data).empty(); break;
                case ValueKind::List:  b = !std::get<List>(cur.data).empty(); break;
                case ValueKind::Map:   b = !std::get<Map>(cur.data).empty(); break;
                case ValueKind::Code:  b = !std::get<Code>(cur.data).empty(); break;
            }
            cur = Value(b);
        }
        else if (target == "str") {
            cur = Value(stringifyValue(cur));
        }
        else if (target == "char") {
            if (cur.kind() == ValueKind::Str) {
                const auto& s = std::get<std::string>(cur.data);
                cur = s.empty() ? Value('\0') : Value(s[0]);
            } else if (cur.kind() == ValueKind::Int) {
                cur = Value(static_cast<char>(std::get<long long>(cur.data)));
            } else {
                throw std::runtime_error("CAST char: unsupported source type " +
                                         std::string(kindName(cur.kind())));
            }
        }
        else {
            throw std::runtime_error("CAST: unknown target type: " + target);
        }
    }

    // ---- Multi-tape ops ---------------------------------------------------

    else if (ins.opcode == "SEEK") {
        tape.ptr = integerOperand(argOrThrow(ins, 0), tape, ins.opcode, "cell index");
    }
    else if (ins.opcode == "HOME") {
        tape.ptr = 0;
    }

    else if (ins.opcode == "TAPE") {
        int target = resolveTapeSelector(stringOperand(argOrThrow(ins, 0), tape), ins.opcode);
        activeTape = target;
    }
    else if (ins.opcode == "PRINT_TAPE") {
        int target = resolveTapeSelector(stringOperand(argOrThrow(ins, 0), tape), ins.opcode);
        std::cout << tapeSnapshot(3, target);
    }
    else if (ins.opcode == "TAPENAME" || ins.opcode == "NAMETAPE") {
        int target = activeTape;
        std::string name;
        if (ins.args.size() == 1) {
            name = stringOperand(argOrThrow(ins, 0), tape);
        } else if (ins.args.size() >= 2) {
            target = resolveTapeSelector(stringOperand(argOrThrow(ins, 0), tape), ins.opcode);
            name = stringOperand(argOrThrow(ins, 1), tape);
        } else {
            throw std::runtime_error(ins.opcode + ": expected name, or tape name");
        }
        tapes[static_cast<size_t>(target)].name = name;
        tapes[static_cast<size_t>(target)].cells[TAPE_NAME_CELL] = Value(name);
        syncAllTapeMetadata();
    }
    else if (ins.opcode == "TAPEDEF" || ins.opcode == "TFUNC") {
        std::string name = stringOperand(argOrThrow(ins, 0), tape);
        long long cellIndex = ins.args.size() > 1
            ? integerOperand(argOrThrow(ins, 1), tape, ins.opcode, "function cell index")
            : tape.ptr;

        Value& directory = tapes[activeTape].cells[TAPE_FUNCTIONS_CELL];
        if (directory.kind() == ValueKind::Nil)
            directory = Value(Map{});
        if (directory.kind() != ValueKind::Map)
            throw std::runtime_error(ins.opcode + ": tape function directory cell -1 must be a map or nil");
        std::get<Map>(directory.data)[name] = Value(cellIndex);
        tapes[activeTape].functions[toUpperStr(name)] = cellIndex;
        syncAllTapeMetadata();
    }
    else if (ins.opcode == "TAPEUNDEF") {
        std::string name = stringOperand(argOrThrow(ins, 0), tape);
        auto& directory = tapes[activeTape].cells[TAPE_FUNCTIONS_CELL];
        if (directory.kind() == ValueKind::Map)
            std::get<Map>(directory.data).erase(name);
        tapes[activeTape].functions.erase(toUpperStr(name));
        syncAllTapeMetadata();
    }
    else if (ins.opcode == "TAPEFUNCS") {
        int target = ins.args.empty()
            ? activeTape
            : resolveTapeSelector(stringOperand(argOrThrow(ins, 0), tape), ins.opcode);
        syncAllTapeMetadata();
        List names;
        std::vector<std::string> sorted;
        auto dirIt = tapes[static_cast<size_t>(target)].cells.find(TAPE_FUNCTIONS_CELL);
        if (dirIt != tapes[static_cast<size_t>(target)].cells.end() &&
            dirIt->second.kind() == ValueKind::Map) {
            for (const auto& [name, _] : std::get<Map>(dirIt->second.data))
                sorted.push_back(name);
        }
        std::sort(sorted.begin(), sorted.end());
        for (const auto& name : sorted)
            names.emplace_back(name);
        tape.current() = Value(std::move(names));
    }
    else if (ins.opcode == "LEN" || ins.opcode == "LENGTH") {
        tape.current() = Value(static_cast<long long>(tape.length()));
    }
    else if (ins.opcode == "CMP" || ins.opcode == "COMPARE") {
        int other = resolveTapeSelector(stringOperand(argOrThrow(ins, 0), tape), ins.opcode);
        tapes[activeTape].current() = Value(valuesEqual(tapes[activeTape].current(), tapes[other].current()));
    }
    else if (ins.opcode == "COPY") {
        // Copy current cell to the same ptr position on dest tape
        Value value = tape.current();
        int dest = resolveTapeSelector(stringOperand(argOrThrow(ins, 0), tape), ins.opcode);
        tapes[dest].cells[tapes[dest].ptr] = value;
    }
    else if (ins.opcode == "TAPEREAD" || ins.opcode == "TGET" || ins.opcode == "PEEK") {
        bool hasCell = ins.args.size() > 1;
        long long srcCell = hasCell ? integerOperand(argOrThrow(ins, 1), tape, ins.opcode, "cell index") : 0;
        int src = resolveTapeSelector(stringOperand(argOrThrow(ins, 0), tape), ins.opcode);
        if (!hasCell)
            srcCell = tapes[src].ptr;
        tapes[activeTape].current() = tapes[src].cells[srcCell];
    }
    else if (ins.opcode == "TAPEWRITE" || ins.opcode == "TPUT" || ins.opcode == "POKE") {
        Value value = ins.args.size() > 2 ? resolveOperand(argOrThrow(ins, 2), tape) : tape.current();
        bool hasCell = ins.args.size() > 1;
        long long destCell = hasCell ? integerOperand(argOrThrow(ins, 1), tape, ins.opcode, "cell index") : 0;
        int dest = resolveTapeSelector(stringOperand(argOrThrow(ins, 0), tape), ins.opcode);
        if (!hasCell)
            destCell = tapes[dest].ptr;
        tapes[dest].cells[destCell] = value;
    }
    else if (ins.opcode == "TAPESWAP" || ins.opcode == "TSWAP") {
        bool hasCell = ins.args.size() > 1;
        long long otherCell = hasCell ? integerOperand(argOrThrow(ins, 1), tape, ins.opcode, "cell index") : 0;
        int other = resolveTapeSelector(stringOperand(argOrThrow(ins, 0), tape), ins.opcode);
        if (!hasCell)
            otherCell = tapes[other].ptr;
        std::swap(tapes[activeTape].current(), tapes[other].cells[otherCell]);
    }
    else if (ins.opcode == "TAPESEND" || ins.opcode == "TSEND" || ins.opcode == "SEND") {
        Value value = ins.args.size() > 2 ? resolveOperand(argOrThrow(ins, 2), tape) : tape.current();
        bool hasChannel = ins.args.size() > 1;
        long long channel = hasChannel ? integerOperand(argOrThrow(ins, 1), tape, ins.opcode, "cell index") : 0;
        int dest = resolveTapeSelector(stringOperand(argOrThrow(ins, 0), tape), ins.opcode);
        if (!hasChannel)
            channel = tapes[dest].ptr;
        Value& inbox = tapes[dest].cells[channel];
        if (inbox.kind() == ValueKind::Nil)
            inbox = Value(List{});
        if (inbox.kind() != ValueKind::List)
            throw std::runtime_error(ins.opcode + ": destination cell must be a list or nil");
        std::get<List>(inbox.data).push_back(value);
    }
    else if (ins.opcode == "TAPERECV" || ins.opcode == "TRECV" || ins.opcode == "RECV") {
        bool hasChannel = ins.args.size() > 1;
        long long channel = hasChannel ? integerOperand(argOrThrow(ins, 1), tape, ins.opcode, "cell index") : 0;
        int src = resolveTapeSelector(stringOperand(argOrThrow(ins, 0), tape), ins.opcode);
        if (!hasChannel)
            channel = tapes[src].ptr;
        Value& inbox = tapes[src].cells[channel];
        if (inbox.kind() == ValueKind::Nil) {
            tapes[activeTape].current() = Value();
        } else if (inbox.kind() == ValueKind::List) {
            auto& queue = std::get<List>(inbox.data);
            if (queue.empty()) {
                tapes[activeTape].current() = Value();
            } else {
                Value message = queue.front();
                queue.erase(queue.begin());
                tapes[activeTape].current() = message;
            }
        } else {
            throw std::runtime_error(ins.opcode + ": source cell must be a list or nil");
        }
    }
    else if (ins.opcode == "CLEAR_TAPE" || ins.opcode == "CLEARTAPE") {
        tape.cells.clear();
        tape.name.clear();
        tape.functions.clear();
        tape.ptr = 0;
        syncAllTapeMetadata();
    }
    else if (ins.opcode == "DELETE") {
        tape.cells.erase(tape.ptr);
        if (tape.ptr == TAPE_NAME_CELL || tape.ptr == TAPE_FUNCTIONS_CELL)
            syncAllTapeMetadata();
    }

    // ---- Arithmetic -------------------------------------------------------

    else if (ins.opcode == "ADD") {
        Value operand = resolveOperand(argOrThrow(ins, 0), tape);
        double result = numericValue(tape.current()) + numericValue(operand);
        bool intOnly = tape.current().kind() == ValueKind::Int && operand.kind() == ValueKind::Int;
        tape.current() = intOnly ? Value(static_cast<long long>(result)) : Value(result);
    }
    else if (ins.opcode == "SUB") {
        Value operand = resolveOperand(argOrThrow(ins, 0), tape);
        double result = numericValue(tape.current()) - numericValue(operand);
        bool intOnly = tape.current().kind() == ValueKind::Int && operand.kind() == ValueKind::Int;
        tape.current() = intOnly ? Value(static_cast<long long>(result)) : Value(result);
    }
    else if (ins.opcode == "MUL") {
        Value operand = resolveOperand(argOrThrow(ins, 0), tape);
        double result = numericValue(tape.current()) * numericValue(operand);
        bool intOnly = tape.current().kind() == ValueKind::Int && operand.kind() == ValueKind::Int;
        tape.current() = intOnly ? Value(static_cast<long long>(result)) : Value(result);
    }
    else if (ins.opcode == "DIV") {
        Value operand = resolveOperand(argOrThrow(ins, 0), tape);
        double divisor = numericValue(operand);
        if (divisor == 0.0) throw std::runtime_error("DIV by zero");
        double result = numericValue(tape.current()) / divisor;
        bool intOnly = tape.current().kind() == ValueKind::Int && operand.kind() == ValueKind::Int;
        tape.current() = intOnly ? Value(static_cast<long long>(result)) : Value(result);
    }
    else if (ins.opcode == "MOD") {
        Value operand = resolveOperand(argOrThrow(ins, 0), tape);
        long long divisor = static_cast<long long>(numericValue(operand));
        if (divisor == 0) throw std::runtime_error("MOD by zero");
        long long lhs = static_cast<long long>(numericValue(tape.current()));
        tape.current() = Value(lhs % divisor);
    }
    else if (ins.opcode == "LT") {
        Value operand = resolveOperand(argOrThrow(ins, 0), tape);
        tape.current() = Value(numericValue(tape.current()) < numericValue(operand));
    }
    else if (ins.opcode == "GT") {
        Value operand = resolveOperand(argOrThrow(ins, 0), tape);
        tape.current() = Value(numericValue(tape.current()) > numericValue(operand));
    }
    else if (ins.opcode == "LTE") {
        Value operand = resolveOperand(argOrThrow(ins, 0), tape);
        tape.current() = Value(numericValue(tape.current()) <= numericValue(operand));
    }
    else if (ins.opcode == "GTE") {
        Value operand = resolveOperand(argOrThrow(ins, 0), tape);
        tape.current() = Value(numericValue(tape.current()) >= numericValue(operand));
    }
    else if (ins.opcode == "ABS") {
        double val = numericValue(tape.current());
        double result = val < 0 ? -val : val;
        bool isInt = tape.current().kind() == ValueKind::Int;
        tape.current() = isInt ? Value(static_cast<long long>(result)) : Value(result);
    }
    else if (ins.opcode == "NEG") {
        double val = numericValue(tape.current());
        bool isInt = tape.current().kind() == ValueKind::Int;
        tape.current() = isInt ? Value(-static_cast<long long>(val)) : Value(-val);
    }

    // ---- File I/O ---------------------------------------------------------

    else if (ins.opcode == "READFILE") {
        std::string path = stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
        std::ifstream file(path);
        if (!file) throw std::runtime_error("Cannot open file: " + path);
        std::stringstream buf;
        buf << file.rdbuf();
        tape.current() = Value(buf.str());
    }
    else if (ins.opcode == "WRITEFILE") {
        std::string path = stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
        std::filesystem::path outPath(path);
        if (outPath.has_parent_path())
            std::filesystem::create_directories(outPath.parent_path());
        std::ofstream file(outPath);
        if (!file) throw std::runtime_error("Cannot write file: " + path);
        file << stringifyValue(tape.current());
    }

    // ---- PostgreSQL DB I/O ------------------------------------------------
    // These opcodes dynamically load libpq at runtime. The interpreter still
    // builds when PostgreSQL client libraries are absent; DB opcodes then raise
    // a clear runtime error.

    else if (ins.opcode == "DBCONNECT") {
        PgApi& api = requirePgApi();
        std::string conninfo = sqlFromInstructionOrCell(ins, tape);

        if (dbConn) {
            api.PQfinish(static_cast<PgApi::PGconn*>(dbConn));
            dbConn = nullptr;
        }

        PgApi::PGconn* conn = api.PQconnectdb(conninfo.c_str());
        if (!conn)
            throw std::runtime_error("DBCONNECT: failed to allocate connection");
        if (api.PQstatus(conn) != 0) {
            std::string error = pgError(api, conn);
            api.PQfinish(conn);
            throw std::runtime_error("DBCONNECT: " + error);
        }

        dbConn = conn;
        Map out;
        out["connected"] = Value(true);
        out["backend"] = Value(std::string("postgres"));
        tape.current() = Value(std::move(out));
    }
    else if (ins.opcode == "DBCLOSE") {
        if (dbConn) {
            PgApi& api = requirePgApi();
            api.PQfinish(static_cast<PgApi::PGconn*>(dbConn));
            dbConn = nullptr;
        }
        Map out;
        out["connected"] = Value(false);
        out["backend"] = Value(std::string("postgres"));
        tape.current() = Value(std::move(out));
    }
    else if (ins.opcode == "DBSTATUS") {
        Map out;
        out["connected"] = Value(dbConn != nullptr);
        out["backend"] = Value(std::string("postgres"));
        tape.current() = Value(std::move(out));
    }
    else if (ins.opcode == "DBEXEC" || ins.opcode == "DBQUERY") {
        requireDbConnected(dbConn);
        PgApi& api = requirePgApi();
        std::string sql = sqlFromInstructionOrCell(ins, tape);
        tape.current() = pgRunSql(api, dbConn, sql, ins.opcode == "DBQUERY", ins.opcode);
    }
    else if (ins.opcode == "DBSELECT") {
        requireDbConnected(dbConn);
        PgApi& api = requirePgApi();
        std::string table = stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
        std::string sql = "SELECT * FROM " + pgQuoteIdentifier(api, dbConn, table);
        if (ins.args.size() > 1) {
            std::string where = stringifyValue(resolveOperand(argOrThrow(ins, 1), tape));
            if (!trimStr(where).empty())
                sql += " WHERE " + where;
        }
        tape.current() = pgRunSql(api, dbConn, sql, true, ins.opcode);
    }
    else if (ins.opcode == "DBINSERT") {
        requireDbConnected(dbConn);
        PgApi& api = requirePgApi();
        std::string table = stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
        Value values = dbMapArgOrCurrent(ins, 1, tape, ins.opcode);
        std::string sql = buildInsertSql(api, dbConn, table, requireMapValue(values, ins.opcode));
        tape.current() = pgRunSql(api, dbConn, sql, false, ins.opcode);
    }
    else if (ins.opcode == "DBUPDATE") {
        requireDbConnected(dbConn);
        PgApi& api = requirePgApi();
        std::string table = stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
        std::string where = stringifyValue(resolveOperand(argOrThrow(ins, 1), tape));
        Value values = dbMapArgOrCurrent(ins, 2, tape, ins.opcode);
        std::string sql = buildUpdateSql(api, dbConn, table, requireMapValue(values, ins.opcode), where);
        tape.current() = pgRunSql(api, dbConn, sql, false, ins.opcode);
    }
    else if (ins.opcode == "DBDELETE") {
        requireDbConnected(dbConn);
        PgApi& api = requirePgApi();
        std::string table = stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
        std::string where = stringifyValue(resolveOperand(argOrThrow(ins, 1), tape));
        if (trimStr(where).empty())
            throw std::runtime_error("DBDELETE: WHERE clause is required");
        std::string sql = "DELETE FROM " + pgQuoteIdentifier(api, dbConn, table) + " WHERE " + where;
        tape.current() = pgRunSql(api, dbConn, sql, false, ins.opcode);
    }
    else if (ins.opcode == "DBLITERAL") {
        requireDbConnected(dbConn);
        PgApi& api = requirePgApi();
        Value value = ins.args.empty() ? tape.current() : resolveOperand(argOrThrow(ins, 0), tape);
        tape.current() = Value(pgValueLiteral(api, dbConn, value));
    }
    else if (ins.opcode == "DBIDENT") {
        requireDbConnected(dbConn);
        PgApi& api = requirePgApi();
        std::string value = ins.args.empty()
            ? stringifyValue(tape.current())
            : stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
        tape.current() = Value(pgQuoteIdentifier(api, dbConn, value));
    }
    else if (ins.opcode == "DB_TAPE_OPEN" || ins.opcode == "DBTAPEOPEN") {
        requireDbConnected(dbConn);
        PgApi& api = requirePgApi();
        std::string spaceKey = stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
        std::string kind = ins.args.size() > 1
            ? stringifyValue(resolveOperand(argOrThrow(ins, 1), tape))
            : "memory";
        pgOpenTapeSpace(api, dbConn, spaceKey, kind, ins.opcode);
        tapes[activeTape].current() = Value(spaceKey);
    }
    else if (ins.opcode == "DB_TAPE_INPUT" || ins.opcode == "DB_TAPE_PUT" || ins.opcode == "DBTAPEPUT") {
        requireDbConnected(dbConn);
        PgApi& api = requirePgApi();
        std::string spaceKey = stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
        long long tapeIndex = activeTape;
        long long cellIndex = tape.ptr;
        if (ins.args.size() > 1)
            tapeIndex = integerOperand(argOrThrow(ins, 1), tape, ins.opcode, "tape index");
        if (ins.args.size() > 2)
            cellIndex = integerOperand(argOrThrow(ins, 2), tape, ins.opcode, "cell index");
        Value value = ins.args.size() > 3 ? resolveOperand(argOrThrow(ins, 3), tape) : tape.current();
        int tapeSlot = ensureTapeIndex(tapeIndex, ins.opcode);
        tapes[tapeSlot].cells[cellIndex] = value;
        pgStoreTapeCell(api, dbConn, spaceKey, tapeIndex, cellIndex, value, ins.opcode);
    }
    else if (ins.opcode == "DB_TAPE_OUTPUT" || ins.opcode == "DB_TAPE_GET" || ins.opcode == "DBTAPEGET") {
        requireDbConnected(dbConn);
        PgApi& api = requirePgApi();
        std::string spaceKey = stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
        long long tapeIndex = activeTape;
        if (ins.args.size() > 1)
            tapeIndex = integerOperand(argOrThrow(ins, 1), tape, ins.opcode, "tape index");
        int tapeSlot = ensureTapeIndex(tapeIndex, ins.opcode);
        long long cellIndex = ins.args.size() > 2
            ? integerOperand(argOrThrow(ins, 2), tapes[activeTape], ins.opcode, "cell index")
            : tapes[tapeSlot].ptr;

        Value loaded;
        if (pgFetchTapeCell(api, dbConn, spaceKey, tapeIndex, cellIndex, loaded, ins.opcode))
            tapes[activeTape].current() = loaded;
        else
            tapes[activeTape].current() = Value();
    }
    else if (ins.opcode == "DB_TAPE_FIND" || ins.opcode == "DBTAPEFIND") {
        requireDbConnected(dbConn);
        PgApi& api = requirePgApi();
        long long tapeIndex = integerOperand(argOrThrow(ins, 0), tape, ins.opcode, "tape index");
        long long cellIndex = integerOperand(argOrThrow(ins, 1), tape, ins.opcode, "cell index");
        Value value = ins.args.size() > 2 ? resolveOperand(argOrThrow(ins, 2), tape) : tape.current();
        tapes[activeTape].current() = pgFindTapeSpace(api, dbConn, tapeIndex, cellIndex, value, ins.opcode);
    }
    else if (ins.opcode == "DB_TAPE_SAVE" || ins.opcode == "DBTAPESAVE") {
        requireDbConnected(dbConn);
        PgApi& api = requirePgApi();
        std::string spaceKey = stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
        std::string selector = ins.args.size() > 1
            ? stringifyValue(resolveOperand(argOrThrow(ins, 1), tape))
            : std::to_string(activeTape);

        long long saved = 0;
        if (selector == "*") {
            for (size_t tapeIndex = 0; tapeIndex < tapes.size(); ++tapeIndex) {
                for (const auto& [cellIndex, value] : tapes[tapeIndex].cells) {
                    pgStoreTapeCell(api, dbConn, spaceKey, static_cast<long long>(tapeIndex), cellIndex, value, ins.opcode);
                    ++saved;
                }
            }
        } else {
            long long tapeIndex = integerValue(parseValue(selector), ins.opcode, "tape selector");
            int tapeSlot = ensureTapeIndex(tapeIndex, ins.opcode);
            for (const auto& [cellIndex, value] : tapes[tapeSlot].cells) {
                pgStoreTapeCell(api, dbConn, spaceKey, tapeIndex, cellIndex, value, ins.opcode);
                ++saved;
            }
        }
        tapes[activeTape].current() = Value(saved);
    }
    else if (ins.opcode == "DB_TAPE_LOAD" || ins.opcode == "DBTAPELOAD") {
        requireDbConnected(dbConn);
        PgApi& api = requirePgApi();
        std::string spaceKey = stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
        std::string selector = ins.args.size() > 1
            ? stringifyValue(resolveOperand(argOrThrow(ins, 1), tape))
            : std::to_string(activeTape);
        if (selector != "*") {
            long long tapeIndex = integerValue(parseValue(selector), ins.opcode, "tape selector");
            ensureTapeIndex(tapeIndex, ins.opcode);
            selector = std::to_string(tapeIndex);
        }
        auto ensure = [this](long long index, const std::string& opcode) {
            return ensureTapeIndex(index, opcode);
        };
        long long loaded = pgLoadTapeCells(api, dbConn, spaceKey, selector, tapes, ensure, ins.opcode);
        tapes[activeTape].current() = Value(loaded);
    }
    else if (ins.opcode == "DB_TAPE_EVENT" || ins.opcode == "DBTAPEEVENT") {
        requireDbConnected(dbConn);
        PgApi& api = requirePgApi();
        std::string spaceKey = stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
        std::string eventType = stringifyValue(resolveOperand(argOrThrow(ins, 1), tape));
        std::string note = ins.args.size() > 2
            ? stringifyValue(resolveOperand(argOrThrow(ins, 2), tape))
            : "";
        long long id = pgInsertTapeEvent(
            api,
            dbConn,
            spaceKey,
            eventType,
            activeTape,
            tape.ptr,
            ins.opcode,
            note,
            ins.opcode
        );
        tapes[activeTape].current() = Value(id);
    }

    // ---- String operations ------------------------------------------------

    else if (ins.opcode == "CONCAT") {
        std::string rhs = stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
        tape.current() = Value(stringifyValue(tape.current()) + rhs);
    }
    else if (ins.opcode == "SPLIT") {
        std::string delim = stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
        std::string src   = stringifyValue(tape.current());
        if (delim.empty())
            throw std::runtime_error("SPLIT: delimiter must not be empty");
        List out;
        size_t pos = 0, prev = 0;
        while ((pos = src.find(delim, prev)) != std::string::npos) {
            out.emplace_back(src.substr(prev, pos - prev));
            prev = pos + delim.size();
        }
        out.emplace_back(src.substr(prev));
        tape.current() = Value(std::move(out));
    }
    else if (ins.opcode == "JOIN") {
        std::string delim = ins.args.empty() ? "" : stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
        auto* list = std::get_if<List>(&tape.current().data);
        if (!list)
            throw std::runtime_error("JOIN: current cell must be a list");
        std::string out;
        for (size_t i = 0; i < list->size(); ++i) {
            if (i > 0) out += delim;
            out += stringifyValue((*list)[i]);
        }
        tape.current() = Value(std::move(out));
    }
    else if (ins.opcode == "SUBSTR") {
        int start = intOperand(argOrThrow(ins, 0), tape, ins.opcode, "start index");
        int len   = intOperand(argOrThrow(ins, 1), tape, ins.opcode, "length");
        if (tape.current().kind() != ValueKind::Str)
            throw std::runtime_error("SUBSTR: current cell is not a string");
        tape.current() = Value(std::get<std::string>(tape.current().data).substr(start, len));
    }
    else if (ins.opcode == "FIND") {
        std::string sub = stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
        if (tape.current().kind() != ValueKind::Str)
            throw std::runtime_error("FIND: current cell is not a string");
        auto pos = std::get<std::string>(tape.current().data).find(sub);
        tape.current() = Value(static_cast<long long>(pos));
    }
    else if (ins.opcode == "REPLACE") {
        std::string oldStr = stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
        std::string repStr = stringifyValue(resolveOperand(argOrThrow(ins, 1), tape));
        if (tape.current().kind() != ValueKind::Str)
            throw std::runtime_error("REPLACE: current cell is not a string");
        auto& s = std::get<std::string>(tape.current().data);
        size_t pos = 0;
        while ((pos = s.find(oldStr, pos)) != std::string::npos) {
            s.replace(pos, oldStr.size(), repStr);
            pos += repStr.size();
        }
    }
    else if (ins.opcode == "UPPER") {
        if (tape.current().kind() != ValueKind::Str)
            throw std::runtime_error("UPPER: current cell is not a string");
        auto& s = std::get<std::string>(tape.current().data);
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::toupper(c); });
    }
    else if (ins.opcode == "LOWER") {
        if (tape.current().kind() != ValueKind::Str)
            throw std::runtime_error("LOWER: current cell is not a string");
        auto& s = std::get<std::string>(tape.current().data);
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
    }
    else if (ins.opcode == "REVERSE") {
        if (tape.current().kind() != ValueKind::Str)
            throw std::runtime_error("REVERSE: current cell is not a string");
        auto& s = std::get<std::string>(tape.current().data);
        std::reverse(s.begin(), s.end());
    }

    // ---- NLP service operations ------------------------------------------
    // Core tape NLP: tokenization, analysis, similarity, diff, and transparent
    // lexicon sentiment. NLPLOAD is a compatibility hook for future backends.

    else if (ins.opcode == "NLPLOAD") {
        std::string path = ins.args.empty()
            ? stringifyValue(tape.current())
            : stringOperand(argOrThrow(ins, 0), tape);
        tape.current() = Value(nlp.loadModel(path));
    }
    else if (ins.opcode == "NLPTOKENS") {
        tape.current() = Value(nlp.tokenize(stringifyValue(tape.current())));
    }
    else if (ins.opcode == "NLPANALYZE") {
        tape.current() = Value(nlp.analyze(stringifyValue(tape.current())));
    }
    else if (ins.opcode == "NLPSIM") {
        long long otherCell = integerOperand(argOrThrow(ins, 0), tape, ins.opcode, "other cell index");
        bool forceTokenMode = false;
        if (ins.args.size() > 1) {
            std::string mode = toUpperStr(stringOperand(ins.args[1], tape));
            forceTokenMode = mode == "TOKEN" || mode == "TOKENS";
        }
        std::string left = stringifyValue(tape.current());
        std::string right = stringifyValue(tape.cells[otherCell]);
        tape.current() = Value(nlp.similarity(left, right, forceTokenMode));
    }
    else if (ins.opcode == "NLPDIFF") {
        long long otherCell = integerOperand(argOrThrow(ins, 0), tape, ins.opcode, "other cell index");
        std::string left = stringifyValue(tape.current());
        std::string right = stringifyValue(tape.cells[otherCell]);
        tape.current() = Value(nlp.diff(left, right));
    }
    else if (ins.opcode == "NLPPREDICT") {
        int k = ins.args.size() > 0 ? intOperand(ins.args[0], tape, ins.opcode, "result count") : 1;
        double threshold = ins.args.size() > 1 ? doubleOperand(ins.args[1], tape, ins.opcode, "threshold") : 0.0;
        tape.current() = Value(nlp.predict(stringifyValue(tape.current()), k, threshold));
    }
    else if (ins.opcode == "NLPSENTIMENT") {
        int k = ins.args.size() > 0 ? intOperand(ins.args[0], tape, ins.opcode, "result count") : 1;
        double threshold = ins.args.size() > 1 ? doubleOperand(ins.args[1], tape, ins.opcode, "threshold") : 0.0;
        tape.current() = Value(nlp.sentiment(stringifyValue(tape.current()), k, threshold));
    }
    else if (ins.opcode == "NLPGRAMMAR") {
        tape.current() = Value(nlp.grammarCheck(stringifyValue(tape.current())));
    }
    else if (ins.opcode == "NLPCONTEXT") {
        tape.current() = Value(nlp.contextAnalysis(stringifyValue(tape.current())));
    }
    else if (ins.opcode == "NLPRELATIONS") {
        tape.current() = Value(nlp.relationExtraction(stringifyValue(tape.current())));
    }
    else if (ins.opcode == "NLPLOGIC") {
        std::string operation = ins.args.empty() ? "contrapositive" : stringOperand(ins.args[0], tape);
        tape.current() = Value(nlp.logicalOperation(stringifyValue(tape.current()), operation));
    }
    else if (ins.opcode == "NLPFUZZY") {
        double membership = ins.args.empty() ? 1.0 : doubleOperand(ins.args[0], tape, ins.opcode, "membership");
        tape.current() = Value(nlp.fuzzyLogic(stringifyValue(tape.current()), membership));
    }
    else if (ins.opcode == "NLPPROB") {
        std::string type = ins.args.empty() ? "token" : stringOperand(ins.args[0], tape);
        tape.current() = Value(nlp.probabilityCalc(stringifyValue(tape.current()), type));
    }

    // ---- I/O: stdin -------------------------------------------------------

    else if (ins.opcode == "INPUT") {
        std::string line;
        std::getline(std::cin, line);
        if (ins.args.empty()) {
            tape.current() = Value(line);
        } else {
            std::string typeArg = stringOperand(ins.args[0], tape);
            std::transform(typeArg.begin(), typeArg.end(), typeArg.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (typeArg == "int") {
                tape.current() = Value(parseLongLongStrict(line, ins.opcode, "input integer"));
            } else if (typeArg == "float") {
                tape.current() = Value(parseDoubleStrict(line, ins.opcode, "input float"));
            } else if (typeArg == "bool") {
                tape.current() = Value(line == "true" || line == "1");
            } else {
                tape.current() = Value(line);
            }
        }
    }

    // ---- Homoiconic: QUOTE ------------------------------------------------
    // QUOTE stores instructions as a Code cell without executing them.
    // Forms:
    //   QUOTE (INSTR1 ; INSTR2)   — code block literal
    //   QUOTE OPCODE arg1 arg2    — single instruction from remaining args

    else if (ins.opcode == "QUOTE") {
        if (!ins.args.empty() && ins.args[0].front() == '(') {
            tape.current() = parseCodeBlockToken(ins.args[0]);
        } else if (!ins.args.empty()) {
            Instruction quoted;
            quoted.opcode = toUpperStr(ins.args[0]);
            for (size_t i = 1; i < ins.args.size(); ++i)
                quoted.args.push_back(ins.args[i]);
            tape.current() = Value(Code{quoted});
        } else {
            throw std::runtime_error("QUOTE: requires at least one argument");
        }
    }

    // ---- Homoiconic: EXEC / EVAL ------------------------------------------
    // Execute the symbolic Code (or Str) stored in the current cell.
    // EVAL is an alias — both evaluate/execute the symbolic form.

    else if (ins.opcode == "EXEC" || ins.opcode == "EVAL") {
        if (tape.current().kind() == ValueKind::Code) {
            Code codeCopy = std::get<Code>(tape.current().data);
            runBlock(codeCopy);
        } else if (tape.current().kind() == ValueKind::Str) {
            const std::string& s = std::get<std::string>(tape.current().data);
            Code code = parseCodeSource(s);
            runBlock(code);
        } else {
            throw std::runtime_error(ins.opcode + ": current cell must be Code or Str, got " +
                                     kindName(tape.current().kind()));
        }
    }
    else if (ins.opcode == "TRY") {
        auto* tryCode = std::get_if<Code>(&tape.current().data);
        if (!tryCode)
            throw std::runtime_error("TRY: current cell must be Code");

        Code tryCopy = *tryCode;
        try {
            runBlock(tryCopy);
        } catch (const std::exception& e) {
            Map err;
            err["error"] = Value(std::string(e.what()));
            err["handled"] = Value(false);
            tape.current() = Value(std::move(err));

            if (!ins.args.empty()) {
                long long catchCell = integerOperand(argOrThrow(ins, 0), tape, ins.opcode, "catch cell index");
                auto* catchCode = std::get_if<Code>(&tape.cells[catchCell].data);
                if (!catchCode)
                    throw std::runtime_error("TRY: catch cell must contain Code");
                std::get<Map>(tape.current().data)["handled"] = Value(true);
                Code catchCopy = *catchCode;
                runBlock(catchCopy);
            }
        }
    }
    else if (ins.opcode == "RAISE") {
        std::string message = ins.args.empty()
            ? stringifyValue(tape.current())
            : stringOperand(argOrThrow(ins, 0), tape);
        throw std::runtime_error(message);
    }

    // ---- Homoiconic: MATCH ------------------------------------------------
    // Match the current cell against a pattern; store bool result in current cell.
    // Patterns: type names ("int","str",...), "_" wildcard, literals, list/map structures.

    else if (ins.opcode == "MATCH") {
        Value pattern = resolveOperand(argOrThrow(ins, 0), tape);
        bool matched = matchPattern(tape.current(), pattern);
        tape.current() = Value(matched);
    }

    else if (ins.opcode == "JSONGET" || ins.opcode == "LIST_GET" || ins.opcode == "MAP_GET") {
        std::string key = stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
        const Value& original = tape.current();
        Value result;

        if (original.kind() == ValueKind::Map) {
            const auto& map = std::get<Map>(original.data);
            auto it = map.find(key);
            if (it != map.end())
                result = it->second;
        } else if (original.kind() == ValueKind::List && isInteger(key)) {
            long long idx = parseLongLongStrict(key, ins.opcode, "list index");
            const auto& list = std::get<List>(original.data);
            if (idx >= 0 && idx < static_cast<long long>(list.size()))
                result = list[static_cast<size_t>(idx)];
        }
        tape.current() = std::move(result);
    }
    else if (ins.opcode == "JSONHAS") {
        std::string key = stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
        const Value& original = tape.current();
        bool found = false;

        if (original.kind() == ValueKind::Map) {
            const auto& map = std::get<Map>(original.data);
            found = map.find(key) != map.end();
        } else if (original.kind() == ValueKind::List && isInteger(key)) {
            long long idx = parseLongLongStrict(key, ins.opcode, "list index");
            const auto& list = std::get<List>(original.data);
            found = idx >= 0 && idx < static_cast<long long>(list.size());
        }

        tape.current() = Value(found);
    }
    else if (ins.opcode == "JSONKEYS") {
        List out;
        if (tape.current().kind() == ValueKind::Map) {
            std::vector<std::string> keys;
            for (const auto& [key, _] : std::get<Map>(tape.current().data))
                keys.push_back(key);
            std::sort(keys.begin(), keys.end());
            for (const auto& key : keys)
                out.emplace_back(key);
        } else if (tape.current().kind() == ValueKind::List) {
            const auto& list = std::get<List>(tape.current().data);
            for (size_t i = 0; i < list.size(); ++i)
                out.emplace_back(static_cast<long long>(i));
        } else {
            throw std::runtime_error("JSONKEYS: current cell must be map or list");
        }
        tape.current() = Value(std::move(out));
    }
    else if (ins.opcode == "JSONPARSE") {
        std::string src = stringifyValue(tape.current());
        size_t pos = 0;
        tape.current() = parseJsonValue(src, pos);
    }
    else if (ins.opcode == "JSONLEN") {
        if (tape.current().kind() == ValueKind::Map) {
            tape.current() = Value(static_cast<long long>(std::get<Map>(tape.current().data).size()));
        } else if (tape.current().kind() == ValueKind::List) {
            tape.current() = Value(static_cast<long long>(std::get<List>(tape.current().data).size()));
        } else {
            tape.current() = Value(static_cast<long long>(0));
        }
    }
    else if (ins.opcode == "JSONSET") {
        std::string key = stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
        Value value = resolveOperand(argOrThrow(ins, 1), tape);

        if (tape.current().kind() == ValueKind::Nil)
            tape.current() = Value(Map{});

        if (tape.current().kind() == ValueKind::Map) {
            std::get<Map>(tape.current().data)[key] = value;
        } else if (tape.current().kind() == ValueKind::List && isInteger(key)) {
            long long idx = parseLongLongStrict(key, ins.opcode, "list index");
            if (idx < 0) throw std::runtime_error("JSONSET: negative list index");
            auto& list = std::get<List>(tape.current().data);
            if (idx >= static_cast<long long>(list.size()))
                list.resize(static_cast<size_t>(idx + 1));
            list[static_cast<size_t>(idx)] = value;
        } else {
            throw std::runtime_error("JSONSET: current cell must be map, list, or nil");
        }
    }
    else if (ins.opcode == "JSONDEL") {
        std::string key = stringifyValue(resolveOperand(argOrThrow(ins, 0), tape));
        if (tape.current().kind() == ValueKind::Nil) {
            // Deleting from nil is a no-op.
        } else if (tape.current().kind() == ValueKind::Map) {
            std::get<Map>(tape.current().data).erase(key);
        } else if (tape.current().kind() == ValueKind::List && isInteger(key)) {
            long long idx = parseLongLongStrict(key, ins.opcode, "list index");
            auto& list = std::get<List>(tape.current().data);
            if (idx >= 0 && idx < static_cast<long long>(list.size()))
                list.erase(list.begin() + idx);
        } else {
            throw std::runtime_error("JSONDEL: current cell must be map, list, or nil");
        }
    }
    else if (ins.opcode == "JSONPUSH") {
        Value value = ins.args.empty() ? tape.current() : resolveOperand(argOrThrow(ins, 0), tape);
        if (tape.current().kind() == ValueKind::Nil)
            tape.current() = Value(List{});
        if (tape.current().kind() != ValueKind::List)
            throw std::runtime_error("JSONPUSH: current cell must be list or nil");
        std::get<List>(tape.current().data).push_back(value);
    }
    else if (ins.opcode == "MAKEANSWERCODE") {
        Instruction setAnswer;
        setAnswer.opcode = "SET";
        setAnswer.args.push_back(answerLiteralToken(tape.current()));
        tape.current() = Value(Code{setAnswer});
    }
    else if (ins.opcode == "MAKEANSWERSOURCE") {
        tape.current() = Value(std::string("main:\n    SET ") +
                               answerLiteralToken(tape.current()) +
                               "\n    PRINT\n    RET\n");
    }

    // ---- Code manipulation -----------------------------------------------

    else if (ins.opcode == "CODELEN") {
        auto* code = std::get_if<Code>(&tape.current().data);
        if (!code) throw std::runtime_error("CODELEN: current cell must be Code");
        tape.current() = Value(static_cast<long long>(code->size()));
    }
    else if (ins.opcode == "CODEGET") {
        int idx = intOperand(argOrThrow(ins, 0), tape, ins.opcode, "instruction index");
        auto* code = std::get_if<Code>(&tape.current().data);
        if (!code) throw std::runtime_error("CODEGET: current cell must be Code");
        if (idx < 0 || idx >= (int)code->size())
            throw std::runtime_error("CODEGET: index out of bounds");
        const Instruction& instr = (*code)[idx];
        List lst;
        lst.emplace_back(instr.opcode);
        for (const auto& arg : instr.args)
            lst.emplace_back(arg);
        tape.current() = Value(std::move(lst));
    }
    else if (ins.opcode == "CODESET") {
        int instrIdx  = intOperand(argOrThrow(ins, 0), tape, ins.opcode, "instruction index");
        long long cellIdx = integerOperand(argOrThrow(ins, 1), tape, ins.opcode, "source cell index");
        auto* code = std::get_if<Code>(&tape.current().data);
        if (!code) throw std::runtime_error("CODESET: current cell must be Code");
        if (instrIdx < 0 || instrIdx >= (int)code->size())
            throw std::runtime_error("CODESET: instruction index out of bounds");
        const Value& src = tape.cells[cellIdx];
        if (src.kind() != ValueKind::List)
            throw std::runtime_error("CODESET: source cell must be a List [opcode, args...]");
        (*code)[instrIdx] = listToInstruction(std::get<List>(src.data));
    }
    else if (ins.opcode == "CODEAPPEND") {
        long long cellIdx = integerOperand(argOrThrow(ins, 0), tape, ins.opcode, "source cell index");
        auto* code = std::get_if<Code>(&tape.current().data);
        if (!code) throw std::runtime_error("CODEAPPEND: current cell must be Code");
        const Value& src = tape.cells[cellIdx];
        if (src.kind() != ValueKind::List)
            throw std::runtime_error("CODEAPPEND: source cell must be a List [opcode, args...]");
        code->push_back(listToInstruction(std::get<List>(src.data)));
    }

    // ---- List / Code append -----------------------------------------------
    // APPEND: append a value to a List cell, or an instruction (from List) to a Code cell.

    else if (ins.opcode == "APPEND") {
        Value operand = resolveOperand(argOrThrow(ins, 0), tape);
        if (tape.current().kind() == ValueKind::List) {
            std::get<List>(tape.current().data).push_back(operand);
        } else if (tape.current().kind() == ValueKind::Code) {
            if (operand.kind() != ValueKind::List)
                throw std::runtime_error("APPEND to Code: operand must be a List [opcode, args...]");
            std::get<Code>(tape.current().data).push_back(
                listToInstruction(std::get<List>(operand.data)));
        } else {
            throw std::runtime_error("APPEND: current cell must be List or Code");
        }
    }

    // ---- ML: Descriptive statistics ---------------------------------------

    else if (ins.opcode == "MEANVAL") {
        auto* list = std::get_if<List>(&tape.current().data);
        if (!list || list->empty())
            throw std::runtime_error("MEANVAL: current cell must be a non-empty list");
        Eigen::VectorXd pts = listToEigenVector(*list);
        tape.current() = Value(pts.mean());
    }
    else if (ins.opcode == "STDDEV") {
        auto* list = std::get_if<List>(&tape.current().data);
        if (!list || list->size() < 2)
            throw std::runtime_error("STDDEV: need at least 2 elements");
        Eigen::VectorXd pts = listToEigenVector(*list);
        double mean = pts.mean();
        double var = (pts.array() - mean).square().mean();
        tape.current() = Value(std::sqrt(var));
    }

    // ---- ML: Normalization ------------------------------------------------

    else if (ins.opcode == "NORMALIZE") {
        auto* list = std::get_if<List>(&tape.current().data);
        if (!list || list->empty())
            throw std::runtime_error("NORMALIZE: current cell must be a non-empty list");
        Eigen::VectorXd pts = listToEigenVector(*list);
        double mn = pts.minCoeff();
        double mx = pts.maxCoeff();
        double range = mx - mn;
        if (range == 0.0)
            pts.setZero();
        else
            pts = ((pts.array() - mn) / range).matrix();
        tape.current() = Value(eigenVectorToList(pts));
    }
    else if (ins.opcode == "ZSCORE") {
        auto* list = std::get_if<List>(&tape.current().data);
        if (!list || list->size() < 2)
            throw std::runtime_error("ZSCORE: need at least 2 elements");
        Eigen::VectorXd pts = listToEigenVector(*list);
        double mean = pts.mean();
        double sd = std::sqrt((pts.array() - mean).square().mean());
        if (sd == 0.0) throw std::runtime_error("ZSCORE: zero standard deviation");
        pts = ((pts.array() - mean) / sd).matrix();
        tape.current() = Value(eigenVectorToList(pts));
    }

    // ---- ML: Activation / probability ------------------------------------

    else if (ins.opcode == "SOFTMAX") {
        auto* list = std::get_if<List>(&tape.current().data);
        if (!list || list->empty())
            throw std::runtime_error("SOFTMAX: current cell must be a non-empty list");
        Eigen::VectorXd pts = listToEigenVector(*list);
        double maxVal = pts.maxCoeff();
        Eigen::VectorXd exps = (pts.array() - maxVal).exp().matrix();
        tape.current() = Value(eigenVectorToList(exps / exps.sum()));
    }

    // ---- ML: Linear algebra -----------------------------------------------

    else if (ins.opcode == "DOTPROD") {
        long long otherCell = integerOperand(argOrThrow(ins, 0), tape, ins.opcode, "other cell index");
        auto* listA = std::get_if<List>(&tape.current().data);
        auto* listB = std::get_if<List>(&tape.cells[otherCell].data);
        if (!listA || !listB)
            throw std::runtime_error("DOTPROD: both cells must contain lists");
        if (listA->size() != listB->size())
            throw std::runtime_error("DOTPROD: list size mismatch");
        Eigen::VectorXd a = listToEigenVector(*listA);
        Eigen::VectorXd b = listToEigenVector(*listB);
        tape.current() = Value(a.dot(b));
    }

    // ---- ML: Supervised learning ------------------------------------------

    else if (ins.opcode == "LINEARREG") {
        auto* list = std::get_if<List>(&tape.current().data);
        if (!list || list->size() < 2)
            throw std::runtime_error("LINEARREG: need at least 2 [x,y] pairs");
        std::vector<double> xs;
        std::vector<double> ys;
        xs.reserve(list->size());
        ys.reserve(list->size());
        for (const auto& pair : *list) {
            auto* p = std::get_if<List>(&pair.data);
            if (p && p->size() == 2) {
                xs.push_back(numericValue((*p)[0]));
                ys.push_back(numericValue((*p)[1]));
            }
        }
        if (xs.size() < 2) throw std::runtime_error("LINEARREG: not enough valid pairs");
        Eigen::Map<const Eigen::VectorXd> x(xs.data(), static_cast<Eigen::Index>(xs.size()));
        Eigen::Map<const Eigen::VectorXd> y(ys.data(), static_cast<Eigen::Index>(ys.size()));
        double xMean = x.mean();
        double yMean = y.mean();
        double denom = (x.array() - xMean).square().sum();
        if (denom == 0.0) throw std::runtime_error("LINEARREG: singular matrix (all x identical)");
        double slope = ((x.array() - xMean) * (y.array() - yMean)).sum() / denom;
        double intercept = yMean - slope * xMean;
        tape.current() = Value(List{Value(slope), Value(intercept)});
    }
    else if (ins.opcode == "PREDICT") {
        long long modelCell = integerOperand(argOrThrow(ins, 0), tape, ins.opcode, "model cell index");
        auto* model = std::get_if<List>(&tape.cells[modelCell].data);
        if (!model || model->size() < 2)
            throw std::runtime_error("PREDICT: model cell must contain [slope, intercept]");
        double slope     = numericValue((*model)[0]);
        double intercept = numericValue((*model)[1]);
        double x         = numericValue(tape.current());
        tape.current() = Value(slope * x + intercept);
    }

    // ---- ML: Clustering ---------------------------------------------------

    else if (ins.opcode == "KMEANS") {
        int k        = intOperand(argOrThrow(ins, 0), tape, ins.opcode, "cluster count");
        int maxIters = ins.args.size() > 1 ? intOperand(ins.args[1], tape, ins.opcode, "max iterations") : 100;

        auto* list = std::get_if<List>(&tape.current().data);
        if (!list || list->empty())
            throw std::runtime_error("KMEANS: current cell must be a non-empty list");
        if (k <= 0) throw std::runtime_error("KMEANS: k must be positive");

        Eigen::VectorXd pts = listToEigenVector(*list);
        if (k > static_cast<int>(pts.size())) k = static_cast<int>(pts.size());

        Eigen::VectorXd centroids(k);
        for (int i = 0; i < k; ++i)
            centroids(i) = pts((i * (pts.size() - 1)) / std::max(k - 1, 1));

        std::vector<int> assign(static_cast<size_t>(pts.size()), 0);

        for (int iter = 0; iter < maxIters; ++iter) {
            bool changed = false;
            for (Eigen::Index i = 0; i < pts.size(); ++i) {
                int best = 0;
                (centroids.array() - pts(i)).abs().minCoeff(&best);
                size_t idx = static_cast<size_t>(i);
                if (assign[idx] != best) { assign[idx] = best; changed = true; }
            }
            if (!changed) break;

            Eigen::VectorXd sums = Eigen::VectorXd::Zero(k);
            Eigen::VectorXi counts = Eigen::VectorXi::Zero(k);
            for (Eigen::Index i = 0; i < pts.size(); ++i) {
                int cluster = assign[static_cast<size_t>(i)];
                sums(cluster) += pts(i);
                counts(cluster) += 1;
            }
            for (int c = 0; c < k; ++c)
                if (counts(c) > 0) centroids(c) = sums(c) / counts(c);
        }

        tape.current() = Value(eigenVectorToList(centroids));
    }

    // ---- Function calls ---------------------------------------------------

    // ---- Argument registers: SETARG / GETARG / CLEARARGS / ARGCOUNT --------
    // Tape-oriented arg passing: caller writes to argRegs before CALL;
    // callee reads via GETARG. VM saves/restores argRegs across CALL/RET
    // so nested calls are transparent. Old inline ARG system is preserved.

    else if (ins.opcode == "SETARG") {
        size_t n = sizeOperand(argOrThrow(ins, 0), tape, ins.opcode, "argument register index");
        Value v = ins.args.size() > 1 ? resolveOperand(ins.args[1], tape) : tape.current();
        if (argRegs.size() <= n) argRegs.resize(n + 1);
        argRegs[n] = v;
    }
    else if (ins.opcode == "GETARG") {
        size_t n = sizeOperand(argOrThrow(ins, 0), tape, ins.opcode, "argument register index");
        tape.current() = n < argRegs.size() ? argRegs[n] : Value();
    }
    else if (ins.opcode == "CLEAR_ARGS" || ins.opcode == "CLEARARGS") {
        argRegs.clear();
    }
    else if (ins.opcode == "ARGCOUNT") {
        tape.current() = Value(static_cast<long long>(argRegs.size()));
    }

    else if (ins.opcode == "SHOW_ARGS")
    {
        if (argRegs.empty())
            throw std::runtime_error("SHOW_ARGS used outside of a function call");
        for (size_t i = 0; i < argRegs.size(); ++i) {
            std::cout << "ARG " << i << ": " << stringifyValue(argRegs[i]) << "\n";
        }
    }
    else if (ins.opcode == "CALL") {
        Value target = resolveOperand(argOrThrow(ins, 0), tape);
        Frame frame;
        frame.functionName = target.kind() == ValueKind::Str
            ? std::get<std::string>(target.data)
            : stringifyValue(target);
        for (size_t i = 1; i < ins.args.size(); ++i)
            frame.args.push_back(resolveOperand(ins.args[i], tape));
        // Save arg registers; callee inherits them (reads via GETARG),
        // may overwrite for sub-calls. Restored to caller's snapshot on RET.
        frame.savedArgRegs = argRegs;
        callStack.push(frame);
        try {
            if (target.kind() == ValueKind::Code) {
                runBlock(std::get<Code>(target.data));
            } else {
                int moduleTape = 0;
                long long functionCell = 0;
                if (resolveTapeFunction(frame.functionName, moduleTape, functionCell))
                    callTapeFunction(moduleTape, functionCell, frame.functionName);
                else
                    run(frame.functionName);
            }
        } catch (...) {
            argRegs = callStack.top().savedArgRegs;
            callStack.pop();
            throw;
        }
        argRegs = callStack.top().savedArgRegs;
        callStack.pop();
    }
    else if (ins.opcode == "RET") {
        return;
    }

    // ---- JMP family: must be intercepted by run() — error if reached here

    else if (ins.opcode == "JMP" || ins.opcode == "JMPIF" || ins.opcode == "JMPNOT") {
        throw std::runtime_error(ins.opcode + " used outside a named function (not supported in EXEC blocks)");
    }

    // ---- Unknown opcode ---------------------------------------------------

    else {
        throw std::runtime_error("Unknown opcode: " + ins.opcode);
    }
}

// ---------------------------------------------------------------------------
// runBlock — execute a Code value (no local labels, no JMP)
// ---------------------------------------------------------------------------

void VM::runBlock(const Code& code) {
    for (const auto& ins : code) {
        if (halted) return;
        if (ins.opcode == "RET") return;
        execute(ins);
    }
}

void VM::loadSource(const std::string& source, const std::filesystem::path& baseDir) {
    loader.appendSource(source, baseDir);
}

// ---------------------------------------------------------------------------
// run — execute a named function; handles JMP/JMPIF/JMPNOT via pc manipulation
// ---------------------------------------------------------------------------

void VM::run(const std::string& entry) {
    if (halted)
        return;

    Function fn = loader.loadFunction(entry);
    size_t pc = 0;

    while (!halted && pc < fn.instructions.size()) {
        const auto& ins = fn.instructions[pc];

        if (ins.opcode == "RET") return;

        if (ins.opcode == "JMP") {
            std::string label = normalizeJumpLabel(ins);
            auto it = fn.localLabels.find(label);
            if (it == fn.localLabels.end())
                throw std::runtime_error("JMP: undefined label '" + ins.args[0] + "'");
            pc = it->second;
            continue;
        }

        if (ins.opcode == "JMPIF") {
            if (isTruthy(tapes[activeTape].current())) {
                std::string label = normalizeJumpLabel(ins);
                auto it = fn.localLabels.find(label);
                if (it == fn.localLabels.end())
                    throw std::runtime_error("JMPIF: undefined label '" + ins.args[0] + "'");
                pc = it->second;
                continue;
            }
            ++pc;
            continue;
        }

        if (ins.opcode == "JMPNOT") {
            if (!isTruthy(tapes[activeTape].current())) {
                std::string label = normalizeJumpLabel(ins);
                auto it = fn.localLabels.find(label);
                if (it == fn.localLabels.end())
                    throw std::runtime_error("JMPNOT: undefined label '" + ins.args[0] + "'");
                pc = it->second;
                continue;
            }
            ++pc;
            continue;
        }

        execute(ins);
        ++pc;
    }
}
