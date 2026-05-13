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

bool isStopword(const std::string& token) {
    static const std::unordered_set<std::string> words = {
        "a", "an", "the", "this", "that", "these", "those", "only"
    };
    return words.count(token) > 0;
}

bool tokenIsNumber(const std::string& token) {
    if (token.empty()) return false;
    return std::all_of(token.begin(), token.end(),
                       [](unsigned char c) { return std::isdigit(c); });
}

std::string normalizeEntityToken(const std::string& token) {
    if (token.size() > 1 && token.back() == 's')
        return token.substr(0, token.size() - 1);
    return token;
}

std::string numberTokenValue(const std::string& token) {
    static const std::unordered_map<std::string, std::string> words = {
        {"zero", "0"}, {"one", "1"}, {"two", "2"}, {"three", "3"},
        {"four", "4"}, {"five", "5"}, {"six", "6"}, {"seven", "7"},
        {"eight", "8"}, {"nine", "9"}, {"ten", "10"}, {"eleven", "11"},
        {"twelve", "12"}
    };
    auto it = words.find(token);
    if (it != words.end()) return it->second;
    return tokenIsNumber(token) ? token : "";
}

long long numberTokenAsInt(const std::string& token) {
    std::string value = numberTokenValue(token);
    if (value.empty()) return 0;
    return std::stoll(value);
}

List tokenListValue(const std::vector<std::string>& tokens) {
    List out;
    out.reserve(tokens.size());
    for (const auto& token : tokens)
        out.emplace_back(token);
    return out;
}

Map relationCandidate(const std::string& kind,
                      const std::string& subject,
                      const std::string& relation,
                      const std::string& object,
                      const std::string& intent,
                      const std::string& routeHint) {
    Map item;
    item["kind"] = Value(kind);
    item["subject"] = Value(subject);
    item["relation"] = Value(relation);
    item["object"] = Value(object);
    item["intent"] = Value(intent);
    item["route_hint"] = Value(routeHint);
    item["logic_weight"] = Value(Map{
        {"state", Value(std::string("assumed"))},
        {"support", Value(std::string("single_observation"))},
        {"scope", Value(std::string("general"))}
    });
    return item;
}

