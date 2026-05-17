#include "FunctionLoader.hpp"
#include "Parser.hpp"
#include "Lexer.hpp"
#include <algorithm>
#include <sstream>
#include <cctype>
#include <utility>

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

static bool endFromLine(const std::string& line) {
    auto tokens = Lexer::tokenize(line);
    return tokens.size() == 1 && toUpper(tokens[0]) == "END";
}

static bool hormoneBlockFromLine(const std::string& line, std::string& name) {
    auto tokens = Lexer::tokenize(line);
    if (tokens.size() != 2)
        return false;
    std::string opcode = toUpper(tokens[0]);
    if (opcode != "HORMONE" && opcode != "HORMONE_DEF" && opcode != "HORMONE_CREATE")
        return false;
    name = tokens[1];
    return !name.empty();
}

static void setHormoneField(
    const std::string& key,
    const std::string& value,
    std::string& level,
    std::string& nature,
    std::string& minValue,
    std::string& maxValue,
    std::string& baseline,
    std::string& decay,
    std::string& sensitivity,
    std::string& weight
) {
    std::string upper = toUpper(key);
    if (upper == "LEVEL" || upper == "HORMONE_LEVEL") {
        level = value;
    } else if (upper == "NATURE" || upper == "HORMONE_NATURE" ||
               upper == "BEHAVIOR" || upper == "HORMONE_BEHAVIOR" ||
               upper == "TYPE" || upper == "HORMONE_TYPE") {
        nature = value;
    } else if (upper == "MIN" || upper == "HORMONE_MIN") {
        minValue = value;
    } else if (upper == "MAX" || upper == "HORMONE_MAX" || upper == "HORMOE_MAX") {
        maxValue = value;
    } else if (upper == "BASELINE" || upper == "HORMONE_BASELINE") {
        baseline = value;
    } else if (upper == "DECAY" || upper == "HORMONE_DECAY") {
        decay = value;
    } else if (upper == "SENSITIVITY" || upper == "HORMONE_SENSITIVITY") {
        sensitivity = value;
    } else if (upper == "WEIGHT" || upper == "HORMONE_WEIGHT") {
        weight = value;
    } else {
        throw std::runtime_error("Unknown hormone field: " + key);
    }
}

static Instruction parseHormoneBlock(
    const std::vector<std::string>& lines,
    size_t& index,
    const std::string& functionName,
    int lineNumber
) {
    std::string hormoneName;
    if (!hormoneBlockFromLine(lines[index], hormoneName))
        throw std::runtime_error("Internal error: expected HORMONE block");

    std::string level;
    std::string nature = "custom";
    std::string minValue = "0";
    std::string maxValue = "100";
    std::string baseline;
    std::string decay = "0";
    std::string sensitivity = "1";
    std::string weight = "1";

    for (size_t i = index + 1; i < lines.size(); ++i) {
        if (endFromLine(lines[i])) {
            if (level.empty())
                throw std::runtime_error("HORMONE " + hormoneName + " is missing hormone_level");
            if (baseline.empty())
                baseline = level;

            Instruction ins;
            ins.line = lineNumber;
            ins.opcode = "HORMONE";
            ins.args = {
                hormoneName,
                level,
                nature,
                minValue,
                maxValue,
                baseline,
                decay,
                sensitivity,
                weight
            };
            index = i;
            return ins;
        }

        std::string label;
        if (defFromLine(lines[i], label))
            throw std::runtime_error("Function " + functionName +
                                     " has HORMONE " + hormoneName +
                                     " missing end before def " + label);
        if (labelFromLine(lines[i], label))
            throw std::runtime_error("HORMONE " + hormoneName +
                                     " cannot contain labels; close it with end first");

        auto tokens = Lexer::tokenize(lines[i]);
        if (tokens.empty())
            continue;
        if (tokens.size() % 2 != 0)
            throw std::runtime_error("HORMONE " + hormoneName +
                                     " fields must be key value pairs");
        for (size_t j = 0; j < tokens.size(); j += 2) {
            setHormoneField(tokens[j], tokens[j + 1], level, nature, minValue,
                            maxValue, baseline, decay, sensitivity, weight);
        }
    }

    throw std::runtime_error("Function " + functionName +
                             " has HORMONE " + hormoneName + " missing end");
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
        if (defFromLine(lines[i], label))
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

        if (endFromLine(lines[i])) {
            startLine = i;
            break;
        }

        if (defFromLine(lines[i], label))
            throw std::runtime_error("Function " + name + " is missing end before def " + label);

        std::string hormoneName;
        if (hormoneBlockFromLine(lines[i], hormoneName)) {
            auto ins = parseHormoneBlock(lines, i, name, lineNumber++);
            fn.instructions.push_back(std::move(ins));
            continue;
        }

        if (labelFromLine(lines[i], label)) {
            if (label[0] == '$') {
                // In-function jump target: $label: — record index, don't emit as instruction
                fn.localLabels[toUpper(trim(label))] = fn.instructions.size();
                continue;
            }
            throw std::runtime_error("Function " + name + " uses label '" + label +
                                     ":'; functions must start with def, and local labels must use $label:");
        }

        auto ins = Parser::parse(lines[i], lineNumber++);
        if (!ins.opcode.empty())
            fn.instructions.push_back(ins);
    }

    if (startLine >= lines.size() || !endFromLine(lines[startLine]))
        throw std::runtime_error("Function " + name + " is missing end");

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

std::vector<Instruction> FunctionLoader::loadFunctionCode(const std::string& name) {
    std::string normName = toUpper(name);

    if (!labelIndex.count(normName))
        throw std::runtime_error("Function not found: " + name);

    size_t startLine = labelIndex[normName] + 1;
    std::vector<Instruction> code;
    int lineNumber = 0;

    for (size_t i = startLine; i < lines.size(); ++i) {
        std::string label;

        if (endFromLine(lines[i])) {
            startLine = i;
            break;
        }

        if (defFromLine(lines[i], label))
            throw std::runtime_error("Function " + name + " is missing end before def " + label);

        std::string hormoneName;
        if (hormoneBlockFromLine(lines[i], hormoneName)) {
            auto ins = parseHormoneBlock(lines, i, name, lineNumber++);
            code.push_back(std::move(ins));
            continue;
        }

        if (labelFromLine(lines[i], label)) {
            if (label[0] == '$') {
                Instruction marker;
                marker.opcode = "LABEL";
                marker.args.push_back(toUpper(trim(label)));
                code.push_back(std::move(marker));
                continue;
            }
            throw std::runtime_error("Function " + name + " uses label '" + label +
                                     ":'; functions must start with def, and local labels must use $label:");
        }

        auto ins = Parser::parse(lines[i], lineNumber++);
        if (!ins.opcode.empty())
            code.push_back(ins);
    }

    if (startLine >= lines.size() || !endFromLine(lines[startLine]))
        throw std::runtime_error("Function " + name + " is missing end");

    return code;
}
