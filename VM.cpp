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
#include <unordered_set>

// ---------------------------------------------------------------------------
// Internal helpers (anonymous namespace)
// ---------------------------------------------------------------------------
namespace {

constexpr long long TAPE_NAME_CELL = -2;
constexpr long long EMO_HORMONES_CELL = -20;
constexpr long long EMO_EVENTS_CELL = -21;
constexpr long long EMO_CLOCK_CELL = -22;
constexpr long long ROUTE_OUT_CELL = -30;
constexpr long long ROUTE_IN_CELL = -31;
constexpr long long ROUTE_RESOURCE_INDEX_CELL = -32;
constexpr long long ROUTE_NEXT_CELL = -33;
constexpr long long ROUTE_OBSERVATION_LOG_CELL = -34;
constexpr long long ROUTE_FRAME_INDEX_CELL = -35;

// ---- String utilities ------------------------------------------------------

std::string stringifyValue(const Value& value);
std::string stringifyJsonValue(const Value& value);
std::string compactDisplayValue(const Value& value, size_t maxWidth);
const std::string& argOrThrow(const Instruction& ins, size_t index);
Value resolveOperand(const std::string& arg, Tape& tape);
long long integerValue(const Value& value, const std::string& opcode, const std::string& role);
bool isTruthy(const Value& v);
std::vector<std::string> textTokens(const std::string& text);
List stringList(const std::vector<std::string>& items);
Value indexLookupAddress(const Tape& tape, const std::string& key);

struct RouteAddress {
    int tape = 0;
    long long cell = 0;
};

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

std::string toLowerStr(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
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

bool isConditionOperator(const std::string& op) {
    return op == ">" || op == ">=" || op == "<" || op == "<=" ||
           op == "==" || op == "!=";
}

double percentValue(const Value& value, const std::string& opcode, const std::string& role) {
    if (value.kind() == ValueKind::Str) {
        std::string text = trimStr(std::get<std::string>(value.data));
        if (!text.empty() && text.back() == '%')
            text.pop_back();
        text = trimStr(text);
        if (isInteger(text) || isFloat(text))
            return parseDoubleStrict(text, opcode, role);
    }
    return doubleValue(value, opcode, role);
}

double percentOperand(const std::string& arg, Tape& tape, const std::string& opcode, const std::string& role) {
    return percentValue(resolveOperand(arg, tape), opcode, role);
}

Map& mapCell(Tape& tape, long long cell, const std::string& opcode, const std::string& role) {
    Value& value = tape.cells[cell];
    if (value.kind() == ValueKind::Nil)
        value = Value(Map{});
    if (value.kind() != ValueKind::Map)
        throw std::runtime_error(opcode + ": " + role + " must be a map");
    return std::get<Map>(value.data);
}

long long emotionTick(const Tape& tape) {
    auto it = tape.cells.find(EMO_CLOCK_CELL);
    if (it == tape.cells.end() || it->second.kind() == ValueKind::Nil)
        return 0;
    return integerValue(it->second, "EMO", "clock");
}

double mapNumber(const Map& map, const std::string& key, double fallback) {
    auto it = map.find(key);
    if (it == map.end())
        return fallback;
    return doubleValue(it->second, "EMO", key);
}

long long mapInteger(const Map& map, const std::string& key, long long fallback) {
    auto it = map.find(key);
    if (it == map.end())
        return fallback;
    return integerValue(it->second, "EMO", key);
}

std::string mapString(const Map& map, const std::string& key, const std::string& fallback) {
    auto it = map.find(key);
    if (it == map.end())
        return fallback;
    return stringifyValue(it->second);
}

std::string canonicalRouteAddress(const RouteAddress& address) {
    return std::to_string(address.tape) + "." + std::to_string(address.cell);
}

bool looksLikeRouteAddress(const std::string& text) {
    std::string value = trimStr(text);
    if (!value.empty() && value.front() == '@')
        value = value.substr(1);
    size_t dot = value.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= value.size())
        return false;
    return isInteger(trimStr(value.substr(dot + 1)));
}

RouteAddress parseRouteAddressText(
    const std::string& raw,
    const std::function<int(const std::string&)>& resolveTape,
    const std::string& opcode
) {
    std::string value = trimStr(raw);
    if (!value.empty() && value.front() == '@')
        value = trimStr(value.substr(1));

    size_t dot = value.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= value.size()) {
        throw std::runtime_error(opcode + ": route address must be tape.cell, got '" + raw + "'");
    }

    std::string tapeSelector = trimStr(value.substr(0, dot));
    std::string cellText = trimStr(value.substr(dot + 1));
    if (tapeSelector.empty())
        throw std::runtime_error(opcode + ": route address has empty tape selector: '" + raw + "'");
    if (!isInteger(cellText))
        throw std::runtime_error(opcode + ": route cell must be an integer: '" + raw + "'");

    return RouteAddress{
        resolveTape(tapeSelector),
        parseLongLongStrict(cellText, opcode, "route cell")
    };
}

std::string routeAddressTextFromArg(const Instruction& ins, size_t index, Tape& tape) {
    std::string token = trimStr(argOrThrow(ins, index));

    if (!token.empty() && token.front() == '@') {
        std::string rest = trimStr(token.substr(1));
        if (looksLikeRouteAddress(rest))
            return rest;
        if (isInteger(rest))
            return stringifyValue(tape.cells[parseLongLongStrict(rest, ins.opcode, "route address cell")]);
        return rest;
    }

    if (looksLikeRouteAddress(token))
        return token;

    return stringifyValue(resolveOperand(token, tape));
}

bool routeArgLooksAddress(const Instruction& ins, size_t index, Tape& tape) {
    try {
        return looksLikeRouteAddress(routeAddressTextFromArg(ins, index, tape));
    } catch (...) {
        return false;
    }
}

RouteAddress routeAddressArg(
    const Instruction& ins,
    size_t index,
    Tape& tape,
    const std::function<int(const std::string&)>& resolveTape
) {
    return parseRouteAddressText(routeAddressTextFromArg(ins, index, tape), resolveTape, ins.opcode);
}

Value readTapeCellOrNil(const Tape& tape, long long cell) {
    auto it = tape.cells.find(cell);
    return it == tape.cells.end() ? Value() : it->second;
}

Value resolveRoutedOperand(
    const std::string& arg,
    Tape& tape,
    std::vector<Tape>& tapes,
    const std::function<int(const std::string&)>& resolveTape,
    const std::string& opcode
) {
    std::string token = trimStr(arg);
    if (!token.empty() && token.front() == '@') {
        std::string rest = trimStr(token.substr(1));
        if (looksLikeRouteAddress(rest)) {
            RouteAddress address = parseRouteAddressText(rest, resolveTape, opcode);
            return readTapeCellOrNil(tapes[static_cast<size_t>(address.tape)], address.cell);
        }
    }
    return resolveOperand(arg, tape);
}

List& routeListForCell(Map& registry, long long cell, const std::string& opcode, const std::string& role) {
    Value& value = registry[std::to_string(cell)];
    if (value.kind() == ValueKind::Nil)
        value = Value(List{});
    if (value.kind() != ValueKind::List)
        throw std::runtime_error(opcode + ": " + role + " route slot must be a list");
    return std::get<List>(value.data);
}

bool sameRouteLink(const Value& value, const std::string& peerKey, const std::string& peer, const std::string& relation) {
    if (value.kind() != ValueKind::Map)
        return false;
    const Map& link = std::get<Map>(value.data);
    return mapString(link, peerKey, "") == peer &&
           mapString(link, "relation", "link") == relation;
}

Map makeRouteLink(
    const RouteAddress& from,
    const RouteAddress& to,
    const Tape& fromTape,
    const Tape& toTape,
    const std::string& relation,
    double strength,
    long long hits
) {
    Map link;
    link["from"] = Value(canonicalRouteAddress(from));
    link["from_tape"] = Value(static_cast<long long>(from.tape));
    link["from_tape_name"] = Value(fromTape.name);
    link["from_cell"] = Value(from.cell);
    link["to"] = Value(canonicalRouteAddress(to));
    link["to_tape"] = Value(static_cast<long long>(to.tape));
    link["to_tape_name"] = Value(toTape.name);
    link["to_cell"] = Value(to.cell);
    link["relation"] = Value(relation.empty() ? std::string("link") : relation);
    link["strength"] = Value(std::max(0.0, strength));
    link["hits"] = Value(hits);
    return link;
}

void replaceRouteLink(List& links, const Map& link, const std::string& peerKey, const std::string& relation) {
    std::string peer = mapString(link, peerKey, "");
    for (Value& value : links) {
        if (sameRouteLink(value, peerKey, peer, relation)) {
            value = Value(link);
            return;
        }
    }
    links.emplace_back(link);
}

Map upsertRouteLink(
    std::vector<Tape>& tapes,
    const RouteAddress& from,
    const RouteAddress& to,
    const std::string& relationArg,
    double strength,
    bool addDelta,
    const std::string& opcode
) {
    std::string relation = relationArg.empty() ? std::string("link") : relationArg;
    std::string toKey = canonicalRouteAddress(to);
    Map& outbound = mapCell(tapes[static_cast<size_t>(from.tape)], ROUTE_OUT_CELL, opcode, "outbound route registry");
    List& outgoing = routeListForCell(outbound, from.cell, opcode, "outbound");

    double nextStrength = std::max(0.0, strength);
    long long hits = 1;
    for (const Value& value : outgoing) {
        if (!sameRouteLink(value, "to", toKey, relation))
            continue;
        const Map& existing = std::get<Map>(value.data);
        double oldStrength = mapNumber(existing, "strength", 0.0);
        nextStrength = addDelta ? std::max(0.0, oldStrength + strength) : std::max(0.0, strength);
        hits = mapInteger(existing, "hits", 0) + 1;
        break;
    }

    Map link = makeRouteLink(
        from,
        to,
        tapes[static_cast<size_t>(from.tape)],
        tapes[static_cast<size_t>(to.tape)],
        relation,
        nextStrength,
        hits
    );
    if (addDelta)
        link["last_delta"] = Value(strength);

    replaceRouteLink(outgoing, link, "to", relation);

    Map& inbound = mapCell(tapes[static_cast<size_t>(to.tape)], ROUTE_IN_CELL, opcode, "inbound route registry");
    List& incoming = routeListForCell(inbound, to.cell, opcode, "inbound");
    replaceRouteLink(incoming, link, "from", relation);

    return link;
}

List routeLinksFor(
    const std::vector<Tape>& tapes,
    const RouteAddress& from,
    const std::string& relation,
    size_t limit
) {
    List matches;
    const Tape& sourceTape = tapes[static_cast<size_t>(from.tape)];
    auto registryIt = sourceTape.cells.find(ROUTE_OUT_CELL);
    if (registryIt == sourceTape.cells.end() || registryIt->second.kind() != ValueKind::Map)
        return matches;

    const Map& registry = std::get<Map>(registryIt->second.data);
    auto slotIt = registry.find(std::to_string(from.cell));
    if (slotIt == registry.end() || slotIt->second.kind() != ValueKind::List)
        return matches;

    for (const Value& value : std::get<List>(slotIt->second.data)) {
        if (value.kind() != ValueKind::Map)
            continue;
        const Map& link = std::get<Map>(value.data);
        if (!relation.empty() && relation != "*" && mapString(link, "relation", "link") != relation)
            continue;
        matches.push_back(value);
    }

    std::sort(matches.begin(), matches.end(), [](const Value& a, const Value& b) {
        const Map& left = std::get<Map>(a.data);
        const Map& right = std::get<Map>(b.data);
        double leftStrength = mapNumber(left, "strength", 0.0);
        double rightStrength = mapNumber(right, "strength", 0.0);
        if (leftStrength != rightStrength)
            return leftStrength > rightStrength;
        return mapString(left, "to", "") < mapString(right, "to", "");
    });

    if (limit > 0 && matches.size() > limit)
        matches.resize(limit);
    return matches;
}

List inboundRouteLinksFor(const std::vector<Tape>& tapes, const RouteAddress& to) {
    List matches;
    const Tape& targetTape = tapes[static_cast<size_t>(to.tape)];
    auto registryIt = targetTape.cells.find(ROUTE_IN_CELL);
    if (registryIt == targetTape.cells.end() || registryIt->second.kind() != ValueKind::Map)
        return matches;

    const Map& registry = std::get<Map>(registryIt->second.data);
    auto slotIt = registry.find(std::to_string(to.cell));
    if (slotIt == registry.end() || slotIt->second.kind() != ValueKind::List)
        return matches;

    matches = std::get<List>(slotIt->second.data);
    std::sort(matches.begin(), matches.end(), [](const Value& a, const Value& b) {
        const Map& left = std::get<Map>(a.data);
        const Map& right = std::get<Map>(b.data);
        double leftStrength = mapNumber(left, "strength", 0.0);
        double rightStrength = mapNumber(right, "strength", 0.0);
        if (leftStrength != rightStrength)
            return leftStrength > rightStrength;
        return mapString(left, "from", "") < mapString(right, "from", "");
    });
    return matches;
}

struct RouteRelationStrength {
    std::string relation = "link";
    double strength = 1.0;
};

RouteRelationStrength routeRelationStrengthArgs(
    const Instruction& ins,
    size_t start,
    Tape& tape,
    double defaultStrength
) {
    RouteRelationStrength out;
    out.strength = defaultStrength;

    if (ins.args.size() <= start)
        return out;

    bool firstIsStrength = false;
    try {
        (void)doubleOperand(argOrThrow(ins, start), tape, ins.opcode, "route strength");
        firstIsStrength = true;
    } catch (...) {
        firstIsStrength = false;
    }

    if (firstIsStrength) {
        out.strength = doubleOperand(argOrThrow(ins, start), tape, ins.opcode, "route strength");
    } else {
        out.relation = stringOperand(argOrThrow(ins, start), tape);
    }

    if (ins.args.size() > start + 1)
        out.strength = doubleOperand(argOrThrow(ins, start + 1), tape, ins.opcode, "route strength");

    if (out.relation.empty())
        out.relation = "link";
    return out;
}

