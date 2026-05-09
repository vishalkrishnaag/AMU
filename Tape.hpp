#pragma once

#include "Value.hpp"
#include <unordered_map>

struct Tape {
    std::unordered_map<long long, Value> cells;
    long long ptr = 0;

    Value& current() {
        return cells[ptr];
    }

    void moveForward(long long n) {
        ptr += n;
    }

    void moveBackward(long long n) {
        ptr -= n;
    }

    size_t length() const {
        return cells.size();
    }
};