void addRouteHint(List& hints, const std::string& hint) {
    for (const auto& value : hints) {
        if (value.kind() == ValueKind::Str && std::get<std::string>(value.data) == hint)
            return;
    }
    hints.emplace_back(hint);
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

Map NlpService::grammarCheck(const std::string& text) const {
    List errors;
    long long score = 100; // start with perfect score

    // Simple checks
    if (!text.empty() && !std::isupper(text[0])) {
        errors.emplace_back("Sentence does not start with capital letter");
        score -= 10;
    }

    if (!text.empty() && text.back() != '.' && text.back() != '!' && text.back() != '?') {
        errors.emplace_back("Sentence does not end with punctuation");
        score -= 10;
    }

    // Check for double spaces
    if (text.find("  ") != std::string::npos) {
        errors.emplace_back("Contains double spaces");
        score -= 5;
    }

    // Check for common misspellings (very basic)
    std::vector<std::string> misspellings = {"teh", "recieve", "seperate"};
    for (const auto& mis : misspellings) {
        if (text.find(mis) != std::string::npos) {
            errors.emplace_back("Possible misspelling: " + mis);
            score -= 15;
        }
    }

    Map result;
    result["score"] = Value(score);
    result["errors"] = Value(errors);
    result["backend"] = Value(std::string("core"));
    return result;
}

Map NlpService::contextAnalysis(const std::string& text) const {
    auto tokens = tokenStrings(text);
    List nouns, verbs, adjectives;

    // Very basic POS tagging simulation
    for (const auto& token : tokens) {
        if (token.size() > 3 && (token.back() == 's' || token.find("tion") != std::string::npos)) {
            nouns.emplace_back(token);
        } else if (token.size() > 2 && (token.substr(token.size()-2) == "ed" || token.substr(token.size()-3) == "ing")) {
            verbs.emplace_back(token);
        } else if (token.size() > 2) {
            adjectives.emplace_back(token);
        }
    }

    Map result;
    result["nouns"] = Value(nouns);
    result["verbs"] = Value(verbs);
    result["adjectives"] = Value(adjectives);
    result["topic"] = Value("general"); // placeholder
    result["backend"] = Value(std::string("core"));
    return result;
}

Map NlpService::relationExtraction(const std::string& text) const {
    std::vector<std::string> tokens = tokenStrings(text);
    List candidates;
    List routeHints;

    auto addCandidate = [&](Map item) {
        std::string route = "";
        auto it = item.find("route_hint");
        if (it != item.end() && it->second.kind() == ValueKind::Str)
            route = std::get<std::string>(it->second.data);
        if (!route.empty())
            addRouteHint(routeHints, route);
        candidates.emplace_back(std::move(item));
    };

    auto tokenAt = [&](size_t index) -> std::string {
        return index < tokens.size() ? tokens[index] : "";
    };

    // what is 8 plus 9 / 2 times 7
    for (size_t i = 0; i + 2 < tokens.size(); ++i) {
        std::string op = tokens[i + 1];
        if ((op == "plus" || op == "add" || op == "added") &&
            !numberTokenValue(tokens[i]).empty() && !numberTokenValue(tokens[i + 2]).empty()) {
            Map item = relationCandidate("operation", "arithmetic", "addition", "sum",
                                         "compute", "addition");
            item["operands"] = Value(List{
                Value(numberTokenAsInt(tokens[i])),
                Value(numberTokenAsInt(tokens[i + 2]))
            });
            addCandidate(std::move(item));
        }
        if ((op == "times" || op == "multiply" || op == "multiplied") &&
            !numberTokenValue(tokens[i]).empty() && !numberTokenValue(tokens[i + 2]).empty()) {
            Map item = relationCandidate("operation", "arithmetic", "multiplication", "product",
                                         "compute", "multiplication");
            item["operands"] = Value(List{
                Value(numberTokenAsInt(tokens[i])),
                Value(numberTokenAsInt(tokens[i + 2]))
            });
            addCandidate(std::move(item));
        }
    }

    // reverse the word poet
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i] == "reverse") {
            std::string object;
            if (i + 2 < tokens.size() && (tokens[i + 1] == "word" || tokens[i + 1] == "string"))
                object = tokens[i + 2];
            else
                object = tokens[i + 1];
            Map item = relationCandidate("operation", "text", "reverse_word", object,
                                         "transform", "reverse_word");
            addCandidate(std::move(item));
            break;
        }
    }

    // how many legs does a cat have
    if (tokens.size() >= 4 && tokenAt(0) == "how" && tokenAt(1) == "many") {
        std::string attribute = normalizeEntityToken(tokenAt(2));
        std::string subject;
        for (size_t i = 3; i < tokens.size(); ++i) {
            if (tokens[i] == "have" || tokens[i] == "has")
                break;
            if (!isStopword(tokens[i]) && tokens[i] != "does" && tokens[i] != "do")
                subject = normalizeEntityToken(tokens[i]);
        }
        if (!subject.empty() && !attribute.empty()) {
            Map item = relationCandidate("question", subject, attribute + "_count", "",
                                         "answer_lookup", "fact_lookup");
            item["attribute"] = Value(attribute);
            addCandidate(std::move(item));
        }
    }

    // what shape is earth / what is earth shape
    if (tokens.size() >= 4 && tokenAt(0) == "what") {
        if (tokenAt(1) == "shape" && (tokenAt(2) == "is" || tokenAt(2) == "are")) {
            Map item = relationCandidate("question", normalizeEntityToken(tokenAt(3)), "shape", "",
                                         "answer_lookup", "fact_lookup");
            addCandidate(std::move(item));
        } else if ((tokenAt(1) == "is" || tokenAt(1) == "are") && tokenAt(3) == "shape") {
            Map item = relationCandidate("question", normalizeEntityToken(tokenAt(2)), "shape", "",
                                         "answer_lookup", "fact_lookup");
            addCandidate(std::move(item));
        }
    }

    // cat has 4 legs / human have only 2 legs
    for (size_t i = 0; i + 3 < tokens.size(); ++i) {
        if (tokens[i + 1] == "has" || tokens[i + 1] == "have") {
            size_t numberIndex = i + 2;
            if (numberIndex < tokens.size() && isStopword(tokens[numberIndex]))
                ++numberIndex;
            if (numberIndex + 1 < tokens.size() && !numberTokenValue(tokens[numberIndex]).empty()) {
                std::string subject = normalizeEntityToken(tokens[i]);
                std::string attribute = normalizeEntityToken(tokens[numberIndex + 1]);
                std::string object = numberTokenValue(tokens[numberIndex]);
                Map item = relationCandidate("fact", subject, attribute + "_count", object,
                                             "remember", "fact_lookup");
                item["attribute"] = Value(attribute);
                item["answer"] = Value(object);
                item["generated_logic"] = Value("SET " + object);
                addCandidate(std::move(item));
            }
        }
    }

    // fish lives in water / bird lives on tree
    for (size_t i = 0; i + 3 < tokens.size(); ++i) {
        if ((tokens[i + 1] == "lives" || tokens[i + 1] == "live") &&
            (tokens[i + 2] == "in" || tokens[i + 2] == "on" || tokens[i + 2] == "at")) {
            Map item = relationCandidate("fact", normalizeEntityToken(tokens[i]),
                                         "lives_" + tokens[i + 2], normalizeEntityToken(tokens[i + 3]),
                                         "remember", "fact_lookup");
            item["generated_logic"] = Value("SET " + tokens[i + 3]);
            addCandidate(std::move(item));
        }
    }

    // X is Y. Keep this broad but low-assumption.
    for (size_t i = 0; i + 2 < tokens.size(); ++i) {
        if (tokens[i + 1] == "is" || tokens[i + 1] == "are") {
            std::string subject = normalizeEntityToken(tokens[i]);
            std::string object = normalizeEntityToken(tokens[i + 2]);
            if (!isStopword(subject) && !isStopword(object) && subject != "what") {
                Map item = relationCandidate("fact", subject, "is", object,
                                             "remember", "classification");
                item["logic_weight"] = Value(Map{
                    {"state", Value(std::string("weak_assumption"))},
                    {"support", Value(std::string("surface_statement"))},
                    {"scope", Value(std::string("contextual"))}
                });
                addCandidate(std::move(item));
            }
        }
    }

    Map result;
    result["backend"] = Value(std::string("core"));
    result["text"] = Value(text);
    result["tokens"] = Value(tokenListValue(tokens));
    result["candidates"] = Value(candidates);
    result["route_hints"] = Value(routeHints);
    result["status"] = Value(candidates.empty() ? std::string("no_relation_found")
                                                : std::string("relations_found"));
    if (!candidates.empty())
        result["primary"] = candidates.front();
    return result;
}