bool routeLearningStopword(const std::string& token) {
    static const std::unordered_set<std::string> words = {
        "a", "an", "the", "this", "that", "these", "those",
        "i", "me", "my", "we", "our", "you", "your", "he", "she", "they",
        "it", "its", "is", "am", "are", "was", "were", "be", "been",
        "have", "has", "had", "do", "does", "did", "to", "of", "for",
        "and", "or", "but", "if", "then", "with", "without", "using",
        "use", "used", "in", "on", "at", "from", "by", "as", "than",
        "what", "which", "who", "whom", "when", "where", "why", "how"
    };
    return words.count(token) > 0;
}

std::string routeKeyFromTokens(const std::vector<std::string>& tokens, size_t start, size_t count) {
    std::string out;
    for (size_t i = 0; i < count; ++i) {
        if (i > 0)
            out.push_back('_');
        out += tokens[start + i];
    }
    return out;
}

std::string routeLabelFromKey(const std::string& key) {
    std::string out = key;
    std::replace(out.begin(), out.end(), '_', ' ');
    return out;
}

std::vector<std::string> uniqueOrdered(const std::vector<std::string>& values) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const auto& value : values) {
        if (value.empty() || seen.count(value))
            continue;
        seen.insert(value);
        out.push_back(value);
    }
    return out;
}

std::vector<std::string> routeContentTokens(const std::string& text) {
    std::vector<std::string> out;
    for (const auto& token : textTokens(text)) {
        if (routeLearningStopword(token))
            continue;
        if (token.size() == 1 && !std::isdigit(static_cast<unsigned char>(token[0])))
            continue;
        out.push_back(token);
    }
    return out;
}

std::vector<std::string> routeConceptKeys(const std::string& text) {
    std::vector<std::string> rawTokens = textTokens(text);
    std::vector<std::string> tokens = routeContentTokens(text);
    std::vector<std::string> concepts;
    concepts.reserve(tokens.size() * 3);

    for (const auto& token : tokens)
        concepts.push_back(token);

    for (size_t i = 0; i + 1 < rawTokens.size(); ++i) {
        if (!routeLearningStopword(rawTokens[i]) && !routeLearningStopword(rawTokens[i + 1]))
            concepts.push_back(routeKeyFromTokens(rawTokens, i, 2));
    }

    for (size_t i = 0; i + 2 < rawTokens.size(); ++i) {
        if (!routeLearningStopword(rawTokens[i]) &&
            !routeLearningStopword(rawTokens[i + 1]) &&
            !routeLearningStopword(rawTokens[i + 2])) {
            concepts.push_back(routeKeyFromTokens(rawTokens, i, 3));
        }
    }

    return uniqueOrdered(concepts);
}

bool routeHasToken(const std::vector<std::string>& tokens, const std::string& token) {
    return std::find(tokens.begin(), tokens.end(), token) != tokens.end();
}

bool routeHasConcept(const std::vector<std::string>& concepts, const std::string& key) {
    return std::find(concepts.begin(), concepts.end(), key) != concepts.end();
}

long long routeAllocateCell(Tape& tape) {
    long long next = 0;
    auto it = tape.cells.find(ROUTE_NEXT_CELL);
    if (it != tape.cells.end() && it->second.kind() != ValueKind::Nil) {
        next = integerValue(it->second, "ROUTE_LEARN", "next route cell");
    } else {
        for (const auto& [cell, _] : tape.cells) {
            if (cell >= 0)
                next = std::max(next, cell + 1);
        }
    }

    while (tape.cells.find(next) != tape.cells.end())
        ++next;

    tape.cells[ROUTE_NEXT_CELL] = Value(next + 1);
    return next;
}

RouteAddress parseCanonicalRouteAddress(const std::string& raw, const std::string& opcode) {
    auto numericResolver = [&](const std::string& selector) {
        return parseIntStrict(selector, opcode, "canonical route tape");
    };
    return parseRouteAddressText(raw, numericResolver, opcode);
}

RouteAddress routeEnsureResource(
    std::vector<Tape>& tapes,
    int memoryTape,
    const std::string& key,
    const std::string& label,
    const std::string& resourceKind,
    const RouteAddress& observation,
    const std::string& opcode,
    bool& created
) {
    Tape& tape = tapes[static_cast<size_t>(memoryTape)];
    Map& index = mapCell(tape, ROUTE_RESOURCE_INDEX_CELL, opcode, "resource index");
    auto found = index.find(key);
    RouteAddress address;
    created = false;

    if (found != index.end() && found->second.kind() != ValueKind::Nil) {
        address = parseCanonicalRouteAddress(stringifyValue(found->second), opcode);
    } else {
        long long cell = routeAllocateCell(tape);
        address = RouteAddress{memoryTape, cell};
        index[key] = Value(canonicalRouteAddress(address));
        created = true;
    }

    Value& cellValue = tapes[static_cast<size_t>(address.tape)].cells[address.cell];
    if (cellValue.kind() != ValueKind::Map) {
        Map resource;
        resource["kind"] = Value(std::string("resource"));
        resource["resource_kind"] = Value(resourceKind);
        resource["key"] = Value(key);
        resource["label"] = Value(label);
        resource["created_by"] = Value(opcode);
        resource["observation_count"] = Value(static_cast<long long>(0));
        resource["evidence"] = Value(List{});
        cellValue = Value(std::move(resource));
    }

    Map& resource = std::get<Map>(cellValue.data);
    resource["kind"] = Value(std::string("resource"));
    std::string existingKind = mapString(resource, "resource_kind", "");
    bool preserveSpecialKind = !existingKind.empty() &&
        existingKind != "concept" && existingKind != "phrase" &&
        (resourceKind == "concept" || resourceKind == "phrase");
    if (!preserveSpecialKind)
        resource["resource_kind"] = Value(resourceKind);
    resource["key"] = Value(key);
    if (!label.empty())
        resource["label"] = Value(label);

    if (observation.cell >= 0) {
        resource["last_observation"] = Value(canonicalRouteAddress(observation));
        resource["observation_count"] = Value(mapInteger(resource, "observation_count", 0) + 1);

        Value& evidenceValue = resource["evidence"];
        if (evidenceValue.kind() == ValueKind::Nil)
            evidenceValue = Value(List{});
        if (evidenceValue.kind() == ValueKind::List) {
            List& evidence = std::get<List>(evidenceValue.data);
            if (evidence.size() < 24)
                evidence.emplace_back(canonicalRouteAddress(observation));
        }
    }

    return address;
}

RouteAddress routeEnsureModelResource(
    std::vector<Tape>& tapes,
    int memoryTape,
    const std::string& key,
    const std::string& kind,
    const std::string& label,
    const std::string& opcode,
    bool* createdOut = nullptr
) {
    bool created = false;
    RouteAddress address = routeEnsureResource(
        tapes,
        memoryTape,
        key,
        label.empty() ? routeLabelFromKey(key) : label,
        kind.empty() ? std::string("concept") : kind,
        RouteAddress{memoryTape, -1},
        opcode,
        created
    );
    if (createdOut)
        *createdOut = created;
    return address;
}

List& listField(Map& map, const std::string& field) {
    Value& value = map[field];
    if (value.kind() == ValueKind::Nil)
        value = Value(List{});
    if (value.kind() != ValueKind::List)
        throw std::runtime_error("ROUTE: field must be a list: " + field);
    return std::get<List>(value.data);
}

void upsertMapListItem(List& list, const std::string& keyField, const std::string& key, const Map& item) {
    for (Value& value : list) {
        if (value.kind() != ValueKind::Map)
            continue;
        Map& existing = std::get<Map>(value.data);
        if (mapString(existing, keyField, "") == key) {
            value = Value(item);
            return;
        }
    }
    list.emplace_back(item);
}

RouteAddress routeEnsureFrame(
    std::vector<Tape>& tapes,
    int memoryTape,
    const std::string& key,
    const std::string& decision,
    const std::string& question,
    const std::string& opcode
) {
    bool created = false;
    RouteAddress frame = routeEnsureModelResource(tapes, memoryTape, key, "frame", routeLabelFromKey(key), opcode, &created);
    Map& frameIndex = mapCell(tapes[static_cast<size_t>(memoryTape)], ROUTE_FRAME_INDEX_CELL, opcode, "frame index");
    frameIndex[key] = Value(canonicalRouteAddress(frame));

    Value& frameValue = tapes[static_cast<size_t>(frame.tape)].cells[frame.cell];
    Map& frameMap = std::get<Map>(frameValue.data);
    frameMap["resource_kind"] = Value(std::string("frame"));
    if (!decision.empty())
        frameMap["decision"] = Value(decision);
    else if (frameMap.find("decision") == frameMap.end())
        frameMap["decision"] = Value(std::string("route_frame"));
    if (!question.empty())
        frameMap["question"] = Value(question);
    listField(frameMap, "triggers");
    listField(frameMap, "missing_slots");
    frameMap["frame_key"] = Value(key);
    return frame;
}

std::string routeAddressForKey(const Tape& tape, const std::string& key) {
    Value addressValue = indexLookupAddress(tape, key);
    if (addressValue.kind() == ValueKind::Nil)
        return "";
    return stringifyValue(addressValue);
}

long long routeClock(Tape& tape) {
    auto it = tape.cells.find(EMO_CLOCK_CELL);
    if (it == tape.cells.end() || it->second.kind() == ValueKind::Nil)
        return 0;
    return integerValue(it->second, "ROUTE", "clock");
}

long long bumpRouteClock(Tape& tape) {
    long long next = routeClock(tape) + 1;
    tape.cells[EMO_CLOCK_CELL] = Value(next);
    return next;
}

std::string cacheStateForHeat(double heat, double hotThreshold, double coldThreshold) {
    if (heat >= hotThreshold)
        return "hot";
    if (heat <= coldThreshold)
        return "cold";
    return "warm";
}

RouteAddress resolveRouteKeyOrAddress(
    const std::vector<Tape>& tapes,
    int memoryTape,
    const std::string& raw,
    const std::string& opcode
) {
    std::string value = trimStr(raw);
    if (looksLikeRouteAddress(value))
        return parseCanonicalRouteAddress(value, opcode);

    if (memoryTape < 0 || memoryTape >= static_cast<int>(tapes.size()))
        throw std::runtime_error(opcode + ": memory tape out of bounds");

    Value addressValue = indexLookupAddress(tapes[static_cast<size_t>(memoryTape)], value);
    if (addressValue.kind() == ValueKind::Nil)
        throw std::runtime_error(opcode + ": unknown route key or address: " + raw);
    return parseCanonicalRouteAddress(stringifyValue(addressValue), opcode);
}

Map touchRouteCell(std::vector<Tape>& tapes, const RouteAddress& address, double amount, const std::string& opcode) {
    if (address.tape < 0 || address.tape >= static_cast<int>(tapes.size()))
        throw std::runtime_error(opcode + ": route tape out of bounds");

    Tape& tape = tapes[static_cast<size_t>(address.tape)];
    long long tick = bumpRouteClock(tape);
    Value& value = tape.cells[address.cell];
    if (value.kind() == ValueKind::Nil)
        value = Value(Map{});
    if (value.kind() != ValueKind::Map)
        throw std::runtime_error(opcode + ": route use expects a map resource/observation cell");

    Map& map = std::get<Map>(value.data);
    double heat = std::max(0.0, mapNumber(map, "heat", 0.0) + amount);
    long long accessCount = mapInteger(map, "access_count", 0) + 1;
    map["heat"] = Value(heat);
    map["access_count"] = Value(accessCount);
    map["last_access_tick"] = Value(tick);
    map["cache_state"] = Value(cacheStateForHeat(heat, 3.0, 0.25));

    Map out;
    out["address"] = Value(canonicalRouteAddress(address));
    out["heat"] = Value(heat);
    out["access_count"] = Value(accessCount);
    out["cache_state"] = map["cache_state"];
    out["tick"] = Value(tick);
    return out;
}

List routeFramesForContext(const std::vector<Tape>& tapes,
                           const Tape& tape,
                           const std::unordered_set<std::string>& inputConcepts,
                           const std::unordered_set<std::string>& matchedAddresses,
                           const std::string& opcode) {
    List frames;
    auto frameIndexIt = tape.cells.find(ROUTE_FRAME_INDEX_CELL);
    if (frameIndexIt == tape.cells.end() || frameIndexIt->second.kind() != ValueKind::Map)
        return frames;

    const Map& frameIndex = std::get<Map>(frameIndexIt->second.data);
    for (const auto& [frameKey, addressValue] : frameIndex) {
        RouteAddress frameAddress = parseCanonicalRouteAddress(stringifyValue(addressValue), opcode);
        Value frameValue = readTapeCellOrNil(tapes[static_cast<size_t>(frameAddress.tape)], frameAddress.cell);
        if (frameValue.kind() != ValueKind::Map)
            continue;

        const Map& frameMap = std::get<Map>(frameValue.data);
        auto triggerIt = frameMap.find("triggers");
        if (triggerIt == frameMap.end() || triggerIt->second.kind() != ValueKind::List)
            continue;

        List matchedTriggers;
        List missingTriggers;
        double score = 0.0;
        long long required = 0;
        for (const Value& triggerValue : std::get<List>(triggerIt->second.data)) {
            if (triggerValue.kind() != ValueKind::Map)
                continue;
            const Map& trigger = std::get<Map>(triggerValue.data);
            std::string key = mapString(trigger, "key", "");
            std::string address = mapString(trigger, "address", "");
            double strength = mapNumber(trigger, "strength", 1.0);
            bool matched = (!key.empty() && inputConcepts.count(key) > 0) ||
                           (!address.empty() && matchedAddresses.count(address) > 0);
            Map item = trigger;
            item["matched"] = Value(matched);
            ++required;
            if (matched) {
                score += strength;
                matchedTriggers.emplace_back(std::move(item));
            } else {
                missingTriggers.emplace_back(std::move(item));
            }
        }

        Map frame;
        frame["key"] = Value(frameKey);
        frame["address"] = Value(canonicalRouteAddress(frameAddress));
        frame["decision"] = Value(mapString(frameMap, "decision", "route_frame"));
        frame["question"] = Value(mapString(frameMap, "question", ""));
        frame["score"] = Value(score);
        frame["required"] = Value(required);
        frame["matched_triggers"] = Value(std::move(matchedTriggers));
        frame["missing_triggers"] = Value(std::move(missingTriggers));
        frame["complete"] = Value(required > 0 && std::get<List>(frame["missing_triggers"].data).empty());
        frames.emplace_back(std::move(frame));
    }

    std::sort(frames.begin(), frames.end(), [](const Value& a, const Value& b) {
        const Map& left = std::get<Map>(a.data);
        const Map& right = std::get<Map>(b.data);
        bool leftComplete = mapString(left, "complete", "false") == "true";
        bool rightComplete = mapString(right, "complete", "false") == "true";
        if (leftComplete != rightComplete)
            return leftComplete > rightComplete;
        double leftScore = mapNumber(left, "score", 0.0);
        double rightScore = mapNumber(right, "score", 0.0);
        if (leftScore != rightScore)
            return leftScore > rightScore;
        return mapString(left, "key", "") < mapString(right, "key", "");
    });

    return frames;
}

