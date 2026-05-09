#pragma once

#include "Instruction.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <list>
#include <fstream>
#include <filesystem>

struct Function {
    std::vector<Instruction> instructions;
    // @label: targets within the function body → instruction index
    std::unordered_map<std::string, size_t> localLabels;
};

class FunctionLoader {
private:
    std::string filename;

    std::vector<std::string> lines;

    // Keys are uppercase-normalized label names
    std::unordered_map<std::string, size_t> labelIndex;

    std::unordered_map<std::string, Function> cache;

    // Doubly-linked list: front = LRU, back = MRU
    std::list<std::string> lru;

    // O(1) iterator access for splice-based LRU update
    std::unordered_map<std::string, std::list<std::string>::iterator> lruIters;

    std::unordered_set<std::string> importedFiles;

    static constexpr int MAX_FUNCTIONS = 100;

    void loadFileFromPath(const std::filesystem::path& path);
    void importPath(const std::string& importTarget, const std::filesystem::path& baseDir);

public:
    FunctionLoader(const std::string& file);

    void loadFile();
    void buildIndex();
    Function loadFunction(const std::string& name);
};
