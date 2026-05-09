#include "NlpService.hpp"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

const std::unordered_set<std::string>& positiveWords() {
    static const std::unordered_set<std::string> words = {
        "able", "accurate", "benefit", "best", "better", "brilliant",
        "clean", "correct", "efficient", "excellent", "fast", "good",
        "great", "happy", "improve", "improved", "love", "positive",
        "powerful", "reliable", "safe", "stable", "strong", "success",
        "useful", "win", "works"
    };
    return words;
}

const std::unordered_set<std::string>& negativeWords() {
    static const std::unordered_set<std::string> words = {
        "bad", "broken", "bug", "confusing", "danger", "difficult",
        "error", "fail", "failed", "failure", "fragile", "hard",
        "hate", "issue", "negative", "problem", "regression", "risk",
        "slow", "unstable", "weak", "worse", "worst", "wrong"
    };
    return words;
}

std::vector<std::string> tokenStrings(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '_') {
            current.push_back(static_cast<char>(std::tolower(c)));
        } else if (!current.empty()) {
            out.push_back(current);
            current.clear();
        }
    }
    if (!current.empty())
        out.push_back(current);
    return out;
}

double tokenCosine(const std::string& left, const std::string& right) {
    std::unordered_map<std::string, double> a;
    std::unordered_map<std::string, double> b;
    for (const auto& token : tokenStrings(left))
        a[token] += 1.0;
    for (const auto& token : tokenStrings(right))
        b[token] += 1.0;

    double dot = 0.0;
    double normA = 0.0;
    double normB = 0.0;
    for (const auto& [token, count] : a) {
        normA += count * count;
        auto it = b.find(token);
        if (it != b.end())
            dot += count * it->second;
    }
    for (const auto& [token, count] : b)
        normB += count * count;

    if (normA == 0.0 || normB == 0.0)
        return 0.0;
    return dot / (std::sqrt(normA) * std::sqrt(normB));
}

List stringSetToList(const std::set<std::string>& values) {
    List out;
    out.reserve(values.size());
    for (const auto& value : values)
        out.emplace_back(value);
    return out;
}

Map sentimentScores(const std::string& text) {
    long long positive = 0;
    long long negative = 0;
    for (const auto& token : tokenStrings(text)) {
        if (positiveWords().count(token))
            ++positive;
        if (negativeWords().count(token))
            ++negative;
    }

    double total = static_cast<double>(positive + negative);
    double score = total == 0.0 ? 0.0 : (positive - negative) / total;
    std::string polarity = "neutral";
    if (score > 0.0)
        polarity = "positive";
    else if (score < 0.0)
        polarity = "negative";

    Map result;
    result["positive_hits"] = Value(positive);
    result["negative_hits"] = Value(negative);
    result["score"] = Value(score);
    result["polarity"] = Value(polarity);
    result["label"] = Value("__label__" + polarity);
    return result;
}

} // namespace

Map NlpService::loadModel(const std::string& path) {
    Map result;
    result["loaded"] = Value(false);
    result["model"] = Value(path);
    result["backend"] = Value(std::string("core"));
    result["message"] = Value(std::string("External NLP models are not linked by default; using core tape NLP."));
    return result;
}

List NlpService::tokenize(const std::string& text) const {
    List out;
    auto tokens = tokenStrings(text);
    out.reserve(tokens.size());
    for (const auto& token : tokens)
        out.emplace_back(token);
    return out;
}

Map NlpService::analyze(const std::string& text) const {
    auto tokens = tokenStrings(text);
    std::set<std::string> unique(tokens.begin(), tokens.end());
    long long lines = text.empty() ? 0 : 1;
    for (char c : text)
        if (c == '\n')
            ++lines;

    long long instructionLike = 0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos || line[start] == '#')
            continue;
        if (line.find(':') != std::string::npos)
            continue;
        std::string first;
        std::istringstream words(line.substr(start));
        words >> first;
        bool uppercase = !first.empty();
        for (unsigned char c : first) {
            if (std::isalpha(c) && !std::isupper(c)) {
                uppercase = false;
                break;
            }
        }
        if (uppercase)
            ++instructionLike;
    }

    Map result;
    result["chars"] = Value(static_cast<long long>(text.size()));
    result["lines"] = Value(lines);
    result["tokens"] = Value(static_cast<long long>(tokens.size()));
    result["unique_tokens"] = Value(static_cast<long long>(unique.size()));
    result["instruction_like_lines"] = Value(instructionLike);
    result["backend"] = Value(std::string("core"));
    result["sentiment"] = Value(sentimentScores(text));
    return result;
}

List NlpService::predict(const std::string& text, int k, double threshold) const {
    if (k <= 0)
        throw std::runtime_error("NLPPREDICT: k must be positive");

    Map sentiment = sentimentScores(text);
    std::string label = std::get<std::string>(sentiment["label"].data);
    double score = std::abs(std::get<double>(sentiment["score"].data));
    if (score < threshold)
        return List{};
    return List{Value(List{Value(label), Value(score)})};
}

Map NlpService::sentiment(const std::string& text, int k, double threshold) const {
    List predictions = predict(text, k, threshold);
    Map result = sentimentScores(text);
    result["backend"] = Value(std::string("core"));
    result["predictions"] = Value(predictions);
    return result;
}

double NlpService::similarity(const std::string& left, const std::string& right, bool) const {
    return tokenCosine(left, right);
}

Map NlpService::diff(const std::string& left, const std::string& right) const {
    std::set<std::string> leftTokens;
    std::set<std::string> rightTokens;
    for (const auto& token : tokenStrings(left))
        leftTokens.insert(token);
    for (const auto& token : tokenStrings(right))
        rightTokens.insert(token);

    std::set<std::string> shared;
    std::set<std::string> leftOnly;
    std::set<std::string> rightOnly;
    std::set_intersection(leftTokens.begin(), leftTokens.end(),
                          rightTokens.begin(), rightTokens.end(),
                          std::inserter(shared, shared.begin()));
    std::set_difference(leftTokens.begin(), leftTokens.end(),
                        rightTokens.begin(), rightTokens.end(),
                        std::inserter(leftOnly, leftOnly.begin()));
    std::set_difference(rightTokens.begin(), rightTokens.end(),
                        leftTokens.begin(), leftTokens.end(),
                        std::inserter(rightOnly, rightOnly.begin()));

    Map result;
    double sim = similarity(left, right, true);
    result["similarity"] = Value(sim);
    result["distance"] = Value(1.0 - sim);
    result["shared"] = Value(stringSetToList(shared));
    result["left_only"] = Value(stringSetToList(leftOnly));
    result["right_only"] = Value(stringSetToList(rightOnly));
    result["backend"] = Value(std::string("core"));
    return result;
}