Map routeContextText(const std::vector<Tape>& tapes,
                     int memoryTape,
                     const std::string& text,
                     size_t linkLimit,
                     const std::string& opcode) {
    if (memoryTape < 0 || memoryTape >= static_cast<int>(tapes.size()))
        throw std::runtime_error(opcode + ": memory tape out of bounds");

    const Tape& tape = tapes[static_cast<size_t>(memoryTape)];
    std::vector<std::string> tokens = textTokens(text);
    std::vector<std::string> concepts = routeConceptKeys(text);
    std::unordered_set<std::string> inputConcepts(concepts.begin(), concepts.end());
    std::unordered_set<std::string> matchedAddresses;

    List matched;
    List unresolved;
    List associations;
    for (const auto& key : concepts) {
        Value addressValue = indexLookupAddress(tape, key);
        if (addressValue.kind() == ValueKind::Nil) {
            unresolved.emplace_back(key);
            continue;
        }

        std::string addressText = stringifyValue(addressValue);
        matchedAddresses.insert(addressText);
        RouteAddress address = parseCanonicalRouteAddress(addressText, opcode);
        Value cellValue = readTapeCellOrNil(tapes[static_cast<size_t>(address.tape)], address.cell);

        Map item;
        item["key"] = Value(key);
        item["address"] = Value(addressText);
        item["value"] = cellValue;
        matched.emplace_back(item);

        List outgoing = routeLinksFor(tapes, address, "", linkLimit);
        for (const Value& linkValue : outgoing) {
            if (linkValue.kind() != ValueKind::Map)
                continue;
            Map link = std::get<Map>(linkValue.data);
            RouteAddress target{
                static_cast<int>(mapInteger(link, "to_tape", 0)),
                mapInteger(link, "to_cell", 0)
            };
            link["source_key"] = Value(key);
            link["value"] = target.tape >= 0 && target.tape < static_cast<int>(tapes.size())
                ? readTapeCellOrNil(tapes[static_cast<size_t>(target.tape)], target.cell)
                : Value();
            associations.emplace_back(std::move(link));
        }
    }

    List frames = routeFramesForContext(tapes, tape, inputConcepts, matchedAddresses, opcode);

    Map out;
    out["mode"] = Value(std::string("context"));
    out["query"] = Value(text);
    out["memory_tape"] = Value(static_cast<long long>(memoryTape));
    out["tokens"] = Value(stringList(tokens));
    out["concepts"] = Value(stringList(concepts));
    out["matched"] = Value(std::move(matched));
    out["unresolved"] = Value(std::move(unresolved));
    out["associations"] = Value(std::move(associations));
    out["frames"] = Value(std::move(frames));
    return out;
}

Map routeCacheState(std::vector<Tape>& tapes,
                    int memoryTape,
                    double hotThreshold,
                    double coldThreshold,
                    const std::string& opcode) {
    if (memoryTape < 0 || memoryTape >= static_cast<int>(tapes.size()))
        throw std::runtime_error(opcode + ": memory tape out of bounds");

    Tape& tape = tapes[static_cast<size_t>(memoryTape)];
    List hot;
    List warm;
    List cold;

    for (auto& [cell, value] : tape.cells) {
        if (cell < 0 || value.kind() != ValueKind::Map)
            continue;
        Map& map = std::get<Map>(value.data);
        std::string kind = mapString(map, "kind", "");
        if (kind != "resource" && kind != "observation")
            continue;
        double heat = mapNumber(map, "heat", 0.0);
        std::string state = cacheStateForHeat(heat, hotThreshold, coldThreshold);
        map["cache_state"] = Value(state);

        Map item;
        item["address"] = Value(std::to_string(memoryTape) + "." + std::to_string(cell));
        item["key"] = Value(mapString(map, "key", ""));
        item["kind"] = Value(mapString(map, "resource_kind", kind));
        item["heat"] = Value(heat);
        item["access_count"] = Value(mapInteger(map, "access_count", 0));
        item["state"] = Value(state);
        if (state == "hot")
            hot.emplace_back(item);
        else if (state == "cold")
            cold.emplace_back(item);
        else
            warm.emplace_back(item);
    }

    Map out;
    out["memory_tape"] = Value(static_cast<long long>(memoryTape));
    out["hot_threshold"] = Value(hotThreshold);
    out["cold_threshold"] = Value(coldThreshold);
    out["hot"] = Value(std::move(hot));
    out["warm"] = Value(std::move(warm));
    out["cold"] = Value(std::move(cold));
    return out;
}

std::string routePackTape(const std::vector<Tape>& tapes,
                          int memoryTape,
                          const std::string& stateFilter,
                          const std::string& opcode) {
    if (memoryTape < 0 || memoryTape >= static_cast<int>(tapes.size()))
        throw std::runtime_error(opcode + ": memory tape out of bounds");

    const Tape& tape = tapes[static_cast<size_t>(memoryTape)];
    List cells;
    List reserved;
    bool includeReserved = stateFilter.empty() || stateFilter == "*";
    for (const auto& [cell, value] : tape.cells) {
        if (cell < 0) {
            if (includeReserved &&
                (cell == ROUTE_OUT_CELL || cell == ROUTE_IN_CELL ||
                cell == ROUTE_RESOURCE_INDEX_CELL || cell == ROUTE_NEXT_CELL ||
                cell == ROUTE_OBSERVATION_LOG_CELL || cell == ROUTE_FRAME_INDEX_CELL)) {
                Map item;
                item["cell"] = Value(cell);
                item["value"] = value;
                reserved.emplace_back(std::move(item));
            }
            continue;
        }
        if (!stateFilter.empty() && stateFilter != "*") {
            if (value.kind() != ValueKind::Map)
                continue;
            const Map& map = std::get<Map>(value.data);
            if (mapString(map, "cache_state", "") != stateFilter)
                continue;
        }
        Map item;
        item["cell"] = Value(cell);
        item["value"] = value;
        cells.emplace_back(std::move(item));
    }

    Map snapshot;
    snapshot["format"] = Value(std::string("intense_route_tape_snapshot_v1"));
    snapshot["tape"] = Value(static_cast<long long>(memoryTape));
    snapshot["tape_name"] = Value(tape.name);
    snapshot["cells"] = Value(std::move(cells));
    snapshot["reserved"] = Value(std::move(reserved));
    return stringifyJsonValue(Value(std::move(snapshot)));
}

Map routeUnpackTape(std::vector<Tape>& tapes,
                    int memoryTape,
                    const std::string& payload,
                    const std::string& opcode) {
    if (memoryTape < 0)
        throw std::runtime_error(opcode + ": memory tape must be non-negative");
    if (memoryTape >= static_cast<int>(tapes.size()))
        tapes.resize(static_cast<size_t>(memoryTape + 1));

    size_t pos = 0;
    Value parsed = parseJsonValue(payload, pos);
    if (parsed.kind() != ValueKind::Map)
        throw std::runtime_error(opcode + ": packed route payload must be a map");

    Map& snapshot = std::get<Map>(parsed.data);
    Tape& tape = tapes[static_cast<size_t>(memoryTape)];
    std::string name = mapString(snapshot, "tape_name", "");
    if (!name.empty()) {
        tape.name = name;
        tape.cells[TAPE_NAME_CELL] = Value(name);
    }

    auto loadItems = [&](const std::string& field) {
        auto it = snapshot.find(field);
        if (it == snapshot.end() || it->second.kind() != ValueKind::List)
            return static_cast<long long>(0);
        long long loaded = 0;
        for (const Value& itemValue : std::get<List>(it->second.data)) {
            if (itemValue.kind() != ValueKind::Map)
                continue;
            const Map& item = std::get<Map>(itemValue.data);
            auto cellIt = item.find("cell");
            auto valueIt = item.find("value");
            if (cellIt == item.end() || valueIt == item.end())
                continue;
            long long cell = integerValue(cellIt->second, opcode, "packed route cell");
            tape.cells[cell] = valueIt->second;
            ++loaded;
        }
        return loaded;
    };

    long long loaded = loadItems("reserved") + loadItems("cells");

    tape.cells[ROUTE_RESOURCE_INDEX_CELL] = Value(Map{});
    tape.cells[ROUTE_FRAME_INDEX_CELL] = Value(Map{});
    Map& index = std::get<Map>(tape.cells[ROUTE_RESOURCE_INDEX_CELL].data);
    Map& frameIndex = std::get<Map>(tape.cells[ROUTE_FRAME_INDEX_CELL].data);
    long long next = 0;
    for (const auto& [cell, value] : tape.cells) {
        if (cell >= 0)
            next = std::max(next, cell + 1);
        if (cell < 0 || value.kind() != ValueKind::Map)
            continue;
        const Map& map = std::get<Map>(value.data);
        if (mapString(map, "kind", "") != "resource")
            continue;
        std::string key = mapString(map, "key", "");
        if (key.empty())
            continue;
        std::string address = std::to_string(memoryTape) + "." + std::to_string(cell);
        index[key] = Value(address);
        auto aliasesIt = map.find("aliases");
        if (aliasesIt != map.end() && aliasesIt->second.kind() == ValueKind::List) {
            for (const Value& aliasValue : std::get<List>(aliasesIt->second.data)) {
                std::string alias = stringifyValue(aliasValue);
                if (!alias.empty())
                    index[alias] = Value(address);
            }
        }
        if (mapString(map, "resource_kind", "") == "frame")
            frameIndex[key] = Value(address);
    }
    tape.cells[ROUTE_NEXT_CELL] = Value(next);

    Map out;
    out["status"] = Value(std::string("unpacked"));
    out["memory_tape"] = Value(static_cast<long long>(memoryTape));
    out["loaded_cells"] = Value(loaded);
    out["next_cell"] = Value(next);
    return out;
}

Map makeRouteLearningEvidence(
    const std::string& rule,
    const RouteAddress& from,
    const RouteAddress& to,
    const std::string& relation,
    double strength,
    const std::string& note
) {
    Map evidence;
    evidence["rule"] = Value(rule);
    evidence["from"] = Value(canonicalRouteAddress(from));
    evidence["to"] = Value(canonicalRouteAddress(to));
    evidence["relation"] = Value(relation);
    evidence["strength"] = Value(strength);
    evidence["note"] = Value(note);
    return evidence;
}

Map routeLearnText(std::vector<Tape>& tapes, int memoryTape, const std::string& text, const std::string& opcode) {
    Tape& tape = tapes[static_cast<size_t>(memoryTape)];
    std::vector<std::string> tokens = textTokens(text);
    std::vector<std::string> content = routeContentTokens(text);
    std::vector<std::string> concepts = routeConceptKeys(text);

    long long observationCell = routeAllocateCell(tape);
    RouteAddress observation{memoryTape, observationCell};

    Map observationValue;
    observationValue["kind"] = Value(std::string("observation"));
    observationValue["text"] = Value(text);
    observationValue["tokens"] = Value(stringList(tokens));
    observationValue["content_tokens"] = Value(stringList(content));
    observationValue["concepts"] = Value(stringList(concepts));
    observationValue["source"] = Value(opcode);
    observationValue["address"] = Value(canonicalRouteAddress(observation));
    tapes[static_cast<size_t>(memoryTape)].cells[observationCell] = Value(observationValue);

    Map& log = mapCell(tape, ROUTE_OBSERVATION_LOG_CELL, opcode, "observation log");
    log[canonicalRouteAddress(observation)] = Value(text);

    Map learned;
    learned["status"] = Value(std::string("learned"));
    learned["memory_tape"] = Value(static_cast<long long>(memoryTape));
    learned["observation"] = Value(canonicalRouteAddress(observation));
    learned["text"] = Value(text);
    learned["tokens"] = Value(stringList(tokens));

    List resources;
    List links;
    std::unordered_map<std::string, RouteAddress> resourceAddresses;

    for (const auto& key : concepts) {
        bool phrase = key.find('_') != std::string::npos;
        bool created = false;
        RouteAddress resource = routeEnsureResource(
            tapes,
            memoryTape,
            key,
            routeLabelFromKey(key),
            phrase ? "phrase" : "concept",
            observation,
            opcode,
            created
        );
        resourceAddresses[key] = resource;

        Map item;
        item["key"] = Value(key);
        item["label"] = Value(routeLabelFromKey(key));
        item["address"] = Value(canonicalRouteAddress(resource));
        item["created"] = Value(created);
        resources.emplace_back(std::move(item));

        Map link = upsertRouteLink(tapes, observation, resource, "mentions", 1.0, true, opcode);
        links.emplace_back(makeRouteLearningEvidence(
            "observation_mentions_resource",
            observation,
            resource,
            "mentions",
            mapNumber(link, "strength", 1.0),
            "problem text contains resource"
        ));
    }

    for (size_t i = 0; i + 1 < content.size(); ++i) {
        auto left = resourceAddresses.find(content[i]);
        auto right = resourceAddresses.find(content[i + 1]);
        if (left == resourceAddresses.end() || right == resourceAddresses.end())
            continue;
        Map link = upsertRouteLink(tapes, left->second, right->second, "co_occurs", 1.0, true, opcode);
        links.emplace_back(makeRouteLearningEvidence(
            "adjacent_content_tokens",
            left->second,
            right->second,
            "co_occurs",
            mapNumber(link, "strength", 1.0),
            "adjacent content tokens in supplied problem"
        ));
    }

    for (const auto& key : concepts) {
        size_t underscore = key.find('_');
        if (underscore == std::string::npos)
            continue;
        auto phrase = resourceAddresses.find(key);
        if (phrase == resourceAddresses.end())
            continue;

        size_t start = 0;
        while (start < key.size()) {
            size_t next = key.find('_', start);
            std::string part = key.substr(start, next == std::string::npos ? std::string::npos : next - start);
            auto token = resourceAddresses.find(part);
            if (token != resourceAddresses.end()) {
                Map link = upsertRouteLink(tapes, phrase->second, token->second, "contains", 1.0, false, opcode);
                links.emplace_back(makeRouteLearningEvidence(
                    "phrase_contains_token",
                    phrase->second,
                    token->second,
                    "contains",
                    mapNumber(link, "strength", 1.0),
                    "multi-token resource contains component resource"
                ));
            }
            if (next == std::string::npos)
                break;
            start = next + 1;
        }
    }

    learned["resources"] = Value(std::move(resources));
    learned["links"] = Value(std::move(links));
    learned["resource_count"] = Value(static_cast<long long>(concepts.size()));
    return learned;
}

