#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct TestCase {
    std::string name;
    std::string expected;
};

struct CandidateResult {
    fs::path path;
    bool passed = false;
    std::string output;
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

static std::vector<fs::path> findCandidates(const fs::path& outputAmuRoot) {
    std::vector<fs::path> candidates;
    fs::path dir = outputAmuRoot / "memory" / "generated_algorithms";
    if (!fs::exists(dir)) return candidates;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".in10s")
            candidates.push_back(entry.path());
    }
    std::sort(candidates.begin(), candidates.end());
    return candidates;
}

static CandidateResult testCandidate(
    const fs::path& repoRoot,
    const fs::path& candidate,
    const TestCase& test,
    const fs::path& traceDir
) {
    CandidateResult result;
    result.path = candidate;

    fs::path trace = traceDir / (candidate.stem().string() + "_" + test.name + ".txt");
    std::string command = "./intense.out " + shellQuote(candidate.string()) + " main 4";
    int code = runCommandCapture(command, trace);
    result.output = readFile(trace);
    result.passed = code == 0 && result.output.find(test.expected) != std::string::npos;
    return result;
}

static bool testOutputAmu(const fs::path& repoRoot, const TestCase& test, std::ostream& log) {
    fs::path outputRoot = repoRoot / "training" / "output_AMU";
    fs::path traceDir = outputRoot / "test_traces";
    fs::create_directories(traceDir);

    std::vector<fs::path> candidates = findCandidates(outputRoot);
    log << "testing output_AMU candidates=" << candidates.size() << "\n";

    bool anyPassed = false;
    for (const auto& candidate : candidates) {
        CandidateResult result = testCandidate(repoRoot, candidate, test, traceDir);
        log << candidate.filename().string() << ": "
            << (result.passed ? "valid" : "invalid")
            << " output=" << result.output;
        if (!result.output.empty() && result.output.back() != '\n')
            log << "\n";
        anyPassed = anyPassed || result.passed;
    }

    std::ostringstream report;
    report << "last_test_passed: " << (anyPassed ? "true" : "false") << "\n"
           << "candidate_count: " << candidates.size() << "\n";
    writeFile(outputRoot / "test_report.txt", report.str());
    return anyPassed;
}

static bool runTrainingEpoch(int amuCount, int workerThreads) {
    std::string build = "g++ -std=c++17 -O2 -pthread training/trainer.cpp -o training/trainer.out";
    if (std::system(build.c_str()) != 0)
        return false;

    std::string train = "training/trainer.out " + std::to_string(amuCount) + " " +
        std::to_string(workerThreads);
    return std::system(train.c_str()) == 0;
}

int main(int argc, char** argv) {
    int amuCount = argc > 1 ? std::max(1, std::stoi(argv[1])) : 4;
    int workerThreads = argc > 2 ? std::max(1, std::stoi(argv[2])) : 2;
    int maxRetries = argc > 3 ? std::max(0, std::stoi(argv[3])) : 1;

    fs::path repoRoot = fs::current_path();
    TestCase test{"cat_legs", "4"};

    for (int attempt = 0; attempt <= maxRetries; ++attempt) {
        std::cout << "test attempt " << (attempt + 1) << " / " << (maxRetries + 1) << "\n";
        if (testOutputAmu(repoRoot, test, std::cout)) {
            std::cout << "output_AMU valid\n";
            return 0;
        }

        if (attempt == maxRetries)
            break;

        std::cout << "output_AMU invalid; training again\n";
        if (!runTrainingEpoch(amuCount, workerThreads)) {
            std::cerr << "training retry failed\n";
            return 2;
        }
    }

    std::cout << "output_AMU invalid after retries\n";
    return 1;
}