Map NlpService::logicalOperation(const std::string& statement, const std::string& operation) const {
    // Parse simple "if P then Q"
    std::string lower = statement;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    std::string p, q;
    size_t if_pos = lower.find("if ");
    size_t then_pos = lower.find(" then ");
    if (if_pos != std::string::npos && then_pos != std::string::npos && then_pos > if_pos) {
        p = statement.substr(if_pos + 3, then_pos - if_pos - 3);
        q = statement.substr(then_pos + 6);
    } else {
        Map result;
        result["error"] = Value("Unable to parse logical statement");
        return result;
    }

    std::string result_statement;
    if (operation == "contrapositive") {
        result_statement = "if not " + q + " then not " + p;
    } else if (operation == "converse") {
        result_statement = "if " + q + " then " + p;
    } else {
        result_statement = "Unknown operation";
    }

    Map result;
    result["original"] = Value(statement);
    result["operation"] = Value(operation);
    result["result"] = Value(result_statement);
    result["backend"] = Value(std::string("core"));
    return result;
}

Map NlpService::fuzzyLogic(const std::string& input, double membership) const {
    // Simple fuzzy membership for sentiment
    double fuzzy_positive = 0.0;
    double fuzzy_negative = 0.0;

    auto tokens = tokenStrings(input);
    for (const auto& token : tokens) {
        if (positiveWords().count(token)) {
            fuzzy_positive += membership;
        }
        if (negativeWords().count(token)) {
            fuzzy_negative += membership;
        }
    }

    std::string dominant = "neutral";
    if (fuzzy_positive > fuzzy_negative) dominant = "positive";
    else if (fuzzy_negative > fuzzy_positive) dominant = "negative";

    Map result;
    result["fuzzy_positive"] = Value(fuzzy_positive);
    result["fuzzy_negative"] = Value(fuzzy_negative);
    result["dominant"] = Value(dominant);
    result["membership_threshold"] = Value(membership);
    result["backend"] = Value(std::string("core"));
    return result;
}

Map NlpService::probabilityCalc(const std::string& data, const std::string& type) const {
    auto tokens = tokenStrings(data);
    std::unordered_map<std::string, long long> freq;

    for (const auto& token : tokens) {
        freq[token]++;
    }

    long long total = tokens.size();
    Map probabilities;
    for (const auto& [token, count] : freq) {
        probabilities[token] = Value(static_cast<double>(count) / total);
    }

    Map result;
    result["total_tokens"] = Value(total);
    result["unique_tokens"] = Value(static_cast<long long>(freq.size()));
    result["probabilities"] = Value(probabilities);
    result["type"] = Value(type);
    result["backend"] = Value(std::string("core"));
    return result;
}