Value indexLookupAddress(const Tape& tape, const std::string& key) {
    auto indexIt = tape.cells.find(ROUTE_RESOURCE_INDEX_CELL);
    if (indexIt == tape.cells.end() || indexIt->second.kind() != ValueKind::Map)
        return Value();
    const Map& index = std::get<Map>(indexIt->second.data);
    auto it = index.find(key);
    if (it == index.end())
        return Value();
    return it->second;
}

Map routeReasonText(const std::vector<Tape>& tapes, int memoryTape, const std::string& text, const std::string& opcode) {
    const Tape& tape = tapes[static_cast<size_t>(memoryTape)];
    std::vector<std::string> concepts = routeConceptKeys(text);

    List matched;
    List evidence;
    List trace;
    std::unordered_set<std::string> inputConcepts(concepts.begin(), concepts.end());
    std::unordered_set<std::string> matchedAddresses;

    for (const auto& key : concepts) {
        Value addressValue = indexLookupAddress(tape, key);
        if (addressValue.kind() == ValueKind::Nil)
            continue;
        std::string addressText = stringifyValue(addressValue);
        matchedAddresses.insert(addressText);
        RouteAddress address = parseCanonicalRouteAddress(addressText, opcode);
        Map match;
        match["key"] = Value(key);
        match["address"] = Value(addressText);
        match["value"] = readTapeCellOrNil(tapes[static_cast<size_t>(address.tape)], address.cell);
        matched.emplace_back(match);

        List outgoing = routeLinksFor(tapes, address, "", 4);
        for (const auto& linkValue : outgoing) {
            if (linkValue.kind() != ValueKind::Map)
                continue;
            Map link = std::get<Map>(linkValue.data);
            RouteAddress target{
                static_cast<int>(mapInteger(link, "to_tape", 0)),
                mapInteger(link, "to_cell", 0)
            };
            link["value"] = target.tape >= 0 && target.tape < static_cast<int>(tapes.size())
                ? readTapeCellOrNil(tapes[static_cast<size_t>(target.tape)], target.cell)
                : Value();
            trace.emplace_back(std::move(link));
        }
    }

    struct FrameCandidate {
        std::string key;
        RouteAddress address;
        Map frame;
        List matchedTriggers;
        List missingTriggers;
        double score = 0.0;
        long long required = 0;
    };

    std::vector<FrameCandidate> candidates;
    auto frameIndexIt = tape.cells.find(ROUTE_FRAME_INDEX_CELL);
    if (frameIndexIt != tape.cells.end() && frameIndexIt->second.kind() == ValueKind::Map) {
        const Map& frameIndex = std::get<Map>(frameIndexIt->second.data);
        for (const auto& [frameKey, addressValue] : frameIndex) {
            RouteAddress frameAddress = parseCanonicalRouteAddress(stringifyValue(addressValue), opcode);
            Value frameValue = readTapeCellOrNil(tapes[static_cast<size_t>(frameAddress.tape)], frameAddress.cell);
            if (frameValue.kind() != ValueKind::Map)
                continue;

            const Map& frameMap = std::get<Map>(frameValue.data);
            auto triggerIt = frameMap.find("triggers");
            if (triggerIt == frameMap.end() || triggerIt->second.kind() != ValueKind::List)
                continue;

            FrameCandidate candidate;
            candidate.key = frameKey;
            candidate.address = frameAddress;
            candidate.frame = frameMap;

            for (const Value& triggerValue : std::get<List>(triggerIt->second.data)) {
                if (triggerValue.kind() != ValueKind::Map)
                    continue;
                const Map& trigger = std::get<Map>(triggerValue.data);
                std::string key = mapString(trigger, "key", "");
                std::string address = mapString(trigger, "address", "");
                double strength = mapNumber(trigger, "strength", 1.0);
                ++candidate.required;

                bool matchedTrigger = (!key.empty() && inputConcepts.count(key) > 0) ||
                                      (!address.empty() && matchedAddresses.count(address) > 0);
                Map item = trigger;
                item["matched"] = Value(matchedTrigger);
                if (matchedTrigger) {
                    candidate.score += strength;
                    candidate.matchedTriggers.emplace_back(std::move(item));
                } else {
                    candidate.missingTriggers.emplace_back(std::move(item));
                }
            }

            candidates.push_back(std::move(candidate));
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const FrameCandidate& a, const FrameCandidate& b) {
        bool aComplete = a.required > 0 && a.missingTriggers.empty();
        bool bComplete = b.required > 0 && b.missingTriggers.empty();
        if (aComplete != bComplete)
            return aComplete > bComplete;
        if (a.score != b.score)
            return a.score > b.score;
        return a.key < b.key;
    });

    Map result;
    result["query"] = Value(text);
    result["memory_tape"] = Value(static_cast<long long>(memoryTape));
    result["matched"] = Value(std::move(matched));
    result["trace"] = Value(std::move(trace));

    if (!candidates.empty() && candidates.front().required > 0 && candidates.front().missingTriggers.empty()) {
        const FrameCandidate& best = candidates.front();
        Map rule;
        rule["rule"] = Value(std::string("frame_triggers_satisfied"));
        rule["frame"] = Value(best.key);
        rule["frame_address"] = Value(canonicalRouteAddress(best.address));
        rule["matched_triggers"] = Value(best.matchedTriggers);
        rule["decision"] = Value(mapString(best.frame, "decision", "route_frame"));
        evidence.emplace_back(rule);

        List missingSlots;
        std::string question = mapString(best.frame, "question", "");
        auto slotIt = best.frame.find("missing_slots");
        if (slotIt != best.frame.end() && slotIt->second.kind() == ValueKind::List) {
            for (const Value& slotValue : std::get<List>(slotIt->second.data)) {
                if (slotValue.kind() != ValueKind::Map)
                    continue;
                const Map& slot = std::get<Map>(slotValue.data);
                std::string slotKey = mapString(slot, "key", "");
                if (!slotKey.empty())
                    missingSlots.emplace_back(slotKey);
                if (question.empty())
                    question = mapString(slot, "question", "");
            }
        }

        result["status"] = Value(missingSlots.empty() ? std::string("routed") : std::string("needs_clarification"));
        result["decision"] = Value(mapString(best.frame, "decision", missingSlots.empty() ? "route_frame" : "ask_clarifying_question"));
        if (!question.empty())
            result["question"] = Value(question);
        result["frame"] = Value(best.key);
        result["frame_address"] = Value(canonicalRouteAddress(best.address));
        result["missing"] = Value(std::move(missingSlots));
        result["support_score"] = Value(best.score);
    } else if (!candidates.empty()) {
        const FrameCandidate& best = candidates.front();
        Map partial;
        partial["rule"] = Value(std::string("best_frame_partial"));
        partial["frame"] = Value(best.key);
        partial["frame_address"] = Value(canonicalRouteAddress(best.address));
        partial["matched_triggers"] = Value(best.matchedTriggers);
        partial["missing_triggers"] = Value(best.missingTriggers);
        evidence.emplace_back(partial);

        result["status"] = Value(std::string("insufficient_evidence"));
        result["decision"] = Value(std::string("retrieve_more_context"));
        result["question"] = Value(std::string("What missing evidence should complete this route?"));
        result["missing"] = Value(best.missingTriggers);
        result["support_score"] = Value(best.score);
    } else if (!concepts.empty()) {
        result["status"] = Value(std::string("insufficient_evidence"));
        result["decision"] = Value(std::string("retrieve_more_context"));
        result["question"] = Value(std::string("Which model frame should handle this route?"));
        result["missing"] = Value(List{Value(std::string("route_frame"))});
        result["support_score"] = Value(static_cast<long long>(0));
    } else {
        result["status"] = Value(std::string("no_evidence"));
        result["decision"] = Value(std::string("ask_for_problem"));
        result["question"] = Value(std::string("What problem should I learn from?"));
        result["missing"] = Value(List{Value(std::string("problem_text"))});
        result["support_score"] = Value(static_cast<long long>(0));
    }

    result["evidence"] = Value(std::move(evidence));
    return result;
}

List csvNameList(const std::string& text) {
    List out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t comma = text.find(',', start);
        std::string item = trimStr(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!item.empty())
            out.emplace_back(item);
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return out;
}

Map makeHormoneCondition(const std::string& name, const std::string& op, const std::string& levelToken) {
    if (!isConditionOperator(op))
        throw std::runtime_error("EMO_ON: invalid condition operator: " + op);
    Map condition;
    condition["hormone"] = Value(name);
    condition["op"] = Value(op);
    condition["level"] = Value(percentValue(parseValue(levelToken), "EMO_ON", "condition level"));
    return condition;
}

Map parseHormoneCondition(const std::string& token) {
    static const std::vector<std::string> ops = {">=", "<=", "==", "!=", ">", "<"};
    for (const auto& op : ops) {
        size_t pos = token.find(op);
        if (pos == std::string::npos)
            continue;
        std::string name = trimStr(token.substr(0, pos));
        std::string level = trimStr(token.substr(pos + op.size()));
        if (name.empty() || level.empty())
            break;
        return makeHormoneCondition(name, op, level);
    }
    throw std::runtime_error("EMO_ON: condition must look like name>=level or name >= level, got '" + token + "'");
}

Map& ensureHormone(Tape& tape, const std::string& name, const std::string& opcode) {
    Map& hormones = mapCell(tape, EMO_HORMONES_CELL, opcode, "hormone registry");
    Value& existing = hormones[name];
    if (existing.kind() == ValueKind::Nil) {
        Map hormone;
        hormone["level"] = Value(0.0);
        hormone["behavior"] = Value(std::string("custom"));
        hormone["min"] = Value(0.0);
        hormone["max"] = Value(100.0);
        hormone["baseline"] = Value(0.0);
        hormone["decay"] = Value(0.0);
        hormone["sensitivity"] = Value(1.0);
        hormone["weight"] = Value(1.0);
        hormone["blocked_until"] = Value(static_cast<long long>(0));
        hormone["blocked_direction"] = Value(std::string("spike"));
        existing = Value(std::move(hormone));
    }
    if (existing.kind() != ValueKind::Map)
        throw std::runtime_error(opcode + ": hormone entry must be a map: " + name);
    return std::get<Map>(existing.data);
}

double hormoneLevel(const Map& hormones, const std::string& name, const std::string& opcode) {
    auto it = hormones.find(name);
    if (it == hormones.end() || it->second.kind() == ValueKind::Nil)
        throw std::runtime_error(opcode + ": unknown hormone: " + name);
    if (it->second.kind() != ValueKind::Map)
        throw std::runtime_error(opcode + ": hormone entry must be a map: " + name);
    return mapNumber(std::get<Map>(it->second.data), "level", 0.0);
}

bool evaluateEmotionCondition(const Map& condition, const Map& hormones, long long tick) {
    std::string hormone = mapString(condition, "hormone", "");
    std::string op = mapString(condition, "op", "");
    double threshold = mapNumber(condition, "level", 0.0);
    double level = 0.0;
    std::string key = toUpperStr(hormone);
    if (key == "TICK" || key == "TIME") {
        level = static_cast<double>(tick);
    } else {
        auto it = hormones.find(hormone);
        if (it == hormones.end() || it->second.kind() != ValueKind::Map)
            return false;
        level = mapNumber(std::get<Map>(it->second.data), "level", 0.0);
    }

    if (op == ">")  return level > threshold;
    if (op == ">=") return level >= threshold;
    if (op == "<")  return level < threshold;
    if (op == "<=") return level <= threshold;
    if (op == "==") return level == threshold;
    if (op == "!=") return level != threshold;
    return false;
}

std::vector<std::string> textTokens(const std::string& text) {
    std::vector<std::string> tokens;
    std::string current;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '_') {
            current.push_back(static_cast<char>(std::tolower(c)));
        } else if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    }
    if (!current.empty())
        tokens.push_back(current);
    return tokens;
}

List stringList(const std::vector<std::string>& items) {
    List out;
    for (const auto& item : items)
        out.emplace_back(item);
    return out;
}

double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

