#include "VM.hpp"
#include "Parser.hpp"
#include <Eigen/Dense>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <numeric>
#include <vector>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Internal helpers (anonymous namespace)
// ---------------------------------------------------------------------------
namespace {

// ---- String utilities ------------------------------------------------------

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
    if (isInteger(token))                  return Value(std::stoll(token));
    if (isFloat(token))                    return Value(std::stod(token));
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

std::vector<std::string> splitOnSemicolon(const std::string& s) {
    std::vector<std::string> parts;
    std::string current;
    int depth = 0;
    bool inStr = false;
    bool escaped = false;

    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (escaped) { current.push_back(c); escaped = false; continue; }
        if (inStr) {
            current.push_back(c);
            if (c == '\\') escaped = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; current.push_back(c); continue; }
        if (c == '(' || c == '[' || c == '{') { depth++; current.push_back(c); continue; }
        if (c == ')' || c == ']' || c == '}') { depth--; current.push_back(c); continue; }
        if (c == ';' && depth == 0) { parts.push_back(current); current.clear(); continue; }
        current.push_back(c);
    }
    if (!current.empty()) parts.push_back(current);
    return parts;
}

Value parseCodeBlockToken(const std::string& token) {
    // token is "(MOVE 10 ; SET \"hello\" ; PRINT)"
    if (token.size() < 2 || token.front() != '(' || token.back() != ')')
        throw std::runtime_error("Invalid code block token");
    std::string inner = token.substr(1, token.size() - 2);
    Code result;
    for (const auto& part : splitOnSemicolon(inner)) {
        std::string piece = trimStr(part);
        if (piece.empty()) continue;
        auto ins = Parser::parse(piece, 0);
        if (!ins.opcode.empty())
            result.push_back(ins);
    }
    return Value(std::move(result));
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
        try { return Value(std::stoll(token)); } catch (...) {}
    }
    if (isFloat(token)) {
        try { return Value(std::stod(token)); } catch (...) {}
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
            return tape.cells[std::stoll(rest)];
    }
    return parseValue(arg);
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
            if (isInteger(s)) return static_cast<double>(std::stoll(s));
            if (isFloat(s))   return std::stod(s);
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
    : tapes(tapeCount), debugMode(debug), stepMode(step), loader(file)
{}

void VM::debug(const Instruction& ins) {
    if (!debugMode) return;

    std::cout << "\n[DEBUG] OP=" << ins.opcode
              << "  TAPE=" << activeTape
              << "  PTR=" << tapes[activeTape].ptr
              << "  LEN=" << tapes[activeTape].length() << "\n";

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
    const size_t cellWidth = 12;
    const size_t maxOffscreenCells = 6;

    int shownTape = tapeIndex < 0 ? activeTape : tapeIndex;
    if (shownTape < 0 || shownTape >= static_cast<int>(tapes.size()))
        throw std::runtime_error("PRINT_TAPE index out of bounds: " + std::to_string(shownTape));

    std::ostringstream out;
    out << "\nTAPE active=tape" << activeTape
        << " showing=tape" << shownTape
        << " count=" << tapes.size() << "\n";

    const Tape& tape = tapes[shownTape];
    long long start = tape.ptr - windowRadius;
    if (tape.ptr >= 0 && start < 0)
        start = 0;
    const long long end = start + (windowRadius * 2);

    out << (shownTape == activeTape ? "=> " : "   ")
        << "tape" << shownTape
        << " ptr=" << tape.ptr
        << " cells=" << tape.length();

    if (tape.cells.empty()) {
        out << " empty";
    }
    out << "\n";

    out << "      idx ";
    for (long long pos = start; pos <= end; ++pos)
        out << fitColumn(std::to_string(pos), cellWidth);
    out << "\n";

    out << "      val ";
    for (long long pos = start; pos <= end; ++pos) {
        auto it = tape.cells.find(pos);
        std::string cell = it == tape.cells.end()
            ? "."
            : compactDisplayValue(it->second, cellWidth - 2);
        out << fitColumn(cell, cellWidth);
    }
    out << "\n";

    out << "      head";
    for (long long pos = start; pos <= end; ++pos)
        out << fitColumn(pos == tape.ptr ? "^" : "", cellWidth);
    out << "\n";

    std::vector<long long> offscreen;
    for (const auto& [pos, _] : tape.cells) {
        if (pos < start || pos > end)
            offscreen.push_back(pos);
    }
    std::sort(offscreen.begin(), offscreen.end());

    if (!offscreen.empty()) {
        out << "      off-window ";
        size_t shown = std::min(maxOffscreenCells, offscreen.size());
        for (size_t i = 0; i < shown; ++i) {
            long long pos = offscreen[i];
            out << "[" << pos << ":"
                << compactDisplayValue(tape.cells.at(pos), 18) << "]";
            if (i + 1 < shown)
                out << " ";
        }
        if (offscreen.size() > shown)
            out << " ... +" << (offscreen.size() - shown) << " more";
        out << "\n";
    }

    return out.str();
}

