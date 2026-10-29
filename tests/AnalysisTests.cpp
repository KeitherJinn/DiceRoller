#include "DiceAnalysis.h"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void Near(double actual, double expected, const char* message, double tolerance = 1e-10) {
    Check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}
template<class Action> void Reject(Action action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    Check(rejected, "Invalid analysis/rules input was accepted.");
}
dice::Request MakeRequest(const wchar_t* notation) {
    dice::Request request;
    request.name = L"analysis test";
    request.notation = notation;
    request.spec = dice::Parse(notation);
    return request;
}
dice::Result Fixed(const dice::Spec& spec, dice::Mode mode, const std::vector<int>& values) {
    std::size_t index = 0;
    auto result = dice::Roll(spec, mode, [&](int) { return values.at(index++); });
    Check(index == values.size(), "Roll did not consume the expected dice.");
    return result;
}
void CheckExactAgainstEnumeration(const dice::Request& request) {
    std::vector<int> sides;
    const int groups = request.mode == dice::Mode::Normal ? 1 : 2;
    for (int group = 0; group < groups; ++group)
        for (const auto& term : dice::DiceTerms(request.spec))
            sides.insert(sides.end(), static_cast<std::size_t>(term.count), term.sides);
    std::size_t combinations = 1;
    for (int side : sides) combinations *= side;
    Check(combinations <= 10000, "Enumeration test fixture is unexpectedly large.");
    std::vector<int> values(sides.size(), 1);
    std::map<std::int64_t, double> expected;
    for (std::size_t sample = 0; sample < combinations; ++sample) {
        const auto result = Fixed(request.spec, request.mode, values);
        expected[result.finalValue] += 1.0 / static_cast<double>(combinations);
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (++values[i] <= sides[i]) break;
            values[i] = 1;
        }
    }
    const auto actual = dice::Analyze(request);
    Check(actual.exact && actual.distribution.size() == expected.size(), "Exact distribution support differs from enumerated rolls.");
    for (const auto& item : expected) Near(actual.distribution.at(item.first), item.second, "Exact probability differs from enumerated rolls.");
}
}