void applyHormoneDrift(Tape& tape, long long amount) {
    if (amount <= 0)
        return;
    Map& hormones = mapCell(tape, EMO_HORMONES_CELL, "EMO_TICK", "hormone registry");
    long long tick = emotionTick(tape);
    for (auto& [_, value] : hormones) {
        if (value.kind() != ValueKind::Map)
            continue;
        Map& hormone = std::get<Map>(value.data);
        double level = mapNumber(hormone, "level", 0.0);
        double baseline = mapNumber(hormone, "baseline", level);
        double decay = clamp01(mapNumber(hormone, "decay", 0.0));
        if (decay <= 0.0)
            continue;
        double minLevel = mapNumber(hormone, "min", 0.0);
        double maxLevel = mapNumber(hormone, "max", 100.0);
        double pull = 1.0 - std::pow(1.0 - decay, static_cast<double>(amount));
        double nextLevel = std::clamp(level + (baseline - level) * pull, minLevel, maxLevel);
        hormone["level"] = Value(nextLevel);
        hormone["last_delta"] = Value(nextLevel - level);
        hormone["updated_tick"] = Value(tick);
    }
}

Map emotionField(Tape& tape) {
    Map& hormones = mapCell(tape, EMO_HORMONES_CELL, "EMO_FIELD", "hormone registry");
    Map behaviorLevels;
    Map hormoneLevels;
    double weightedTotal = 0.0;
    double weightSum = 0.0;
    std::string dominant;
    double dominantScore = -1.0;

    for (const auto& [name, value] : hormones) {
        if (value.kind() != ValueKind::Map)
            continue;
        const Map& hormone = std::get<Map>(value.data);
        double level = mapNumber(hormone, "level", 0.0);
        double weight = mapNumber(hormone, "weight", 1.0);
        std::string behavior = mapString(hormone, "behavior", "custom");
        hormoneLevels[name] = Value(level);
        double weighted = level * weight;
        weightedTotal += weighted;
        weightSum += std::abs(weight);
        double existing = 0.0;
        auto it = behaviorLevels.find(behavior);
        if (it != behaviorLevels.end())
            existing = doubleValue(it->second, "EMO_FIELD", behavior);
        behaviorLevels[behavior] = Value(existing + weighted);
        if (std::abs(weighted) > dominantScore) {
            dominantScore = std::abs(weighted);
            dominant = name;
        }
    }

    Map field;
    field["tick"] = Value(emotionTick(tape));
    field["weighted_level"] = Value(weightSum == 0.0 ? 0.0 : weightedTotal / weightSum);
    field["dominant"] = Value(dominant);
    field["behaviors"] = Value(std::move(behaviorLevels));
    field["hormones"] = Value(std::move(hormoneLevels));
    return field;
}

Map rationalEvidence(const std::string& layer, const Value& value) {
    std::string mode = toUpperStr(layer.empty() ? std::string("BOOLEAN") : layer);
    Map evidence;
    evidence["layer"] = Value(toLowerStr(mode));
    evidence["input_type"] = Value(std::string(kindName(value.kind())));
    evidence["input"] = Value(compactDisplayValue(value, 120));

    bool pass = false;
    double confidence = 0.0;
    List notes;

    if (mode == "BOOLEAN" || mode == "BOOL") {
        pass = isTruthy(value);
        confidence = 1.0;
        notes.emplace_back(std::string(pass ? "truthy value passed boolean gate"
                                             : "falsy value failed boolean gate"));
    } else if (mode == "FUZZY") {
        double score = 0.0;
        if (value.kind() == ValueKind::Bool) score = std::get<bool>(value.data) ? 1.0 : 0.0;
        else if (value.kind() == ValueKind::Int) score = clamp01(static_cast<double>(std::get<long long>(value.data)) / 100.0);
        else if (value.kind() == ValueKind::Float) score = clamp01(std::get<double>(value.data));
        else if (value.kind() == ValueKind::Str) score = std::get<std::string>(value.data).empty() ? 0.0 : 0.5;
        else score = isTruthy(value) ? 0.6 : 0.0;
        pass = score >= 0.5;
        confidence = score;
        evidence["score"] = Value(score);
        notes.emplace_back(std::string("fuzzy layer interpreted current value as partial truth"));
    } else if (mode == "MATH" || mode == "MATHEMATICAL") {
        pass = value.kind() == ValueKind::Int || value.kind() == ValueKind::Float || value.kind() == ValueKind::List;
        confidence = pass ? 0.75 : 0.1;
        notes.emplace_back(std::string(pass ? "math layer found numeric/list-shaped input"
                                             : "math layer needs numeric or list-shaped input"));
    } else if (mode == "NLP") {
        std::string text = stringifyValue(value);
        auto tokens = textTokens(text);
        pass = !tokens.empty();
        confidence = clamp01(static_cast<double>(tokens.size()) / 12.0);
        evidence["tokens"] = Value(stringList(tokens));
        notes.emplace_back(std::string(pass ? "nlp layer extracted text tokens"
                                             : "nlp layer found no text tokens"));
    } else if (mode == "ML") {
        pass = value.kind() == ValueKind::List || value.kind() == ValueKind::Map;
        confidence = pass ? 0.55 : 0.15;
        notes.emplace_back(std::string(pass ? "ml layer found structured data to score"
                                             : "ml layer needs list/map feature data"));
    } else {
        pass = isTruthy(value);
        confidence = pass ? 0.4 : 0.0;
        notes.emplace_back(std::string("unknown rational layer fell back to truthiness"));
    }

    evidence["pass"] = Value(pass);
    evidence["confidence"] = Value(confidence);
    evidence["evidence"] = Value(std::move(notes));
    return evidence;
}

Map poetStrategy(const std::string& problemText) {
    auto tokens = textTokens(problemText);
    std::unordered_set<std::string> tokenSet(tokens.begin(), tokens.end());
    auto has = [&](const std::string& token) { return tokenSet.count(token) > 0; };
    auto containsAny = [&](std::initializer_list<std::string> words) {
        for (const auto& word : words)
            if (has(word)) return true;
        return false;
    };

    std::string type = "general_reasoning";
    std::vector<std::string> required = {"search", "boolean"};
    std::vector<std::string> optional = {"worksheet", "fuzzy"};
    std::vector<std::string> methods = {"retrieve_relevant_tapes", "form_candidates", "validate_evidence"};
    bool worksheet = problemText.size() > 80;

    bool hasDigit = std::any_of(problemText.begin(), problemText.end(),
                                [](unsigned char c) { return std::isdigit(c); });
    bool hasMathSymbol = problemText.find('+') != std::string::npos ||
                         problemText.find('-') != std::string::npos ||
                         problemText.find('*') != std::string::npos ||
                         problemText.find('/') != std::string::npos ||
                         problemText.find('=') != std::string::npos ||
                         problemText.find('^') != std::string::npos;

    if (hasDigit && hasMathSymbol) {
        type = "formula_math";
        required = {"search", "math", "boolean"};
        optional = {"worksheet", "fuzzy"};
        methods = {"retrieve_formula_tapes", "try_direct_formula", "verify_result"};
        worksheet = true;
    } else if (containsAny({"greater", "less", "equal", "true", "false", "is"})) {
        type = "boolean_check";
        required = {"boolean"};
        optional = {};
        methods = {"compare_conditions", "return_truth_value"};
        worksheet = false;
    } else if (containsAny({"story", "ending", "character", "write", "poem"})) {
        type = "creative_language";
        required = {"nlp", "fuzzy"};
        optional = {"search", "worksheet"};
        methods = {"retrieve_style_memory", "generate_alternatives", "rank_by_goal"};
        worksheet = true;
    } else if (containsAny({"why", "explain", "meaning", "difference", "compare"})) {
        type = "explanatory_nlp";
        required = {"search", "nlp", "fuzzy"};
        optional = {"worksheet", "boolean"};
        methods = {"retrieve_associations", "find_differences", "compose_explanation"};
        worksheet = true;
    }

    Map gate;
    gate["requires_explanation"] = Value(true);
    gate["requires_evidence"] = Value(true);
    gate["min_confidence"] = Value(type == "boolean_check" ? 1.0 : 0.65);

    Map strategy;
    strategy["problem_type"] = Value(type);
    strategy["required_layers"] = Value(stringList(required));
    strategy["optional_layers"] = Value(stringList(optional));
    strategy["candidate_methods"] = Value(stringList(methods));
    strategy["use_worksheet"] = Value(worksheet);
    strategy["evidence_gate"] = Value(std::move(gate));
    return strategy;
}

List poetSearch(const std::vector<Tape>& tapes, const std::string& query, size_t limit) {
    auto queryTokens = textTokens(query);
    std::unordered_set<std::string> querySet(queryTokens.begin(), queryTokens.end());
    struct Match {
        int tape = 0;
        long long cell = 0;
        double score = 0.0;
        std::string text;
        std::string tapeName;
    };
    std::vector<Match> matches;

    if (querySet.empty())
        return {};

    for (size_t tapeIndex = 0; tapeIndex < tapes.size(); ++tapeIndex) {
        const Tape& tape = tapes[tapeIndex];
        for (const auto& [cell, value] : tape.cells) {
            if (value.kind() == ValueKind::Nil)
                continue;
            std::string text = stringifyValue(value);
            auto cellTokens = textTokens(text);
            if (cellTokens.empty())
                continue;
            size_t hits = 0;
            std::unordered_set<std::string> seen;
            for (const auto& token : cellTokens) {
                if (querySet.count(token) && !seen.count(token)) {
                    ++hits;
                    seen.insert(token);
                }
            }
            if (hits == 0)
                continue;
            double score = static_cast<double>(hits) /
                static_cast<double>(std::max<size_t>(querySet.size(), 1));
            matches.push_back(Match{
                static_cast<int>(tapeIndex),
                cell,
                score,
                compactDisplayValue(value, 160),
                tape.name
            });
        }
    }

    std::sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.tape != b.tape) return a.tape < b.tape;
        return a.cell < b.cell;
    });

    List out;
    size_t count = std::min(limit, matches.size());
    for (size_t i = 0; i < count; ++i) {
        const auto& match = matches[i];
        Map item;
        item["tape"] = Value(static_cast<long long>(match.tape));
        item["tape_name"] = Value(match.tapeName);
        item["cell"] = Value(match.cell);
        item["address"] = Value(std::to_string(match.tape) + "." + std::to_string(match.cell));
        item["relevance"] = Value(match.score);
        item["text"] = Value(match.text);
        out.emplace_back(std::move(item));
    }
    return out;
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
        throw std::runtime_error("TAPE_NAME: tape name cell -2 must be a string");
    }

    auto cacheModuleName = [&](const std::string& moduleName) {
        if (!moduleName.empty())
            tapeNameCache[toUpperStr(moduleName)] = tapeIndex;
    };

    cacheModuleName("tape" + std::to_string(tapeIndex));
    cacheModuleName(tape.name);
}

