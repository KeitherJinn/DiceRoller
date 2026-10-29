#pragma once
#include "DiceCore.h"
#include <map>

namespace dice {
bool EvaluateTarget(std::int64_t value, const Target& target);
// Signed distance in the comparison's success direction; saturates on overflow.
// Strict comparisons include their one-integer step. Equal returns -abs(delta).
std::int64_t TargetMargin(std::int64_t value, const Target& target);
struct Statistics {
    std::size_t count = 0;
    std::int64_t minimum = 0, maximum = 0;
    double mean = 0, median = 0, standardDeviation = 0;
    std::map<std::int64_t, std::size_t> frequencies;
    std::size_t matched = 0;
    double matchedProportion = 0;
    bool hasTarget = false;
};
struct Probability {
    bool exact = true;
    std::size_t samples = 0; // Zero for exact calculation.
    std::int64_t minimum = 0, maximum = 0; // Theoretical bounds, even for sampling.
    double mean = 0;
    std::map<std::int64_t, double> distribution;
    bool hasTarget = false;
    double matchedProportion = 0;
    // 95% Hoeffding bound for the target proportion when Monte Carlo is used;
    // not an error bound for the mean or for all histogram bins simultaneously.
    double marginOfError95 = 0;
    std::wstring method;
};
Statistics Summarize(const Batch& batch);
// Bounded exact convolution/enumeration, otherwise fixed-seed Monte Carlo.
// Does not consume the application's live random generator or create a roll.
// Models one roll; Request.times does not affect its probability distribution.
Probability Analyze(const Request& request, const CancelCheck& cancelled = {});
}
