#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

struct Problem {
    std::string name;
    std::string universe;
    std::string expected;
};

struct AmuResult {
    int id = 0;
    bool qualified = false;
    std::string output;
    fs::path root;
};

static std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

static std::string intenseString(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:   out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

static std::string readFile(const fs::path& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void writeFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    if (!out) throw std::runtime_error("Cannot write " + path.string());
    out << text;
}

static int runCommandCapture(const std::string& command, const fs::path& outputPath) {
    std::string full = command + " > " + shellQuote(outputPath.string()) + " 2>&1";
    return std::system(full.c_str());
}

static void writeAmuRunner(
    const fs::path& runner,
    const Problem& problem,
    const fs::path& generatedPath
) {
    std::ostringstream src;
    src << "IMPORT ../poet_algorithm.in10s\n\n"
        << "main:\n"
        << "    SETARG 0 " << intenseString(problem.universe) << "\n"
        << "    SETARG 1 " << intenseString(generatedPath.string()) << "\n"
        << "    CALL poet_generate\n"
        << "    CALL persist_generated_algorithm_to_arg\n"
        << "    CALL execute_generated_answer\n"
        << "    PRINT\n"
        << "    RET\n";
    writeFile(runner, src.str());
}

static AmuResult runAmu(
    int id,
    const fs::path& repoRoot,
    const Problem& problem
) {
    AmuResult result;
    result.id = id;
    result.root = repoRoot / "training" / ("AMU" + std::to_string(id));

    fs::path memoryDir = result.root / "memory";
    fs::path algorithmsDir = memoryDir / "generated_algorithms";
    fs::path tracesDir = result.root / "traces";
    fs::create_directories(algorithmsDir);
    fs::create_directories(tracesDir);

    fs::path generated = algorithmsDir / (problem.name + "_candidate.in10s");
    fs::path runner = result.root / "poet_runner.in10s";
    fs::path poetOutput = tracesDir / "poet_stdout.txt";
    fs::path candidateOutput = tracesDir / "candidate_stdout.txt";

    writeAmuRunner(runner, problem, generated);

    std::string runPoet = "./intense.out " + shellQuote(runner.string()) + " main 8";
    int poetCode = runCommandCapture(runPoet, poetOutput);
    result.output = readFile(poetOutput);

    if (poetCode != 0 || !fs::exists(generated)) {
        result.qualified = false;
        return result;
    }

    std::string runCandidate = "./intense.out " + shellQuote(generated.string()) + " main 4";
    int candidateCode = runCommandCapture(runCandidate, candidateOutput);
    std::string candidate = readFile(candidateOutput);
    result.output += candidate;

    result.qualified = candidateCode == 0 &&
        candidate.find(problem.expected) != std::string::npos;

    std::ostringstream report;
    report << "AMU" << id << "\n"
           << "problem: " << problem.universe << "\n"
           << "expected: " << problem.expected << "\n"
           << "qualified: " << (result.qualified ? "true" : "false") << "\n";
    writeFile(result.root / "score.txt", report.str());

    return result;
}

static void copyQualifiedMemory(const std::vector<AmuResult>& results, const fs::path& outputRoot) {
    fs::remove_all(outputRoot);
    fs::create_directories(outputRoot / "memory" / "generated_algorithms");

    int copied = 0;
    for (const auto& result : results) {
        if (!result.qualified) continue;
        fs::path src = result.root / "memory" / "generated_algorithms";
        if (!fs::exists(src)) continue;

        for (const auto& entry : fs::directory_iterator(src)) {
            if (!entry.is_regular_file()) continue;
            fs::path dest = outputRoot / "memory" / "generated_algorithms" /
                ("AMU" + std::to_string(result.id) + "_" + entry.path().filename().string());
            fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing);
            ++copied;
        }
    }

    std::ostringstream manifest;
    manifest << "qualified_memory_files: " << copied << "\n";
    writeFile(outputRoot / "manifest.txt", manifest.str());
}

static int defaultThreadCount() {
    unsigned int detected = std::thread::hardware_concurrency();
    if (detected == 0) return 1;
    return std::max(1, static_cast<int>(detected));
}

static int defaultAmuCount() {
    return defaultThreadCount();
}

int main(int argc, char** argv) {
    int amuCount = argc > 1 ? std::max(1, std::stoi(argv[1])) : defaultAmuCount();
    int maxThreads = argc > 2 ? std::max(1, std::stoi(argv[2])) : defaultThreadCount();

    fs::path repoRoot = fs::current_path();
    Problem problem{
        "cat_legs",
        "how many legs a cat have",
        "4"
    };

    std::vector<AmuResult> results(static_cast<size_t>(amuCount));
    std::atomic<int> next{1};
    std::mutex printMutex;

    auto worker = [&]() {
        while (true) {
            int id = next.fetch_add(1);
            if (id > amuCount) break;

            AmuResult result = runAmu(id, repoRoot, problem);
            results[static_cast<size_t>(id - 1)] = result;

            std::lock_guard<std::mutex> lock(printMutex);
            std::cout << "AMU" << id << " "
                      << (result.qualified ? "qualified" : "failed")
                      << "\n";
        }
    };

    std::vector<std::thread> threads;
    int threadCount = std::min(amuCount, maxThreads);
    std::cout << "training AMUs=" << amuCount
              << " worker_threads=" << threadCount << "\n";
    for (int i = 0; i < threadCount; ++i)
        threads.emplace_back(worker);
    for (auto& t : threads)
        t.join();

    fs::path outputRoot = repoRoot / "training" / "output_AMU";
    copyQualifiedMemory(results, outputRoot);

    int qualified = 0;
    for (const auto& r : results)
        if (r.qualified) ++qualified;

    std::cout << "qualified " << qualified << " / " << amuCount << "\n"
              << "merged memory: " << outputRoot << "\n";
    return qualified == 0 ? 1 : 0;
}
