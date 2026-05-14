#include "FunctionLoader.hpp"
#include "Parser.hpp"
#include "Lexer.hpp"
#include <algorithm>
#include <sstream>
#include <cctype>

FunctionLoader::FunctionLoader(const std::string& file)
    : filename(file)
{
    loadFile();
    buildIndex();
}

static std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

static std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return s.substr(start);
}

static bool labelFromLine(const std::string& line, std::string& label) {
    auto tokens = Lexer::tokenize(line);
    if (tokens.size() != 1 || tokens[0].empty() || tokens[0].back() != ':')
        return false;

    label = tokens[0].substr(0, tokens[0].size() - 1);
    return !label.empty();
}

static bool defFromLine(const std::string& line, std::string& label) {
    auto tokens = Lexer::tokenize(line);
    if (tokens.size() != 2 || toUpper(tokens[0]) != "DEF")
        return false;

    label = tokens[1];
    return !label.empty() && label[0] != '$';
}

static bool functionStartFromLine(const std::string& line, std::string& label) {
    if (defFromLine(line, label))
        return true;
    if (labelFromLine(line, label))
        return !label.empty() && label[0] != '$';
    return false;
}

static bool endFromLine(const std::string& line) {
    auto tokens = Lexer::tokenize(line);
    return tokens.size() == 1 && toUpper(tokens[0]) == "END";
}

static bool isIntenseSource(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    return ext == ".intense" || ext == ".in10s";
}

static std::string unquoteImportTarget(const std::string& token) {
    if (token.size() >= 2 &&
        ((token.front() == '"' && token.back() == '"') ||
         (token.front() == '\'' && token.back() == '\''))) {
        return token.substr(1, token.size() - 2);
    }
    return token;
}

void FunctionLoader::loadFile() {
    std::filesystem::path root(filename);

    if (std::filesystem::is_directory(root)) {
        for (auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) continue;
            if (isIntenseSource(entry.path()))
                loadFileFromPath(entry.path());
        }
    } else {
        loadFileFromPath(root);
    }
}

void FunctionLoader::loadFileFromPath(const std::filesystem::path& path) {
    auto canonicalPath = std::filesystem::weakly_canonical(path).string();
    if (importedFiles.count(canonicalPath)) return;
    importedFiles.insert(canonicalPath);

    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open file: " + path.string());

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        auto tokens = Lexer::tokenize(line);
        if (!tokens.empty() && toUpper(tokens[0]) == "IMPORT") {
            if (tokens.size() < 2)
                throw std::runtime_error("IMPORT missing target path in " + path.string());
            importPath(unquoteImportTarget(tokens[1]), path.parent_path());
            continue;
        }

        lines.push_back(line);
    }
}

void FunctionLoader::importPath(const std::string& importTarget, const std::filesystem::path& baseDir) {
    std::filesystem::path target(importTarget);
    if (!target.is_absolute())
        target = baseDir / target;

    if (std::filesystem::is_directory(target)) {
        for (auto& entry : std::filesystem::recursive_directory_iterator(target)) {
            if (!entry.is_regular_file()) continue;
            if (isIntenseSource(entry.path()))
                loadFileFromPath(entry.path());
        }
        return;
    }

    if (!std::filesystem::exists(target)) {
        std::filesystem::path fallback = target;
        fallback.replace_extension(".in10s");
        if (std::filesystem::exists(fallback)) {
            target = fallback;
        } else {
            fallback.replace_extension(".intense");
            if (std::filesystem::exists(fallback))
                target = fallback;
            else
                throw std::runtime_error("Imported file not found: " + target.string());
        }
    }

    loadFileFromPath(target);
}

void FunctionLoader::buildIndex() {
    labelIndex.clear();
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string label;
        if (functionStartFromLine(lines[i], label))
            labelIndex[toUpper(trim(label))] = i;
    }
}

void FunctionLoader::appendSource(const std::string& source, const std::filesystem::path& baseDir) {
    std::istringstream in(source);
    std::string line;

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        auto tokens = Lexer::tokenize(line);
        if (!tokens.empty() && toUpper(tokens[0]) == "IMPORT") {
            if (tokens.size() < 2)
                throw std::runtime_error("IMPORT missing target path in REPL source");
            importPath(unquoteImportTarget(tokens[1]), baseDir);
            continue;
        }

        lines.push_back(line);
    }

    cache.clear();
    lru.clear();
    lruIters.clear();
    buildIndex();
}

Function FunctionLoader::loadFunction(const std::string& name) {
    std::string normName = toUpper(name);

    // Cache hit — splice to back of LRU list (O(1))
    if (cache.count(normName)) {
        lru.splice(lru.end(), lru, lruIters[normName]);
        return cache[normName];
    }

    if (!labelIndex.count(normName))
        throw std::runtime_error("Function not found: " + name);

    size_t startLine = labelIndex[normName] + 1;

    Function fn;
    int lineNumber = 0;

    for (size_t i = startLine; i < lines.size(); ++i) {
        std::string label;

        if (endFromLine(lines[i]))
            break;

        if (defFromLine(lines[i], label))
            break;

        if (labelFromLine(lines[i], label)) {
            if (label[0] == '$') {
                // In-function jump target: $label: — record index, don't emit as instruction
                fn.localLabels[toUpper(trim(label))] = fn.instructions.size();
                continue;
            }
            // Global function label → end of this function
            break;
        }

        auto ins = Parser::parse(lines[i], lineNumber++);
        if (!ins.opcode.empty())
            fn.instructions.push_back(ins);
    }

    // Evict LRU entry if at capacity
    if ((int)cache.size() >= MAX_FUNCTIONS) {
        std::string evicted = lru.front();
        lruIters.erase(evicted);
        cache.erase(evicted);
        lru.pop_front();
    }

    cache[normName] = fn;
    lru.push_back(normName);
    lruIters[normName] = std::prev(lru.end());

    return fn;
}