void VM::syncAllTapeMetadata() {
    tapeNameCache.clear();
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

int VM::beginWorksheet(const std::string& name) {
    int previous = activeTape;
    int tapeIndex = static_cast<int>(tapes.size());
    tapes.emplace_back();
    Tape& worksheet = tapes.back();
    worksheet.name = name;
    worksheet.cells[TAPE_NAME_CELL] = Value(name);
    worksheet.ptr = 0;
    worksheetStack.push_back(WorksheetFrame{previous, tapeIndex, name, false});
    activeTape = tapeIndex;
    syncAllTapeMetadata();
    return tapeIndex;
}

void VM::endWorksheet(bool keep) {
    if (worksheetStack.empty())
        throw std::runtime_error("WORKSHEET_END: no active worksheet");

    WorksheetFrame frame = worksheetStack.back();
    worksheetStack.pop_back();
    if (frame.tapeIndex >= 0 && frame.tapeIndex < static_cast<int>(tapes.size())) {
        if (!keep && !frame.kept) {
            tapes[static_cast<size_t>(frame.tapeIndex)].cells.clear();
            tapes[static_cast<size_t>(frame.tapeIndex)].name.clear();
            tapes[static_cast<size_t>(frame.tapeIndex)].ptr = 0;
        }
    }
    activeTape = frame.previousTape >= 0 && frame.previousTape < static_cast<int>(tapes.size())
        ? frame.previousTape
        : 0;
    syncAllTapeMetadata();
}

void VM::cleanupWorksheets(size_t depth) {
    while (worksheetStack.size() > depth)
        endWorksheet(false);
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
    auto routeResolver = [this, &ins](const std::string& selector) {
        return resolveTapeSelector(selector, ins.opcode);
    };
    auto routeMemoryResolver = [this, &ins](const std::string& selector) {
        try {
            return resolveTapeSelector(selector, ins.opcode);
        } catch (const std::runtime_error&) {
            std::string value = trimStr(selector);
            if (value.empty() || isInteger(value))
                throw;
            int tapeIndex = static_cast<int>(tapes.size());
            tapes.emplace_back();
            tapes.back().name = value;
            tapes.back().cells[TAPE_NAME_CELL] = Value(value);
            syncAllTapeMetadata();
            return tapeIndex;
        }
    };

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
        Value value = resolveRoutedOperand(argOrThrow(ins, 0), tape, tapes, routeResolver, ins.opcode);
        tapes[activeTape].current() = value;
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
    else if (ins.opcode == "TAPE_NAME") {
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
    else if (ins.opcode == "WORKSHEET_BEGIN" || ins.opcode == "WORKSHEET_OPEN") {
        std::string name = ins.args.empty()
            ? std::string("worksheet")
            : stringOperand(argOrThrow(ins, 0), tape);
        int tapeIndex = beginWorksheet(name);
        Map info;
        info["tape"] = Value(static_cast<long long>(tapeIndex));
        info["name"] = Value(name);
        info["previous_tape"] = Value(static_cast<long long>(worksheetStack.back().previousTape));
        tapes[activeTape].current() = Value(std::move(info));
    }
    else if (ins.opcode == "WORKSHEET_END" || ins.opcode == "WORKSHEET_CLOSE") {
        bool keep = false;
        if (!ins.args.empty()) {
            std::string mode = toUpperStr(stringOperand(argOrThrow(ins, 0), tape));
            keep = mode == "KEEP" || mode == "SAVE" || mode == "PROMOTE";
        }
        endWorksheet(keep);
    }
    else if (ins.opcode == "WORKSHEET_KEEP" || ins.opcode == "WORKSHEET_PROMOTE") {
        if (worksheetStack.empty())
            throw std::runtime_error(ins.opcode + ": no active worksheet");
        WorksheetFrame& frame = worksheetStack.back();
        frame.kept = true;
        if (!ins.args.empty()) {
            std::string name = stringOperand(argOrThrow(ins, 0), tape);
            frame.name = name;
            tapes[static_cast<size_t>(frame.tapeIndex)].name = name;
            tapes[static_cast<size_t>(frame.tapeIndex)].cells[TAPE_NAME_CELL] = Value(name);
            syncAllTapeMetadata();
        }
        Map info;
        info["tape"] = Value(static_cast<long long>(frame.tapeIndex));
        info["name"] = Value(tapes[static_cast<size_t>(frame.tapeIndex)].name);
        info["kept"] = Value(true);
        tape.current() = Value(std::move(info));
    }
    else if (ins.opcode == "WORKSHEET_STATE") {
        List stack;
        for (const auto& frame : worksheetStack) {
            Map item;
            item["tape"] = Value(static_cast<long long>(frame.tapeIndex));
            item["previous_tape"] = Value(static_cast<long long>(frame.previousTape));
            item["name"] = Value(frame.name);
            item["kept"] = Value(frame.kept);
            stack.emplace_back(std::move(item));
        }
        Map state;
        state["depth"] = Value(static_cast<long long>(worksheetStack.size()));
        state["stack"] = Value(std::move(stack));
        tape.current() = Value(std::move(state));
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
        if (routeArgLooksAddress(ins, 0, tape)) {
            RouteAddress src = routeAddressArg(ins, 0, tape, routeResolver);
            tapes[activeTape].current() = readTapeCellOrNil(tapes[static_cast<size_t>(src.tape)], src.cell);
        } else {
            bool hasCell = ins.args.size() > 1;
            long long srcCell = hasCell ? integerOperand(argOrThrow(ins, 1), tape, ins.opcode, "cell index") : 0;
            int src = resolveTapeSelector(stringOperand(argOrThrow(ins, 0), tape), ins.opcode);
            if (!hasCell)
                srcCell = tapes[src].ptr;
            tapes[activeTape].current() = tapes[src].cells[srcCell];
        }
    }
    else if (ins.opcode == "TAPEWRITE" || ins.opcode == "TPUT" || ins.opcode == "POKE") {
        if (routeArgLooksAddress(ins, 0, tape)) {
            RouteAddress dest = routeAddressArg(ins, 0, tape, routeResolver);
            Value value = ins.args.size() > 1
                ? resolveRoutedOperand(argOrThrow(ins, 1), tapes[activeTape], tapes, routeResolver, ins.opcode)
                : tapes[activeTape].current();
            tapes[static_cast<size_t>(dest.tape)].cells[dest.cell] = value;
        } else {
            bool hasCell = ins.args.size() > 1;
            long long destCell = hasCell ? integerOperand(argOrThrow(ins, 1), tape, ins.opcode, "cell index") : 0;
            std::string destSelector = stringOperand(argOrThrow(ins, 0), tape);
            int dest = resolveTapeSelector(destSelector, ins.opcode);
            if (!hasCell)
                destCell = tapes[dest].ptr;
            Value value = ins.args.size() > 2
                ? resolveRoutedOperand(argOrThrow(ins, 2), tapes[activeTape], tapes, routeResolver, ins.opcode)
                : tapes[activeTape].current();
            tapes[dest].cells[destCell] = value;
        }
    }
    else if (ins.opcode == "TAPESWAP" || ins.opcode == "TSWAP") {
        if (routeArgLooksAddress(ins, 0, tape)) {
            RouteAddress other = routeAddressArg(ins, 0, tape, routeResolver);
            std::swap(tapes[activeTape].current(), tapes[static_cast<size_t>(other.tape)].cells[other.cell]);
        } else {
            bool hasCell = ins.args.size() > 1;
            long long otherCell = hasCell ? integerOperand(argOrThrow(ins, 1), tape, ins.opcode, "cell index") : 0;
            int other = resolveTapeSelector(stringOperand(argOrThrow(ins, 0), tape), ins.opcode);
            if (!hasCell)
                otherCell = tapes[other].ptr;
            std::swap(tapes[activeTape].current(), tapes[other].cells[otherCell]);
        }
    }
    else if (ins.opcode == "TAPESEND" || ins.opcode == "TSEND" || ins.opcode == "SEND") {
        Value value;
        int dest = 0;
        long long channel = 0;
        if (routeArgLooksAddress(ins, 0, tape)) {
            RouteAddress route = routeAddressArg(ins, 0, tape, routeResolver);
            dest = route.tape;
            channel = route.cell;
            value = ins.args.size() > 1
                ? resolveRoutedOperand(argOrThrow(ins, 1), tapes[activeTape], tapes, routeResolver, ins.opcode)
                : tapes[activeTape].current();
        } else {
            bool hasChannel = ins.args.size() > 1;
            channel = hasChannel ? integerOperand(argOrThrow(ins, 1), tape, ins.opcode, "cell index") : 0;
            std::string destSelector = stringOperand(argOrThrow(ins, 0), tape);
            dest = resolveTapeSelector(destSelector, ins.opcode);
            if (!hasChannel)
                channel = tapes[dest].ptr;
            value = ins.args.size() > 2
                ? resolveRoutedOperand(argOrThrow(ins, 2), tapes[activeTape], tapes, routeResolver, ins.opcode)
                : tapes[activeTape].current();
        }
        Value& inbox = tapes[static_cast<size_t>(dest)].cells[channel];
        if (inbox.kind() == ValueKind::Nil)
            inbox = Value(List{});
        if (inbox.kind() != ValueKind::List)
            throw std::runtime_error(ins.opcode + ": destination cell must be a list or nil");
        std::get<List>(inbox.data).push_back(value);
    }
    else if (ins.opcode == "TAPERECV" || ins.opcode == "TRECV" || ins.opcode == "RECV") {
        int src = 0;
        long long channel = 0;
        if (routeArgLooksAddress(ins, 0, tape)) {
            RouteAddress route = routeAddressArg(ins, 0, tape, routeResolver);
            src = route.tape;
            channel = route.cell;
        } else {
            bool hasChannel = ins.args.size() > 1;
            channel = hasChannel ? integerOperand(argOrThrow(ins, 1), tape, ins.opcode, "cell index") : 0;
            src = resolveTapeSelector(stringOperand(argOrThrow(ins, 0), tape), ins.opcode);
            if (!hasChannel)
                channel = tapes[src].ptr;
        }
        Value& inbox = tapes[static_cast<size_t>(src)].cells[channel];
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
    else if (ins.opcode == "ROUTE_ADDR" || ins.opcode == "RADDR") {
        RouteAddress address;
        if (ins.args.empty()) {
            address = RouteAddress{activeTape, tapes[activeTape].ptr};
        } else if (ins.args.size() == 1 && routeArgLooksAddress(ins, 0, tape)) {
            address = routeAddressArg(ins, 0, tape, routeResolver);
        } else if (ins.args.size() == 1) {
            address = RouteAddress{
                activeTape,
                integerOperand(argOrThrow(ins, 0), tape, ins.opcode, "cell index")
            };
        } else {
            std::string routeTapeSelector = stringOperand(argOrThrow(ins, 0), tape);
            long long routeCell = integerOperand(argOrThrow(ins, 1), tape, ins.opcode, "cell index");
            int routeTape = resolveTapeSelector(routeTapeSelector, ins.opcode);
            address = RouteAddress{routeTape, routeCell};
        }
        tapes[activeTape].current() = Value(canonicalRouteAddress(address));
    }
    else if (ins.opcode == "ROUTE_GET" || ins.opcode == "RGET") {
        RouteAddress src = routeAddressArg(ins, 0, tape, routeResolver);
        tapes[activeTape].current() = readTapeCellOrNil(tapes[static_cast<size_t>(src.tape)], src.cell);
    }
    else if (ins.opcode == "ROUTE_SET" || ins.opcode == "RSET") {
        RouteAddress dest = routeAddressArg(ins, 0, tape, routeResolver);
        Value value = ins.args.size() > 1
            ? resolveRoutedOperand(argOrThrow(ins, 1), tapes[activeTape], tapes, routeResolver, ins.opcode)
            : tapes[activeTape].current();
        tapes[static_cast<size_t>(dest.tape)].cells[dest.cell] = value;
    }
    else if (ins.opcode == "ROUTE_LINK" || ins.opcode == "RLINK" || ins.opcode == "CONNECT") {
        RouteAddress from = routeAddressArg(ins, 0, tape, routeResolver);
        RouteAddress to = routeAddressArg(ins, 1, tapes[activeTape], routeResolver);
        RouteRelationStrength args = routeRelationStrengthArgs(ins, 2, tapes[activeTape], 1.0);
        tapes[activeTape].current() = Value(upsertRouteLink(tapes, from, to, args.relation, args.strength, false, ins.opcode));
    }
    else if (ins.opcode == "ROUTE_TOUCH" || ins.opcode == "RTOUCH") {
        RouteAddress from = routeAddressArg(ins, 0, tape, routeResolver);
        RouteAddress to = routeAddressArg(ins, 1, tapes[activeTape], routeResolver);
        RouteRelationStrength args = routeRelationStrengthArgs(ins, 2, tapes[activeTape], 0.1);
        tapes[activeTape].current() = Value(upsertRouteLink(tapes, from, to, args.relation, args.strength, true, ins.opcode));
    }
    else if (ins.opcode == "ROUTE_FOLLOW" || ins.opcode == "RFOLLOW") {
        RouteAddress from = routeAddressArg(ins, 0, tape, routeResolver);
        std::string relation;
        size_t limit = 0;
        if (ins.args.size() > 1)
            relation = stringOperand(argOrThrow(ins, 1), tapes[activeTape]);
        if (ins.args.size() > 2)
            limit = sizeOperand(argOrThrow(ins, 2), tapes[activeTape], ins.opcode, "route limit");

        List links = routeLinksFor(tapes, from, relation, limit);
        List out;
        for (const Value& value : links) {
            Map item = std::get<Map>(value.data);
            RouteAddress target{
                static_cast<int>(mapInteger(item, "to_tape", 0)),
                mapInteger(item, "to_cell", 0)
            };
            if (target.tape >= 0 && target.tape < static_cast<int>(tapes.size()))
                item["value"] = readTapeCellOrNil(tapes[static_cast<size_t>(target.tape)], target.cell);
            else
                item["value"] = Value();
            out.emplace_back(std::move(item));
        }
        tapes[activeTape].current() = Value(std::move(out));
    }
    else if (ins.opcode == "ROUTE_INFO" || ins.opcode == "RINFO") {
        RouteAddress address = routeAddressArg(ins, 0, tape, routeResolver);
        Map info;
        info["address"] = Value(canonicalRouteAddress(address));
        info["tape"] = Value(static_cast<long long>(address.tape));
        info["tape_name"] = Value(tapes[static_cast<size_t>(address.tape)].name);
        info["cell"] = Value(address.cell);
        info["value"] = readTapeCellOrNil(tapes[static_cast<size_t>(address.tape)], address.cell);
        info["outgoing"] = Value(routeLinksFor(tapes, address, "", 0));
        info["incoming"] = Value(inboundRouteLinksFor(tapes, address));
        tapes[activeTape].current() = Value(std::move(info));
    }
    else if (ins.opcode == "ROUTE_RESOURCE" || ins.opcode == "RRESOURCE") {
        std::string key = stringOperand(argOrThrow(ins, 0), tapes[activeTape]);
        std::string kind = ins.args.size() > 1 ? stringOperand(argOrThrow(ins, 1), tapes[activeTape]) : "concept";
        std::string label = ins.args.size() > 2 ? stringOperand(argOrThrow(ins, 2), tapes[activeTape]) : routeLabelFromKey(key);
        bool created = false;
        RouteAddress address = routeEnsureModelResource(tapes, activeTape, key, kind, label, ins.opcode, &created);
        Map out;
        out["key"] = Value(key);
        out["kind"] = Value(kind);
        out["label"] = Value(label);
        out["address"] = Value(canonicalRouteAddress(address));
        out["created"] = Value(created);
        tapes[activeTape].current() = Value(std::move(out));
    }
    else if (ins.opcode == "ROUTE_ALIAS" || ins.opcode == "RALIAS") {
        std::string canonical = stringOperand(argOrThrow(ins, 0), tapes[activeTape]);
        std::string alias = stringOperand(argOrThrow(ins, 1), tapes[activeTape]);
        RouteAddress canonicalAddress = routeEnsureModelResource(
            tapes, activeTape, canonical, "concept", routeLabelFromKey(canonical), ins.opcode);

        Map& index = mapCell(tapes[activeTape], ROUTE_RESOURCE_INDEX_CELL, ins.opcode, "resource index");
        index[alias] = Value(canonicalRouteAddress(canonicalAddress));

        Map& resource = std::get<Map>(tapes[static_cast<size_t>(canonicalAddress.tape)].cells[canonicalAddress.cell].data);
        List& aliases = listField(resource, "aliases");
        bool foundAlias = false;
        for (const Value& value : aliases) {
            if (value.kind() == ValueKind::Str && std::get<std::string>(value.data) == alias) {
                foundAlias = true;
                break;
            }
        }
        if (!foundAlias)
            aliases.emplace_back(alias);

        Map out;
        out["canonical"] = Value(canonical);
        out["alias"] = Value(alias);
        out["address"] = Value(canonicalRouteAddress(canonicalAddress));
        tapes[activeTape].current() = Value(std::move(out));
    }
    else if (ins.opcode == "ROUTE_ASSOC" || ins.opcode == "RASSOC") {
        std::string fromKey = stringOperand(argOrThrow(ins, 0), tapes[activeTape]);
        std::string toKey = stringOperand(argOrThrow(ins, 1), tapes[activeTape]);
        std::string relation = ins.args.size() > 2 ? stringOperand(argOrThrow(ins, 2), tapes[activeTape]) : "related";
        double strength = ins.args.size() > 3 ? doubleOperand(argOrThrow(ins, 3), tapes[activeTape], ins.opcode, "route strength") : 1.0;
        RouteAddress from = routeEnsureModelResource(tapes, activeTape, fromKey, "concept", routeLabelFromKey(fromKey), ins.opcode);
        RouteAddress to = routeEnsureModelResource(tapes, activeTape, toKey, "concept", routeLabelFromKey(toKey), ins.opcode);
        tapes[activeTape].current() = Value(upsertRouteLink(tapes, from, to, relation, strength, true, ins.opcode));
    }
    else if (ins.opcode == "ROUTE_FRAME" || ins.opcode == "RFRAME") {
        std::string key = stringOperand(argOrThrow(ins, 0), tapes[activeTape]);
        std::string decision = ins.args.size() > 1 ? stringOperand(argOrThrow(ins, 1), tapes[activeTape]) : "route_frame";
        std::string question = ins.args.size() > 2 ? stringOperand(argOrThrow(ins, 2), tapes[activeTape]) : "";
        RouteAddress frame = routeEnsureFrame(tapes, activeTape, key, decision, question, ins.opcode);
        Map out;
        out["key"] = Value(key);
        out["address"] = Value(canonicalRouteAddress(frame));
        out["decision"] = Value(decision);
        if (!question.empty())
            out["question"] = Value(question);
        tapes[activeTape].current() = Value(std::move(out));
    }
    else if (ins.opcode == "ROUTE_TRIGGER" || ins.opcode == "RTRIGGER") {
        std::string frameKey = stringOperand(argOrThrow(ins, 0), tapes[activeTape]);
        std::string conceptKey = stringOperand(argOrThrow(ins, 1), tapes[activeTape]);
        double strength = ins.args.size() > 2 ? doubleOperand(argOrThrow(ins, 2), tapes[activeTape], ins.opcode, "trigger strength") : 1.0;
        RouteAddress frame = routeEnsureFrame(tapes, activeTape, frameKey, "", "", ins.opcode);
        RouteAddress concept = routeEnsureModelResource(tapes, activeTape, conceptKey, "concept", routeLabelFromKey(conceptKey), ins.opcode);

        Map& frameMap = std::get<Map>(tapes[static_cast<size_t>(frame.tape)].cells[frame.cell].data);
        List& triggers = listField(frameMap, "triggers");
        Map trigger;
        trigger["key"] = Value(conceptKey);
        trigger["address"] = Value(canonicalRouteAddress(concept));
        trigger["strength"] = Value(strength);
        upsertMapListItem(triggers, "key", conceptKey, trigger);

        upsertRouteLink(tapes, concept, frame, "activates_frame", strength, true, ins.opcode);
        upsertRouteLink(tapes, frame, concept, "requires_evidence", strength, false, ins.opcode);

        Map out;
        out["frame"] = Value(frameKey);
        out["trigger"] = Value(conceptKey);
        out["frame_address"] = Value(canonicalRouteAddress(frame));
        out["trigger_address"] = Value(canonicalRouteAddress(concept));
        out["strength"] = Value(strength);
        tapes[activeTape].current() = Value(std::move(out));
    }
    else if (ins.opcode == "ROUTE_SLOT" || ins.opcode == "RSLOT") {
        std::string frameKey = stringOperand(argOrThrow(ins, 0), tapes[activeTape]);
        std::string slotKey = stringOperand(argOrThrow(ins, 1), tapes[activeTape]);
        std::string question = ins.args.size() > 2 ? stringOperand(argOrThrow(ins, 2), tapes[activeTape]) : "";
        RouteAddress frame = routeEnsureFrame(tapes, activeTape, frameKey, "", question, ins.opcode);
        RouteAddress slot = routeEnsureModelResource(tapes, activeTape, slotKey, "slot", routeLabelFromKey(slotKey), ins.opcode);

        Map& frameMap = std::get<Map>(tapes[static_cast<size_t>(frame.tape)].cells[frame.cell].data);
        List& slots = listField(frameMap, "missing_slots");
        Map item;
        item["key"] = Value(slotKey);
        item["address"] = Value(canonicalRouteAddress(slot));
        if (!question.empty())
            item["question"] = Value(question);
        upsertMapListItem(slots, "key", slotKey, item);
        if (!question.empty() && mapString(frameMap, "question", "").empty())
            frameMap["question"] = Value(question);

        upsertRouteLink(tapes, frame, slot, "requires_missing_slot", 1.0, false, ins.opcode);

        Map out;
        out["frame"] = Value(frameKey);
        out["slot"] = Value(slotKey);
        out["frame_address"] = Value(canonicalRouteAddress(frame));
        out["slot_address"] = Value(canonicalRouteAddress(slot));
        if (!question.empty())
            out["question"] = Value(question);
        tapes[activeTape].current() = Value(std::move(out));
    }
    else if (ins.opcode == "ROUTE_USE" || ins.opcode == "RUSE") {
        std::string target = stringOperand(argOrThrow(ins, 0), tapes[activeTape]);
        int memoryTape = activeTape;
        if (ins.args.size() > 1)
            memoryTape = resolveTapeSelector(stringOperand(argOrThrow(ins, 1), tapes[activeTape]), ins.opcode);
        double amount = ins.args.size() > 2
            ? doubleOperand(argOrThrow(ins, 2), tapes[activeTape], ins.opcode, "route use amount")
            : 1.0;
        RouteAddress address = resolveRouteKeyOrAddress(tapes, memoryTape, target, ins.opcode);
        tapes[activeTape].current() = Value(touchRouteCell(tapes, address, amount, ins.opcode));
    }
    else if (ins.opcode == "ROUTE_CONTEXT" || ins.opcode == "RCONTEXT") {
        std::string text = ins.args.empty()
            ? stringifyValue(tapes[activeTape].current())
            : stringOperand(argOrThrow(ins, 0), tapes[activeTape]);
        int memoryTape = activeTape;
        if (ins.args.size() > 1)
            memoryTape = resolveTapeSelector(stringOperand(argOrThrow(ins, 1), tapes[activeTape]), ins.opcode);
        size_t limit = ins.args.size() > 2
            ? sizeOperand(argOrThrow(ins, 2), tapes[activeTape], ins.opcode, "route context link limit")
            : 4;
        tapes[activeTape].current() = Value(routeContextText(tapes, memoryTape, text, limit, ins.opcode));
    }
    else if (ins.opcode == "ROUTE_CACHE" || ins.opcode == "RCACHE") {
        int memoryTape = activeTape;
        if (!ins.args.empty())
            memoryTape = resolveTapeSelector(stringOperand(argOrThrow(ins, 0), tapes[activeTape]), ins.opcode);
        double hotThreshold = ins.args.size() > 1
            ? doubleOperand(argOrThrow(ins, 1), tapes[activeTape], ins.opcode, "hot threshold")
            : 3.0;
        double coldThreshold = ins.args.size() > 2
            ? doubleOperand(argOrThrow(ins, 2), tapes[activeTape], ins.opcode, "cold threshold")
            : 0.25;
        tapes[activeTape].current() = Value(routeCacheState(tapes, memoryTape, hotThreshold, coldThreshold, ins.opcode));
    }
    else if (ins.opcode == "ROUTE_PACK" || ins.opcode == "RPACK") {
        int memoryTape = activeTape;
        if (!ins.args.empty())
            memoryTape = resolveTapeSelector(stringOperand(argOrThrow(ins, 0), tapes[activeTape]), ins.opcode);
        std::string stateFilter = ins.args.size() > 1
            ? stringOperand(argOrThrow(ins, 1), tapes[activeTape])
            : std::string("*");
        tapes[activeTape].current() = Value(routePackTape(tapes, memoryTape, stateFilter, ins.opcode));
    }
    else if (ins.opcode == "ROUTE_UNPACK" || ins.opcode == "RUNPACK") {
        std::string payload = ins.args.empty()
            ? stringifyValue(tapes[activeTape].current())
            : stringOperand(argOrThrow(ins, 0), tapes[activeTape]);
        int memoryTape = activeTape;
        if (ins.args.size() > 1)
            memoryTape = routeMemoryResolver(stringOperand(argOrThrow(ins, 1), tapes[activeTape]));
        tapes[activeTape].current() = Value(routeUnpackTape(tapes, memoryTape, payload, ins.opcode));
        syncAllTapeMetadata();
    }
    else if (ins.opcode == "ROUTE_LEARN" || ins.opcode == "RLEARN" ||
             ins.opcode == "ROUTE_OBSERVE" || ins.opcode == "ROBSERVE" ||
             ins.opcode == "LEARN_ROUTE") {
        std::string text = ins.args.empty()
            ? stringifyValue(tapes[activeTape].current())
            : stringOperand(argOrThrow(ins, 0), tapes[activeTape]);
        int memoryTape = activeTape;
        if (ins.args.size() > 1)
            memoryTape = routeMemoryResolver(stringOperand(argOrThrow(ins, 1), tapes[activeTape]));
        tapes[activeTape].current() = Value(routeLearnText(tapes, memoryTape, text, ins.opcode));
    }
    else if (ins.opcode == "ROUTE_REASON" || ins.opcode == "RREASON" ||
             ins.opcode == "ROUTE_QUERY" || ins.opcode == "RQUERY") {
        std::string text = ins.args.empty()
            ? stringifyValue(tapes[activeTape].current())
            : stringOperand(argOrThrow(ins, 0), tapes[activeTape]);
        int memoryTape = activeTape;
        if (ins.args.size() > 1)
            memoryTape = resolveTapeSelector(stringOperand(argOrThrow(ins, 1), tapes[activeTape]), ins.opcode);
        tapes[activeTape].current() = Value(routeReasonText(tapes, memoryTape, text, ins.opcode));
    }
    else if (ins.opcode == "CLEAR_TAPE" || ins.opcode == "CLEARTAPE") {
        tape.cells.clear();
        tape.name.clear();
        tape.ptr = 0;
        syncAllTapeMetadata();
    }
    else if (ins.opcode == "DELETE") {
        tape.cells.erase(tape.ptr);
        if (tape.ptr == TAPE_NAME_CELL)
            syncAllTapeMetadata();
    }

    // ---- Emotional / hormone tape ------------------------------------------

    else if (ins.opcode == "EMO_INIT" || ins.opcode == "EMOTIONAL_TAPE") {
        std::string name = ins.args.empty()
            ? std::string("emotional_tape")
            : stringOperand(argOrThrow(ins, 0), tape);
        tape.name = name;
        tape.cells[TAPE_NAME_CELL] = Value(name);
        mapCell(tape, EMO_HORMONES_CELL, ins.opcode, "hormone registry");
        mapCell(tape, EMO_EVENTS_CELL, ins.opcode, "event registry");
        if (tape.cells[EMO_CLOCK_CELL].kind() == ValueKind::Nil)
            tape.cells[EMO_CLOCK_CELL] = Value(static_cast<long long>(0));
        tape.ptr = 0;
        syncAllTapeMetadata();
    }
    else if (ins.opcode == "HORMONE" || ins.opcode == "HORMONE_DEF" ||
             ins.opcode == "HORMONE_CREATE") {
        std::string name = stringOperand(argOrThrow(ins, 0), tape);
        double level = percentOperand(argOrThrow(ins, 1), tape, ins.opcode, "hormone level");
        std::string behavior = ins.args.size() > 2
            ? stringOperand(argOrThrow(ins, 2), tape)
            : std::string("custom");
        double minLevel = ins.args.size() > 3
            ? percentOperand(argOrThrow(ins, 3), tape, ins.opcode, "minimum level")
            : 0.0;
        double maxLevel = ins.args.size() > 4
            ? percentOperand(argOrThrow(ins, 4), tape, ins.opcode, "maximum level")
            : 100.0;
        double baseline = ins.args.size() > 5
            ? percentOperand(argOrThrow(ins, 5), tape, ins.opcode, "baseline level")
            : level;
        double decay = ins.args.size() > 6
            ? percentOperand(argOrThrow(ins, 6), tape, ins.opcode, "decay rate")
            : 0.0;
        double sensitivity = ins.args.size() > 7
            ? percentOperand(argOrThrow(ins, 7), tape, ins.opcode, "sensitivity")
            : 1.0;
        double weight = ins.args.size() > 8
            ? percentOperand(argOrThrow(ins, 8), tape, ins.opcode, "weight")
            : 1.0;
        if (maxLevel < minLevel)
            throw std::runtime_error(ins.opcode + ": maximum level must be >= minimum level");

        Map hormone;
        hormone["level"] = Value(std::clamp(level, minLevel, maxLevel));
        hormone["behavior"] = Value(behavior);
        hormone["min"] = Value(minLevel);
        hormone["max"] = Value(maxLevel);
        hormone["baseline"] = Value(std::clamp(baseline, minLevel, maxLevel));
        hormone["decay"] = Value(clamp01(decay));
        hormone["sensitivity"] = Value(sensitivity);
        hormone["weight"] = Value(weight);
        hormone["blocked_until"] = Value(static_cast<long long>(0));
        hormone["blocked_direction"] = Value(std::string("spike"));
        hormone["updated_tick"] = Value(emotionTick(tape));
        mapCell(tape, EMO_HORMONES_CELL, ins.opcode, "hormone registry")[name] = Value(std::move(hormone));
        tape.current() = mapCell(tape, EMO_HORMONES_CELL, ins.opcode, "hormone registry")[name];
    }
    else if (ins.opcode == "HORMONE_SET" || ins.opcode == "HORMONE_LEVEL_SET") {
        std::string name = stringOperand(argOrThrow(ins, 0), tape);
        double level = percentOperand(argOrThrow(ins, 1), tape, ins.opcode, "hormone level");
        Map& hormone = ensureHormone(tape, name, ins.opcode);
        double minLevel = mapNumber(hormone, "min", 0.0);
        double maxLevel = mapNumber(hormone, "max", 100.0);
        double oldLevel = mapNumber(hormone, "level", 0.0);
        double nextLevel = std::clamp(level, minLevel, maxLevel);
        hormone["level"] = Value(nextLevel);
        hormone["last_delta"] = Value(nextLevel - oldLevel);
        hormone["updated_tick"] = Value(emotionTick(tape));
        tape.current() = Value(nextLevel);
    }
    else if (ins.opcode == "HORMONE_ADD" || ins.opcode == "HORMONE_SPIKE" ||
             ins.opcode == "HORMONE_DROP") {
        std::string name = stringOperand(argOrThrow(ins, 0), tape);
        double delta = percentOperand(argOrThrow(ins, 1), tape, ins.opcode, "hormone delta");
        if (ins.opcode == "HORMONE_DROP" && delta > 0)
            delta = -delta;
        Map& hormone = ensureHormone(tape, name, ins.opcode);
        delta *= mapNumber(hormone, "sensitivity", 1.0);
        long long tick = emotionTick(tape);
        long long blockedUntil = mapInteger(hormone, "blocked_until", 0);
        std::string blockedDirection = toUpperStr(mapString(hormone, "blocked_direction", "spike"));
        std::string direction = delta >= 0.0 ? "SPIKE" : "DROP";
        bool blocked = blockedUntil > tick &&
            (blockedDirection == "BOTH" || blockedDirection == direction);
        double oldLevel = mapNumber(hormone, "level", 0.0);
        if (!blocked) {
            double minLevel = mapNumber(hormone, "min", 0.0);
            double maxLevel = mapNumber(hormone, "max", 100.0);
            double nextLevel = std::clamp(oldLevel + delta, minLevel, maxLevel);
            hormone["level"] = Value(nextLevel);
            hormone["last_delta"] = Value(nextLevel - oldLevel);
            hormone["updated_tick"] = Value(tick);
            tape.current() = Value(nextLevel);
        } else {
            hormone["blocked_delta"] = Value(delta);
            hormone["blocked_tick"] = Value(tick);
            tape.current() = Value(oldLevel);
        }
    }
    else if (ins.opcode == "HORMONE_LEVEL" || ins.opcode == "HORMONE_GET") {
        std::string name = stringOperand(argOrThrow(ins, 0), tape);
        Map& hormones = mapCell(tape, EMO_HORMONES_CELL, ins.opcode, "hormone registry");
        tape.current() = Value(hormoneLevel(hormones, name, ins.opcode));
    }
    else if (ins.opcode == "HORMONE_INFO") {
        std::string name = stringOperand(argOrThrow(ins, 0), tape);
        Map& hormone = ensureHormone(tape, name, ins.opcode);
        tape.current() = Value(hormone);
    }
    else if (ins.opcode == "HORMONE_BLOCK") {
        std::string name = stringOperand(argOrThrow(ins, 0), tape);
        long long duration = integerOperand(argOrThrow(ins, 1), tape, ins.opcode, "block duration");
        if (duration < 0)
            throw std::runtime_error(ins.opcode + ": block duration must be non-negative");
        std::string direction = ins.args.size() > 2
            ? toUpperStr(stringOperand(argOrThrow(ins, 2), tape))
            : std::string("SPIKE");
        if (direction != "SPIKE" && direction != "DROP" && direction != "BOTH")
            throw std::runtime_error(ins.opcode + ": block direction must be spike, drop, or both");
        Map& hormone = ensureHormone(tape, name, ins.opcode);
        hormone["blocked_until"] = Value(emotionTick(tape) + duration);
        hormone["blocked_direction"] = Value(toLowerStr(direction));
        tape.current() = Value(hormone);
    }
    else if (ins.opcode == "HORMONE_UNBLOCK") {
        std::string name = stringOperand(argOrThrow(ins, 0), tape);
        Map& hormone = ensureHormone(tape, name, ins.opcode);
        hormone["blocked_until"] = Value(static_cast<long long>(0));
        tape.current() = Value(hormone);
    }
    else if (ins.opcode == "EMO_ON" || ins.opcode == "EMO_EVENT") {
        std::string name = stringOperand(argOrThrow(ins, 0), tape);
        std::string action = stringOperand(argOrThrow(ins, 1), tape);
        Map event;
        event["action"] = Value(action);
        event["conditions"] = Value(List{});
        event["cooldown"] = Value(static_cast<long long>(0));
        event["block_for"] = Value(static_cast<long long>(0));
        event["block"] = Value(List{});
        event["last_tick"] = Value(static_cast<long long>(-1));
        event["fired_count"] = Value(static_cast<long long>(0));

        List conditions;
        for (size_t i = 2; i < ins.args.size(); ++i) {
            std::string token = stringOperand(ins.args[i], tape);
            size_t eq = token.find('=');
            if (eq != std::string::npos) {
                std::string key = toUpperStr(trimStr(token.substr(0, eq)));
                std::string value = trimStr(token.substr(eq + 1));
                if (key == "COOLDOWN" || key == "REFRACTORY") {
                    event["cooldown"] = Value(parseLongLongStrict(value, ins.opcode, "cooldown"));
                    continue;
                }
                if (key == "BLOCK_FOR") {
                    event["block_for"] = Value(parseLongLongStrict(value, ins.opcode, "block duration"));
                    continue;
                }
                if (key == "BLOCK") {
                    event["block"] = Value(csvNameList(value));
                    continue;
                }
            }

            if (i + 2 < ins.args.size() && isConditionOperator(stringOperand(ins.args[i + 1], tape))) {
                conditions.emplace_back(makeHormoneCondition(
                    token,
                    stringOperand(ins.args[i + 1], tape),
                    stringOperand(ins.args[i + 2], tape)
                ));
                i += 2;
            } else {
                conditions.emplace_back(parseHormoneCondition(token));
            }
        }
        event["conditions"] = Value(std::move(conditions));
        mapCell(tape, EMO_EVENTS_CELL, ins.opcode, "event registry")[name] = Value(std::move(event));
    }
    else if (ins.opcode == "EMO_CHECK" || ins.opcode == "HORMONE_CHECK") {
        int emotionTape = activeTape;
        Map& hormones = mapCell(tape, EMO_HORMONES_CELL, ins.opcode, "hormone registry");
        Map& events = mapCell(tape, EMO_EVENTS_CELL, ins.opcode, "event registry");
        long long tick = emotionTick(tape);

        std::vector<std::string> names;
        for (const auto& [name, _] : events)
            names.push_back(name);
        std::sort(names.begin(), names.end());

        std::vector<std::string> actions;
        for (const auto& name : names) {
            Value& eventValue = events[name];
            if (eventValue.kind() != ValueKind::Map)
                throw std::runtime_error(ins.opcode + ": event entry must be a map: " + name);
            Map& event = std::get<Map>(eventValue.data);
            long long lastTick = mapInteger(event, "last_tick", -1);
            long long cooldown = mapInteger(event, "cooldown", 0);
            if (lastTick >= 0 && tick < lastTick + cooldown)
                continue;

            auto condIt = event.find("conditions");
            if (condIt == event.end() || condIt->second.kind() != ValueKind::List)
                throw std::runtime_error(ins.opcode + ": event conditions must be a list: " + name);
            bool matched = true;
            for (const auto& conditionValue : std::get<List>(condIt->second.data)) {
                if (conditionValue.kind() != ValueKind::Map ||
                    !evaluateEmotionCondition(std::get<Map>(conditionValue.data), hormones, tick)) {
                    matched = false;
                    break;
                }
            }
            if (!matched)
                continue;

            event["last_tick"] = Value(tick);
            event["fired_count"] = Value(mapInteger(event, "fired_count", 0) + 1);
            long long blockFor = mapInteger(event, "block_for", 0);
            if (blockFor > 0) {
                auto blockIt = event.find("block");
                if (blockIt != event.end() && blockIt->second.kind() == ValueKind::List) {
                    for (const auto& nameValue : std::get<List>(blockIt->second.data)) {
                        std::string hormoneName = stringifyValue(nameValue);
                        Map& hormone = ensureHormone(tape, hormoneName, ins.opcode);
                        hormone["blocked_until"] = Value(tick + blockFor);
                        hormone["blocked_direction"] = Value(std::string("spike"));
                    }
                }
            }
            actions.push_back(mapString(event, "action", ""));
        }

        for (const auto& action : actions) {
            if (halted)
                break;
            std::string upper = toUpperStr(action);
            if (upper == "EXIT" || upper == "HALT" || upper == "SHUTDOWN") {
                halted = true;
                break;
            }
            activeTape = emotionTape;
            Instruction call;
            call.opcode = "CALL";
            call.args.push_back(action);
            execute(call);
        }
        if (!halted)
            activeTape = emotionTape;
    }
    else if (ins.opcode == "EMO_TICK" || ins.opcode == "HORMONE_TICK") {
        long long amount = ins.args.empty()
            ? 1
            : integerOperand(argOrThrow(ins, 0), tape, ins.opcode, "tick amount");
        if (amount < 0)
            throw std::runtime_error(ins.opcode + ": tick amount must be non-negative");
        tape.cells[EMO_CLOCK_CELL] = Value(emotionTick(tape) + amount);
        applyHormoneDrift(tape, amount);
        Instruction check;
        check.opcode = "EMO_CHECK";
        execute(check);
    }
    else if (ins.opcode == "EMO_FIELD" || ins.opcode == "HORMONE_FIELD") {
        tape.current() = Value(emotionField(tape));
    }
    else if (ins.opcode == "EMO_STATE") {
        Map state;
        state["tick"] = Value(emotionTick(tape));
        state["hormones"] = tape.cells[EMO_HORMONES_CELL];
        state["events"] = tape.cells[EMO_EVENTS_CELL];
        state["field"] = Value(emotionField(tape));
        tape.current() = Value(std::move(state));
    }

    // ---- Poet / rational scaffolding -------------------------------------

    else if (ins.opcode == "POET_SEARCH") {
        std::string query = ins.args.empty()
            ? stringifyValue(tape.current())
            : stringOperand(argOrThrow(ins, 0), tape);
        size_t limit = ins.args.size() > 1
            ? sizeOperand(argOrThrow(ins, 1), tape, ins.opcode, "result limit")
            : 8;
        tape.current() = Value(poetSearch(tapes, query, limit));
    }
    else if (ins.opcode == "POET_STRATEGY") {
        std::string problem = ins.args.empty()
            ? stringifyValue(tape.current())
            : stringOperand(argOrThrow(ins, 0), tape);
        tape.current() = Value(poetStrategy(problem));
    }
    else if (ins.opcode == "RATIONAL_RUN") {
        std::string layer = ins.args.empty()
            ? std::string("boolean")
            : stringOperand(argOrThrow(ins, 0), tape);
        tape.current() = Value(rationalEvidence(layer, tape.current()));
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
    //   QUOTE_FUNCTION fn         — full def/end body copied from the loader

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
    else if (ins.opcode == "QUOTE_FUNCTION" || ins.opcode == "QUOTE_FN" ||
             ins.opcode == "FUNCTION_CODE" || ins.opcode == "DEF_CODE" ||
             ins.opcode == "LOAD_FUNCTION" || ins.opcode == "LOAD_FN") {
        std::string name = stringOperand(argOrThrow(ins, 0), tape);
        tape.current() = Value(loader.loadFunctionCode(name));
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
    else if (ins.opcode == "LABEL") {
        // Pseudo-instruction used inside Code cells to preserve quoted local labels.
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
    size_t worksheetDepth = worksheetStack.size();
    std::unordered_map<std::string, size_t> localLabels;
    for (size_t i = 0; i < code.size(); ++i) {
        const auto& ins = code[i];
        if (ins.opcode != "LABEL")
            continue;
        std::string label = argOrThrow(ins, 0);
        if (label.empty() || label[0] != '$')
            throw std::runtime_error("LABEL: labels must use $label syntax, got '" + label + "'");
        localLabels[toUpperStr(label)] = i;
    }

    size_t pc = 0;
    try {
        while (!halted && pc < code.size()) {
            const auto& ins = code[pc];

            if (ins.opcode == "RET") {
                cleanupWorksheets(worksheetDepth);
                return;
            }
            if (ins.opcode == "LABEL") {
                ++pc;
                continue;
            }

            if (ins.opcode == "JMP") {
                std::string label = normalizeJumpLabel(ins);
                auto it = localLabels.find(label);
                if (it == localLabels.end())
                    throw std::runtime_error("JMP: undefined label '" + ins.args[0] + "'");
                pc = it->second;
                continue;
            }

            if (ins.opcode == "JMPIF") {
                if (isTruthy(tapes[activeTape].current())) {
                    std::string label = normalizeJumpLabel(ins);
                    auto it = localLabels.find(label);
                    if (it == localLabels.end())
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
                    auto it = localLabels.find(label);
                    if (it == localLabels.end())
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
        cleanupWorksheets(worksheetDepth);
    } catch (...) {
        cleanupWorksheets(worksheetDepth);
        throw;
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

    size_t worksheetDepth = worksheetStack.size();
    Function fn = loader.loadFunction(entry);
    size_t pc = 0;

    try {
        while (!halted && pc < fn.instructions.size()) {
            const auto& ins = fn.instructions[pc];

            if (ins.opcode == "RET") {
                cleanupWorksheets(worksheetDepth);
                return;
            }
            if (ins.opcode == "LABEL") {
                ++pc;
                continue;
            }

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
        cleanupWorksheets(worksheetDepth);
    } catch (...) {
        cleanupWorksheets(worksheetDepth);
        throw;
    }
}
