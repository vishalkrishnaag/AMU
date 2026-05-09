#pragma once

#include "Instruction.hpp"
#include <variant>
#include <vector>
#include <unordered_map>
#include <string>

enum class ValueKind {
    Nil   = 0,
    Bool  = 1,
    Int   = 2,
    Float = 3,
    Char  = 4,
    Str   = 5,
    List  = 6,
    Map   = 7,
    Code  = 8   // executable symbolic block: vector<Instruction>
};

inline const char* kindName(ValueKind k) {
    switch (k) {
        case ValueKind::Nil:   return "nil";
        case ValueKind::Bool:  return "bool";
        case ValueKind::Int:   return "int";
        case ValueKind::Float: return "float";
        case ValueKind::Char:  return "char";
        case ValueKind::Str:   return "str";
        case ValueKind::List:  return "list";
        case ValueKind::Map:   return "map";
        case ValueKind::Code:  return "code";
    }
    return "unknown";
}

struct Value;

using List = std::vector<Value>;
using Map  = std::unordered_map<std::string, Value>;
using Code = std::vector<Instruction>;

struct Value {
    // Variant indices must stay in sync with ValueKind ordinals
    using Variant = std::variant<
        std::monostate,  // 0 Nil
        bool,            // 1 Bool
        long long,       // 2 Int
        double,          // 3 Float
        char,            // 4 Char
        std::string,     // 5 Str
        List,            // 6 List
        Map,             // 7 Map
        Code             // 8 Code
    >;

    Variant data;

    Value() : data(std::monostate{}) {}
    Value(std::nullptr_t) : data(std::monostate{}) {}
    Value(bool v)               : data(v) {}
    Value(long long v)          : data(v) {}
    Value(double v)             : data(v) {}
    Value(char v)               : data(v) {}
    Value(const std::string& v) : data(v) {}
    Value(std::string&& v)      : data(std::move(v)) {}
    Value(const List& v)        : data(v) {}
    Value(List&& v)             : data(std::move(v)) {}
    Value(const Map& v)         : data(v) {}
    Value(Map&& v)              : data(std::move(v)) {}
    Value(const Code& v)        : data(v) {}
    Value(Code&& v)             : data(std::move(v)) {}

    ValueKind kind() const {
        return static_cast<ValueKind>(data.index());
    }

    bool operator==(const Value& other) const {
        return data == other.data;
    }
};