// ---------------------------------------------------------------------------
// execute — handles all opcodes except JMP/JMPIF/JMPNOT (those live in run)
// ---------------------------------------------------------------------------

void VM::execute(const Instruction& ins) {
    debug(ins);

    auto& tape = tapes[activeTape];

    // ---- Tape navigation --------------------------------------------------

    if (ins.opcode == "MOVE") {
        tape.moveForward(std::stoll(argOrThrow(ins, 0)));
    }
    else if (ins.opcode == "BACK") {
        tape.moveBackward(std::stoll(argOrThrow(ins, 0)));
    }

    // ---- Core cell ops ----------------------------------------------------

    else if (ins.opcode == "SET") {
        tape.current() = parseValue(argOrThrow(ins, 0));
    }
    else if (ins.opcode == "PRINT") {
        std::cout << stringifyValue(tape.current()) << "\n";
    }
    else if (ins.opcode == "PRINTJ") {
        std::cout << stringifyJsonValue(tape.current()) << "\n";
    }

    // ---- Type system ------------------------------------------------------

    else if (ins.opcode == "TYPE") {
        tape.current() = Value(std::string(kindName(tape.current().kind())));
    }
    else if (ins.opcode == "CAST") {
        std::string target = argOrThrow(ins, 0);
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
        tape.ptr = std::stoll(argOrThrow(ins, 0));
    }

    else if (ins.opcode == "TAPE") {
        int target = std::stoi(argOrThrow(ins, 0));
        if (target < 0 || target >= static_cast<int>(tapes.size()))
            throw std::runtime_error("TAPE index out of bounds: " + std::to_string(target));
        activeTape = target;
    }
    else if (ins.opcode == "PRINT_TAPE") {
        int target = std::stoi(argOrThrow(ins, 0));
        std::cout << tapeSnapshot(3, target);
    }
    else if (ins.opcode == "LEN" || ins.opcode == "LENGTH") {
        tape.current() = Value(static_cast<long long>(tape.length()));
    }
    else if (ins.opcode == "CMP" || ins.opcode == "COMPARE") {
        int other = std::stoi(argOrThrow(ins, 0));
        if (other < 0 || other >= static_cast<int>(tapes.size()))
            throw std::runtime_error("CMP tape index out of bounds: " + std::to_string(other));
        tape.current() = Value(valuesEqual(tape.current(), tapes[other].current()));
    }
    else if (ins.opcode == "COPY") {
        // Copy current cell to the same ptr position on dest tape
        int dest = std::stoi(argOrThrow(ins, 0));
        if (dest < 0 || dest >= static_cast<int>(tapes.size()))
            throw std::runtime_error("COPY tape index out of bounds: " + std::to_string(dest));
        tapes[dest].cells[tapes[dest].ptr] = tape.current();
    }
    else if (ins.opcode == "DELETE") {
        tape.cells.erase(tape.ptr);
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
        std::string path = stringifyValue(parseValue(argOrThrow(ins, 0)));
        std::ifstream file(path);
        if (!file) throw std::runtime_error("Cannot open file: " + path);
        std::stringstream buf;
        buf << file.rdbuf();
        tape.current() = Value(buf.str());
    }
    else if (ins.opcode == "WRITEFILE") {
        std::string path = stringifyValue(parseValue(argOrThrow(ins, 0)));
        std::ofstream file(path);
        if (!file) throw std::runtime_error("Cannot write file: " + path);
        file << stringifyValue(tape.current());
    }

    // ---- String operations ------------------------------------------------

    else if (ins.opcode == "CONCAT") {
        std::string rhs = stringifyValue(parseValue(argOrThrow(ins, 0)));
        tape.current() = Value(stringifyValue(tape.current()) + rhs);
    }
    else if (ins.opcode == "SPLIT") {
        std::string delim = stringifyValue(parseValue(argOrThrow(ins, 0)));
        std::string src   = stringifyValue(tape.current());
        List out;
        size_t pos = 0, prev = 0;
        while ((pos = src.find(delim, prev)) != std::string::npos) {
            out.emplace_back(src.substr(prev, pos - prev));
            prev = pos + delim.size();
        }
        out.emplace_back(src.substr(prev));
        tape.current() = Value(std::move(out));
    }
    else if (ins.opcode == "SUBSTR") {
        int start = std::stoi(argOrThrow(ins, 0));
        int len   = std::stoi(argOrThrow(ins, 1));
        if (tape.current().kind() != ValueKind::Str)
            throw std::runtime_error("SUBSTR: current cell is not a string");
        tape.current() = Value(std::get<std::string>(tape.current().data).substr(start, len));
    }
    else if (ins.opcode == "FIND") {
        std::string sub = stringifyValue(parseValue(argOrThrow(ins, 0)));
        if (tape.current().kind() != ValueKind::Str)
            throw std::runtime_error("FIND: current cell is not a string");
        auto pos = std::get<std::string>(tape.current().data).find(sub);
        tape.current() = Value(static_cast<long long>(pos));
    }
    else if (ins.opcode == "REPLACE") {
        std::string oldStr = stringifyValue(parseValue(argOrThrow(ins, 0)));
        std::string repStr = stringifyValue(parseValue(argOrThrow(ins, 1)));
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

    // ---- NLP service operations ------------------------------------------
    // Core tape NLP: tokenization, analysis, similarity, diff, and transparent
    // lexicon sentiment. NLPLOAD is a compatibility hook for future backends.

    else if (ins.opcode == "NLPLOAD") {
        std::string path = ins.args.empty()
            ? stringifyValue(tape.current())
            : stringifyValue(parseValue(argOrThrow(ins, 0)));
        tape.current() = Value(nlp.loadModel(path));
    }
    else if (ins.opcode == "NLPTOKENS") {
        tape.current() = Value(nlp.tokenize(stringifyValue(tape.current())));
    }
    else if (ins.opcode == "NLPANALYZE") {
        tape.current() = Value(nlp.analyze(stringifyValue(tape.current())));
    }
    else if (ins.opcode == "NLPSIM") {
        long long otherCell = std::stoll(argOrThrow(ins, 0));
        bool forceTokenMode = false;
        if (ins.args.size() > 1) {
            std::string mode = toUpperStr(stringifyValue(parseValue(ins.args[1])));
            forceTokenMode = mode == "TOKEN" || mode == "TOKENS";
        }
        std::string left = stringifyValue(tape.current());
        std::string right = stringifyValue(tape.cells[otherCell]);
        tape.current() = Value(nlp.similarity(left, right, forceTokenMode));
    }
    else if (ins.opcode == "NLPDIFF") {
        long long otherCell = std::stoll(argOrThrow(ins, 0));
        std::string left = stringifyValue(tape.current());
        std::string right = stringifyValue(tape.cells[otherCell]);
        tape.current() = Value(nlp.diff(left, right));
    }
    else if (ins.opcode == "NLPPREDICT") {
        int k = ins.args.size() > 0 ? std::stoi(ins.args[0]) : 1;
        double threshold = ins.args.size() > 1 ? std::stod(ins.args[1]) : 0.0;
        tape.current() = Value(nlp.predict(stringifyValue(tape.current()), k, threshold));
    }
    else if (ins.opcode == "NLPSENTIMENT") {
        int k = ins.args.size() > 0 ? std::stoi(ins.args[0]) : 1;
        double threshold = ins.args.size() > 1 ? std::stod(ins.args[1]) : 0.0;
        tape.current() = Value(nlp.sentiment(stringifyValue(tape.current()), k, threshold));
    }

    // ---- I/O: stdin -------------------------------------------------------

    else if (ins.opcode == "INPUT") {
        std::string line;
        std::getline(std::cin, line);
        if (ins.args.empty()) {
            tape.current() = Value(line);
        } else {
            std::string typeArg = ins.args[0];
            std::transform(typeArg.begin(), typeArg.end(), typeArg.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (typeArg == "int") {
                tape.current() = Value(static_cast<long long>(std::stoll(line)));
            } else if (typeArg == "float") {
                tape.current() = Value(std::stod(line));
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
            Instruction parsed = Parser::parse(s, 0);
            if (!parsed.opcode.empty())
                execute(parsed);
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
                long long catchCell = std::stoll(argOrThrow(ins, 0));
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
            : stringifyValue(parseValue(argOrThrow(ins, 0)));
        throw std::runtime_error(message);
    }

    // ---- Homoiconic: MATCH ------------------------------------------------
    // Match the current cell against a pattern; store bool result in current cell.
    // Patterns: type names ("int","str",...), "_" wildcard, literals, list/map structures.

    else if (ins.opcode == "MATCH") {
        Value pattern = parseValue(argOrThrow(ins, 0));
        Value original = tape.current();
        tape.current() = Value(matchPattern(original, pattern));
    }

    // ---- Code manipulation -----------------------------------------------

    else if (ins.opcode == "CODELEN") {
        auto* code = std::get_if<Code>(&tape.current().data);
        if (!code) throw std::runtime_error("CODELEN: current cell must be Code");
        tape.current() = Value(static_cast<long long>(code->size()));
    }
    else if (ins.opcode == "CODEGET") {
        int idx = std::stoi(argOrThrow(ins, 0));
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
        int instrIdx  = std::stoi(argOrThrow(ins, 0));
        long long cellIdx = std::stoll(argOrThrow(ins, 1));
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
        long long cellIdx = std::stoll(argOrThrow(ins, 0));
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
        Value operand = parseValue(argOrThrow(ins, 0));
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
        long long otherCell = std::stoll(argOrThrow(ins, 0));
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
        long long modelCell = std::stoll(argOrThrow(ins, 0));
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
        int k        = std::stoi(argOrThrow(ins, 0));
        int maxIters = ins.args.size() > 1 ? std::stoi(ins.args[1]) : 100;

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
        size_t n = static_cast<size_t>(std::stoull(argOrThrow(ins, 0)));
        Value v = ins.args.size() > 1 ? parseValue(ins.args[1]) : tape.current();
        if (argRegs.size() <= n) argRegs.resize(n + 1);
        argRegs[n] = v;
    }
    else if (ins.opcode == "GETARG") {
        size_t n = static_cast<size_t>(std::stoull(argOrThrow(ins, 0)));
        tape.current() = n < argRegs.size() ? argRegs[n] : Value();
    }
    else if (ins.opcode == "CLEARARGS") {
        argRegs.clear();
    }
    else if (ins.opcode == "ARGCOUNT") {
        tape.current() = Value(static_cast<long long>(argRegs.size()));
    }

    // ---- Old inline-arg system (kept for backward compat) -----------------

    else if (ins.opcode == "ARG") {
        int index = std::stoi(argOrThrow(ins, 0));
        if (callStack.empty())
            throw std::runtime_error("ARG used outside of a function call");
        const Frame& frame = callStack.top();
        if (index < 0 || index >= static_cast<int>(frame.args.size()))
            throw std::runtime_error("ARG index out of range: " + std::to_string(index));
        tape.current() = frame.args[index];
    }
    else if (ins.opcode == "CALL") {
        Frame frame;
        frame.functionName = argOrThrow(ins, 0);
        for (size_t i = 1; i < ins.args.size(); ++i)
            frame.args.push_back(parseValue(ins.args[i]));
        // Save arg registers; callee inherits them (reads via GETARG),
        // may overwrite for sub-calls. Restored to caller's snapshot on RET.
        frame.savedArgRegs = argRegs;
        callStack.push(frame);
        run(frame.functionName);
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
        if (ins.opcode == "RET") return;
        execute(ins);
    }
}

// ---------------------------------------------------------------------------
// run — execute a named function; handles JMP/JMPIF/JMPNOT via pc manipulation
// ---------------------------------------------------------------------------

void VM::run(const std::string& entry) {
    Function fn = loader.loadFunction(entry);
    size_t pc = 0;

    while (pc < fn.instructions.size()) {
        const auto& ins = fn.instructions[pc];

        if (ins.opcode == "RET") return;

        if (ins.opcode == "JMP") {
            std::string label = toUpperStr(argOrThrow(ins, 0));
            auto it = fn.localLabels.find(label);
            if (it == fn.localLabels.end())
                throw std::runtime_error("JMP: undefined label '" + ins.args[0] + "'");
            pc = it->second;
            continue;
        }

        if (ins.opcode == "JMPIF") {
            if (isTruthy(tapes[activeTape].current())) {
                std::string label = toUpperStr(argOrThrow(ins, 0));
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
                std::string label = toUpperStr(argOrThrow(ins, 0));
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
