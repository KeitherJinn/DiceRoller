#include "DiceAnalysis.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

namespace dice {
namespace {
using Distribution = std::map<std::int64_t, double>;
constexpr std::size_t MaxStates = 20000;
constexpr std::size_t MaxExactWork = 4000000;
constexpr std::size_t MaxEnumerated = 200000;
struct ExactLimit {};
void Cancel(const CancelCheck& cancelled) { if (cancelled && cancelled()) throw Cancelled(); }
struct Budget {
    std::size_t used = 0;
    const CancelCheck& cancelled;
    void Spend(std::size_t amount) {
        Cancel(cancelled);
        if (amount > MaxExactWork - used) throw ExactLimit{};
        used += amount;
    }
};
Distribution Convolve(const Distribution& left, const Distribution& right, Budget& budget) {
    if (left.empty() || right.empty()) throw std::logic_error("An exact distribution is empty.");
    if (right.size() > MaxExactWork / left.size()) throw ExactLimit{};
    budget.Spend(left.size() * right.size());
    Distribution result;
    for (const auto& a : left) {
        Cancel(budget.cancelled);
        for (const auto& b : right) {
            if (a.second == 0 || b.second == 0) continue;
            result[CheckedAdd(a.first, b.first)] += a.second * b.second;
            if (result.size() > MaxStates) throw ExactLimit{};
        }
    }
    return result;
}
Distribution TermDistribution(const Term& term, int threshold, Budget& budget) {
    const int retained = term.keep ? term.keep : term.count;
    if (threshold > 0) {
        if (threshold == 1) return {{retained, 1.0}};
        if (threshold > term.sides) return {{0, 1.0}};
        const double chance = static_cast<double>(static_cast<std::int64_t>(term.sides) - threshold + 1) / term.sides;
        Distribution distribution{{0, 1.0}};
        for (int i = 0; i < term.count; ++i)
            distribution = Convolve(distribution, {{0, 1.0 - chance}, {1, chance}}, budget);
        if (!term.keep || term.keep == term.count) return distribution;
        Distribution kept;
        for (const auto& item : distribution) {
            const auto successes = term.keepHighest ? (std::min)(item.first, static_cast<std::int64_t>(retained))
                : (std::max)(std::int64_t(0), item.first - (term.count - retained));
            kept[successes] += item.second;
        }
        return kept;
    }
    if (term.sides == 1) return {{retained, 1.0}};
    if (!term.keep || term.keep == term.count) {
        if (static_cast<std::size_t>(term.sides) > MaxStates) throw ExactLimit{};
        Distribution die;
        for (int i = 1; i <= term.sides; ++i) die.emplace(i, 1.0 / term.sides);
        Distribution distribution{{0, 1.0}};
        for (int i = 0; i < term.count; ++i) distribution = Convolve(distribution, die, budget);
        return distribution;
    }
    std::size_t possibilities = 1;
    for (int i = 0; i < term.count; ++i) {
        if (static_cast<std::size_t>(term.sides) > MaxEnumerated / possibilities) throw ExactLimit{};
        possibilities *= term.sides;
    }
    budget.Spend(possibilities * static_cast<std::size_t>(term.count));
    Distribution distribution;
    std::vector<int> values(static_cast<std::size_t>(term.count), 1);
    for (std::size_t sample = 0; sample < possibilities; ++sample) {
        if (sample % 256 == 0) Cancel(budget.cancelled);
        auto sorted = values;
        std::sort(sorted.begin(), sorted.end());
        std::int64_t total = 0;
        const int start = term.keepHighest ? term.count - retained : 0;
        for (int i = 0; i < retained; ++i) total += sorted[static_cast<std::size_t>(start + i)];
        distribution[total] += 1.0 / static_cast<double>(possibilities);
        if (distribution.size() > MaxStates) throw ExactLimit{};
        for (auto& value : values) {
            if (++value <= term.sides) break;
            value = 1;
        }
    }
    return distribution;
}
Distribution Exact(const Spec& spec, Mode mode, Budget& budget) {
    Distribution distribution{{0, 1.0}};
    for (const auto& term : DiceTerms(spec))
        distribution = Convolve(distribution, TermDistribution(term, spec.successThreshold, budget), budget);
    Distribution result;
    double cumulative = 0;
    for (const auto& item : distribution) {
        const double before = cumulative;
        cumulative += item.second;
        double probability = item.second;
        if (mode == Mode::Advantage) probability *= cumulative + before;
        else if (mode == Mode::Disadvantage) probability *= (std::max)(0.0, 2.0 - cumulative - before);
        result[CheckedAdd(item.first, spec.modifier)] = probability;
    }
    return result;
}
std::pair<std::int64_t, std::int64_t> Bounds(const Spec& spec) {
    std::int64_t minimum = 0, maximum = 0;
    for (const auto& term : DiceTerms(spec)) {
        const int retained = term.keep ? term.keep : term.count;
        if (spec.successThreshold > 0) {
            minimum += spec.successThreshold == 1 ? retained : 0;
            maximum += spec.successThreshold <= term.sides ? retained : 0;
        } else {
            minimum += retained;
            maximum += static_cast<std::int64_t>(retained) * term.sides;
        }
    }
    return {CheckedAdd(minimum, spec.modifier), CheckedAdd(maximum, spec.modifier)};
}
std::int64_t SaturatingSubtract(std::int64_t a, std::int64_t b) {
    if (b > 0 && a < INT64_MIN + b) return INT64_MIN;
    if (b < 0 && a > INT64_MAX + b) return INT64_MAX;
    return a - b;
}
void Normalize(Distribution& distribution) {
    double sum = 0;
    for (const auto& item : distribution) sum += item.second;
    if (!(sum > 0) || !std::isfinite(sum)) throw std::runtime_error("Probability calculation failed.");
    for (auto& item : distribution) item.second /= sum;
}
long double IntegerDifference(std::int64_t value, std::int64_t origin) {
    // Subtract in integer space before floating conversion. Unsigned subtraction
    // also represents distances that span INT64_MIN through INT64_MAX safely.
    return value >= origin
        ? static_cast<long double>(static_cast<std::uint64_t>(value) - static_cast<std::uint64_t>(origin))
        : -static_cast<long double>(static_cast<std::uint64_t>(origin) - static_cast<std::uint64_t>(value));
}
struct IntegerSum {
    // Two-limb two's-complement sum: exact for any in-memory vector of int64s.
    // MSVC has no portable int128 and its long double has double precision.
    std::uint64_t high = 0, low = 0;
    void Add(std::int64_t value) {
        const auto previous = low;
        low += static_cast<std::uint64_t>(value);
        high += (value < 0 ? UINT64_MAX : 0) + (low < previous ? 1 : 0);
    }
};
std::pair<std::uint64_t, std::uint64_t> Divide(std::uint64_t high, std::uint64_t low, std::uint64_t denominator) {
    std::uint64_t quotient = 0, remainder = 0;
    for (int bit = 127; bit >= 0; --bit) {
        const bool overflow = (remainder >> 63) != 0;
        const auto incoming = bit >= 64 ? ((high >> (bit - 64)) & 1) : ((low >> bit) & 1);
        remainder = (remainder << 1) | incoming;
        if (overflow || remainder >= denominator) {
            remainder -= denominator;
            // The absolute mean of int64 values always fits in uint64.
            if (bit >= 64) throw std::overflow_error("The mean exceeds its supported range.");
            quotient |= std::uint64_t(1) << bit;
        }
    }
    return {quotient, remainder};
}
std::wstring DecimalText(bool negative, std::uint64_t whole, unsigned thousandths) {
    if (thousandths == 1000) { ++whole; thousandths = 0; }
    std::wstring text = negative && (whole || thousandths) ? L"-" : L"";
    text += std::to_wstring(whole);
    if (thousandths) {
        auto digits = std::to_wstring(thousandths + 1000).substr(1);
        while (digits.back() == L'0') digits.pop_back();
        text += L'.' + digits;
    }
    return text;
}
std::wstring MeanText(IntegerSum sum, std::uint64_t count, double* approximate = nullptr) {
    const bool negative = (sum.high >> 63) != 0;
    if (negative) {
        sum.low = ~sum.low + 1;
        sum.high = ~sum.high + (sum.low == 0 ? 1 : 0);
    }
    auto parts = Divide(sum.high, sum.low, count);
    if (approximate) {
        const auto value = static_cast<double>(parts.first) + static_cast<double>(parts.second) / static_cast<double>(count);
        *approximate = negative ? -value : value;
    }
    unsigned decimal = 0;
    for (int place = 0; place < 4; ++place) {
        // Long division by one decimal digit, without overflowing remainder*10.
        std::uint64_t remainder = 0;
        unsigned digit = 0;
        for (int step = 0; step < 10; ++step) {
            if (parts.second >= count - remainder) { remainder = parts.second - (count - remainder); ++digit; }
            else remainder += parts.second;
        }
        parts.second = remainder;
        if (place < 3) decimal = decimal * 10 + digit;
        else if (digit >= 5) ++decimal;
    }
    return DecimalText(negative, parts.first, decimal);
}
std::wstring OffsetText(std::int64_t origin, long double offset) {
    // Valid expressions span at most 1000*INT_MAX. Separate the small fractional
    // offset before adding its integer part to the potentially huge origin.
    const auto integerOffset = static_cast<std::int64_t>(std::floor(offset));
    const auto whole = CheckedAdd(origin, integerOffset);
    const auto fraction = offset - integerOffset;
    if (whole >= 0) return DecimalText(false, static_cast<std::uint64_t>(whole), static_cast<unsigned>(std::llround(fraction * 1000)));
    auto magnitude = std::uint64_t(0) - static_cast<std::uint64_t>(whole);
    return fraction > 0 ? DecimalText(true, magnitude - 1, static_cast<unsigned>(std::llround((1 - fraction) * 1000))) : DecimalText(true, magnitude, 0);
}
}

bool EvaluateTarget(std::int64_t value, const Target& target) {
    if (!target.enabled) return false;
    switch (target.comparison) {
    case Compare::AtLeast: return value >= target.value;
    case Compare::AtMost: return value <= target.value;
    case Compare::Equal: return value == target.value;
    case Compare::Greater: return value > target.value;
    case Compare::Less: return value < target.value;
    }
    throw std::invalid_argument("Unknown target comparison.");
}

std::int64_t TargetMargin(std::int64_t value, const Target& target) {
    if (!target.enabled) return 0;
    switch (target.comparison) {
    case Compare::AtLeast: return SaturatingSubtract(value, target.value);
    case Compare::AtMost: return SaturatingSubtract(target.value, value);
    case Compare::Greater: {
        // Subtract before saturating so INT64_MAX - INT64_MIN - 1 stays maximal.
        if (value <= target.value) return SaturatingSubtract(SaturatingSubtract(value, target.value), 1);
        return SaturatingSubtract(value, CheckedAdd(target.value, 1));
    }
    case Compare::Less: {
        if (value >= target.value) return SaturatingSubtract(SaturatingSubtract(target.value, value), 1);
        return SaturatingSubtract(CheckedAdd(target.value, -1), value);
    }
    case Compare::Equal: return value <= target.value ? SaturatingSubtract(value, target.value) : SaturatingSubtract(target.value, value);
    }
    throw std::invalid_argument("Unknown target comparison.");
}

Statistics Summarize(const Batch& batch) {
    Statistics result;
    result.count = batch.results.size();
    result.hasTarget = batch.request.target.enabled;
    if (!result.count) return result;
    result.minimum = result.maximum = batch.results.front().finalValue;
    const auto origin = batch.results.front().finalValue;
    long double meanOffset = 0, squared = 0;
    IntegerSum exactSum;
    std::size_t count = 0;
    for (const auto& roll : batch.results) {
        result.minimum = (std::min)(result.minimum, roll.finalValue);
        result.maximum = (std::max)(result.maximum, roll.finalValue);
        ++result.frequencies[roll.finalValue];
        if (EvaluateTarget(roll.finalValue, batch.request.target)) ++result.matched;
        exactSum.Add(roll.finalValue);
        const auto offset = IntegerDifference(roll.finalValue, origin);
        const auto delta = offset - meanOffset;
        meanOffset += delta / static_cast<long double>(++count);
        squared += delta * (offset - meanOffset);
    }
    result.meanText = MeanText(exactSum, static_cast<std::uint64_t>(result.count), &result.mean);
    result.standardDeviation = std::sqrt((std::max)(0.0, static_cast<double>(squared / result.count)));
    result.matchedProportion = static_cast<double>(result.matched) / result.count;
    const std::size_t lowerIndex = (result.count - 1) / 2, upperIndex = result.count / 2;
    std::size_t seen = 0;
    std::int64_t lower = 0, upper = 0;
    for (const auto& item : result.frequencies) {
        if (seen <= lowerIndex && lowerIndex < seen + item.second) lower = item.first;
        if (seen <= upperIndex && upperIndex < seen + item.second) { upper = item.first; break; }
        seen += item.second;
    }
    // Split integer halves before conversion: INT64_MIN and INT64_MAX have
    // median -0.5, which is lost if both endpoints are rounded to double first.
    IntegerSum middle; middle.Add(lower); middle.Add(upper);
    result.medianText = MeanText(middle, 2, &result.median);
    return result;
}

Probability Analyze(const Request& request, const CancelCheck& cancelled) {
    auto singleRoll = request;
    singleRoll.times = 1;
    Validate(singleRoll);
    Cancel(cancelled);
    Probability result;
    result.hasTarget = request.target.enabled;
    const auto bounds = Bounds(request.spec);
    result.minimum = bounds.first;
    result.maximum = bounds.second;
    Budget budget{0, cancelled};
    try {
        result.distribution = Exact(request.spec, request.mode, budget);
        result.method = L"Exact distribution (bounded convolution / enumeration)";
    } catch (const ExactLimit&) {
        result.exact = false;
        const auto work = static_cast<std::size_t>(request.spec.count) * (request.mode == Mode::Normal ? 1 : 2);
        result.samples = (std::min)(std::size_t(100000), (std::max)(std::size_t(1000), std::size_t(2000000) / work));
        std::mt19937 engine(0x44494345u);
        const NextDie next = [&engine](int sides) { return std::uniform_int_distribution<int>(1, sides)(engine); };
        for (std::size_t sample = 0; sample < result.samples; ++sample) {
            Cancel(cancelled);
            const auto roll = Roll(request.spec, request.mode, next, cancelled);
            result.distribution[roll.finalValue] += 1.0 / result.samples;
        }
        result.marginOfError95 = std::sqrt(std::log(40.0) / (2.0 * result.samples));
        result.method = L"Monte Carlo estimate (fixed seed; 95% target-proportion error bound)";
    }
    Normalize(result.distribution);
    long double meanOffset = 0;
    for (const auto& item : result.distribution) {
        meanOffset += IntegerDifference(item.first, result.minimum) * item.second;
        if (EvaluateTarget(item.first, request.target)) result.matchedProportion += item.second;
    }
    meanOffset = (std::clamp)(meanOffset, 0.0L, IntegerDifference(result.maximum, result.minimum));
    result.mean = static_cast<double>(static_cast<long double>(result.minimum) + meanOffset);
    result.meanText = OffsetText(result.minimum, meanOffset);
    result.matchedProportion = (std::clamp)(result.matchedProportion, 0.0, 1.0);
    Cancel(cancelled);
    return result;
}
}
