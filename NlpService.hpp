#pragma once

#include "Value.hpp"
#include <string>

class NlpService {
public:
    Map loadModel(const std::string& path);
    List tokenize(const std::string& text) const;
    Map analyze(const std::string& text) const;
    List predict(const std::string& text, int k = 1, double threshold = 0.0) const;
    Map sentiment(const std::string& text, int k = 1, double threshold = 0.0) const;
    double similarity(const std::string& left, const std::string& right, bool forceTokenMode = false) const;
    Map diff(const std::string& left, const std::string& right) const;
};