void AnalysisTests() {
    using namespace dice;
    const auto mixed = Parse(L" 2D6 + d4 + 3 ");
    Check(mixed.count == 3 && mixed.sides == 6 && mixed.modifier == 3 && mixed.terms.size() == 2, "Mixed expression parse failed.");
    Check(CanonicalNotation(mixed) == L"2d6+1d4+3", "Mixed expression canonicalization failed.");
    auto roll = Fixed(mixed, Mode::Normal, {2, 6, 4});
    Check(roll.finalValue == 15 && roll.first.total == 12 && roll.first.dice == std::vector<int>({2, 6, 4}) &&
        roll.first.sides == std::vector<int>({6, 6, 4}) && roll.first.kept == std::vector<bool>({true, true, true}), "Mixed roll audit is incorrect.");
    Check(Parse(L"d6+3+d4-1").modifier == 2 && Parse(L"d20").terms.empty(), "Modifier accumulation or legacy representation failed.");
    for (const auto* invalid : {L"d6-d4", L"4d6kh0", L"4d6kh5", L"4d6k3", L"4d6kl", L"4d6kh2kh1", L"1000d6+d4", L"d6++d4", L"3+d6"})
        Reject([&] { Parse(invalid); });
    auto keep = Parse(L"4d6kh3");
    roll = Fixed(keep, Mode::Normal, {1, 6, 3, 4});
    Check(roll.finalValue == 13 && roll.first.kept == std::vector<bool>({false, true, true, true}), "Keep-highest failed.");
    roll = Fixed(Parse(L"4d6kl2"), Mode::Normal, {1, 6, 3, 4});
    Check(roll.finalValue == 4 && roll.first.kept == std::vector<bool>({true, false, true, false}), "Keep-lowest failed.");
    roll = Fixed(Parse(L"3d6kh1"), Mode::Normal, {6, 6, 1});
    Check(roll.first.kept == std::vector<bool>({true, false, false}), "Keep ties are not stable.");
    roll = Fixed(Parse(L"2d6kh1+d4"), Mode::Advantage, {6, 1, 1, 3, 4, 4});
    Check(roll.first.total == 7 && roll.second->total == 8 && roll.chosen == 8, "Advantage must compare whole mixed expressions.");
    keep.successThreshold = 4;
    roll = Fixed(keep, Mode::Normal, {1, 6, 3, 4});
    Check(roll.finalValue == 2 && roll.first.dice.size() == 4, "Dice pool must count retained successes.");
    auto pool = Parse(L"2d6+d4");
    pool.successThreshold = 5;
    Check(Fixed(pool, Mode::Normal, {5, 2, 4}).finalValue == 1, "Mixed dice-pool threshold failed.");
    pool.modifier = 1;
    Reject([&] { Validate(pool, 1, Mode::Normal); });
    pool.modifier = 0;
    pool.successThreshold = -1;
    Reject([&] { Validate(pool, 1, Mode::Normal); });
    auto large = Parse(L"500d6+500d4");
    Validate(large, 500, Mode::Advantage);
    Reject([&] { Validate(large, 501, Mode::Advantage); });
    Reject([] { Parse(L"d1+d1+9223372036854775806"); });
    auto inconsistent = mixed;
    inconsistent.count = 2;
    Reject([&] { Validate(inconsistent, 1, Mode::Normal); });
    Check(CanonicalNotation(Parse(L"4D6KL2+2d8-3")) == L"4d6kl2+2d8-3", "Keep expression round trip failed.");
    auto mismatch = MakeRequest(L"d6");
    mismatch.spec.modifier = 1;
    Reject([&] { Validate(mismatch); });
    mismatch = MakeRequest(L"d6");
    mismatch.spec.successThreshold = 4;
    Validate(mismatch);
    mismatch.name = L"bad\x1f name";
    Reject([&] { Validate(mismatch); });
    mismatch = MakeRequest(L"d6");
    mismatch.notation = L"d6\t";
    Reject([&] { Validate(mismatch); });
    mismatch.notation = std::wstring(1022, L' ') + L"1d6";
    Reject([&] { Validate(mismatch); });
    auto excessiveTerms = Parse(L"d6");
    excessiveTerms.terms.resize(1001);
    Reject([&] { Validate(excessiveTerms, 1, Mode::Normal); });

    Target target{true, Compare::AtLeast, 10};
    Check(EvaluateTarget(10, target) && !EvaluateTarget(9, target) && TargetMargin(12, target) == 2, "At-least target failed.");
    target.comparison = Compare::AtMost;
    Check(EvaluateTarget(9, target) && !EvaluateTarget(11, target) && TargetMargin(8, target) == 2, "At-most target failed.");
    target.comparison = Compare::Equal;
    Check(EvaluateTarget(10, target) && !EvaluateTarget(11, target) && TargetMargin(8, target) == -2, "Equality target failed.");
    target.comparison = Compare::Greater;
    Check(!EvaluateTarget(10, target) && TargetMargin(10, target) == -1 && TargetMargin(11, target) == 0, "Strict greater target failed.");
    target.comparison = Compare::Less;
    Check(!EvaluateTarget(10, target) && TargetMargin(10, target) == -1 && TargetMargin(9, target) == 0, "Strict less target failed.");
    target = {true, Compare::AtLeast, INT64_MIN};
    Check(TargetMargin(INT64_MAX, target) == INT64_MAX, "Target margin did not saturate upward.");
    target = {true, Compare::Greater, INT64_MAX};
    Check(!EvaluateTarget(INT64_MAX, target) && TargetMargin(INT64_MIN, target) == INT64_MIN, "Impossible strict target overflowed.");
    target = {true, Compare::Less, INT64_MIN};
    Check(!EvaluateTarget(INT64_MIN, target) && TargetMargin(INT64_MAX, target) == INT64_MIN, "Lower strict target overflowed.");
    Check(!EvaluateTarget(3, {}) && TargetMargin(3, {}) == 0, "Disabled target should not report a match.");

    Batch batch;
    batch.request = MakeRequest(L"d6");
    batch.request.target = {true, Compare::AtLeast, 4};
    for (int value : {1, 3, 4, 6}) batch.results.push_back(Fixed(batch.request.spec, Mode::Normal, {value}));
    const auto statistics = Summarize(batch);
    Check(statistics.count == 4 && statistics.minimum == 1 && statistics.maximum == 6 && statistics.matched == 2 &&
        statistics.frequencies.at(3) == 1 && statistics.hasTarget, "Batch statistics counts/range failed.");
    Near(statistics.mean, 3.5, "Statistics mean failed.");
    Near(statistics.median, 3.5, "Statistics median failed.");
    Near(statistics.standardDeviation, std::sqrt(3.25), "Statistics standard deviation failed.");
    Near(statistics.matchedProportion, 0.5, "Statistics match proportion failed.");
    Check(Summarize({}).count == 0, "Empty batch statistics failed.");
    Batch extremes;
    Result low, high;
    low.finalValue = INT64_MIN;
    high.finalValue = INT64_MAX;
    extremes.results = {low, high};
    Near(Summarize(extremes).median, -0.5, "Extreme integer median lost its fractional part.");

    auto request = MakeRequest(L"2d6");
    request.target = {true, Compare::AtLeast, 7};
    auto probability = Analyze(request);
    Check(probability.exact && !probability.samples && probability.minimum == 2 && probability.maximum == 12, "2d6 should have an exact distribution.");
    Near(probability.mean, 7, "Exact 2d6 mean failed.");
    Near(probability.distribution.at(7), 1.0 / 6.0, "Exact 2d6 frequency failed.");
    Near(probability.matchedProportion, 21.0 / 36.0, "Exact target probability failed.");
    request = MakeRequest(L"d2+d3+4");
    probability = Analyze(request);
    Near(probability.mean, 7.5, "Mixed exact mean failed.");
    Near(probability.distribution.at(7), 1.0 / 3.0, "Mixed exact probability failed.");
    request = MakeRequest(L"2d2kh1");
    probability = Analyze(request);
    Near(probability.distribution.at(2), 0.75, "Keep-highest exact enumeration failed.");
    request = MakeRequest(L"2d2kl1");
    Near(Analyze(request).distribution.at(1), 0.75, "Keep-lowest exact enumeration failed.");
    request = MakeRequest(L"d2");
    request.mode = Mode::Advantage;
    Near(Analyze(request).distribution.at(2), 0.75, "Exact advantage transform failed.");
    request.mode = Mode::Disadvantage;
    Near(Analyze(request).distribution.at(1), 0.75, "Exact disadvantage transform failed.");
    request = MakeRequest(L"2d6");
    request.spec.successThreshold = 4;
    probability = Analyze(request);
    Near(probability.distribution.at(1), 0.5, "Exact binomial dice pool failed.");
    request = MakeRequest(L"3d6kh2");
    request.spec.successThreshold = 4;
    Near(Analyze(request).distribution.at(2), 0.5, "Keep-high dice pool probability failed.");
    request = MakeRequest(L"3d6kl2");
    request.spec.successThreshold = 4;
    Near(Analyze(request).distribution.at(0), 0.5, "Keep-low dice pool probability failed.");
    request.spec.successThreshold = 7;
    probability = Analyze(request);
    Check(probability.minimum == 0 && probability.maximum == 0, "Impossible pool threshold bounds failed.");
    Near(probability.distribution.at(0), 1, "Impossible pool threshold must have zero successes.");
    request.spec.successThreshold = 1;
    Near(Analyze(request).distribution.at(2), 1, "Certain dice-pool successes failed.");
    CheckExactAgainstEnumeration(MakeRequest(L"1d2+2d3kh1-2"));
    request = MakeRequest(L"3d3kl2");
    request.spec.successThreshold = 2;
    CheckExactAgainstEnumeration(request);
    request = MakeRequest(L"d3+2d2kh1");
    request.mode = Mode::Advantage;
    CheckExactAgainstEnumeration(request);
    request = MakeRequest(L"1000d1");
    request.times = MaxRolls;
    Near(Analyze(request).distribution.at(1000), 1, "Single-roll probability must not depend on batch workload.");
    request = MakeRequest(L"1d2147483647");
    request.target = {true, Compare::Greater, 1000000000};
    probability = Analyze(request);
    const auto repeat = Analyze(request);
    Check(!probability.exact && probability.samples >= 1000 && probability.marginOfError95 > 0 &&
        probability.distribution == repeat.distribution && probability.minimum == 1 && probability.maximum == INT_MAX,
        "Monte Carlo metadata, theoretical bounds or reproducibility failed.");
    double mass = 0;
    for (const auto& value : probability.distribution) mass += value.second;
    Near(mass, 1, "Sampled distribution is not normalized.", 1e-8);
    Near(probability.matchedProportion, (2147483647.0 - 1000000000.0) / 2147483647.0,
        "Monte Carlo result is outside its conservative error bound.", probability.marginOfError95);
    bool cancelled = false;
    try { Analyze(request, [] { return true; }); } catch (const Cancelled&) { cancelled = true; }
    Check(cancelled, "Probability analysis ignored cancellation.");
    int calls = 0;
    cancelled = false;
    try { Analyze(MakeRequest(L"10d6kh3"), [&] { return ++calls > 50; }); } catch (const Cancelled&) { cancelled = true; }
    Check(cancelled, "Sampling did not check cancellation during calculation.");
}

#ifdef DICE_ANALYSIS_STANDALONE
#include <iostream>
int main() {
    try { AnalysisTests(); std::cout << "Analysis and rules tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
#endif
