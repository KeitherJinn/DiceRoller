#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace dice {
constexpr int MaxDice = 1000;
constexpr int MaxRolls = 10000;
constexpr std::int64_t MaxWork = 1000000;

enum class Mode { Normal, Advantage, Disadvantage };
struct Term {
    int count = 1;
    int sides = 6;
    int keep = 0; // Zero retains every die.
    bool keepHighest = true;
};
struct Spec {
    int count = 1;
    int sides = 6;
    std::int64_t modifier = 0;
    // Empty for the original single-group form. Otherwise count is the sum of
    // term counts and sides is the first term's sides (a compatibility summary).
    std::vector<Term> terms;
    int successThreshold = 0; // Positive: count retained dice >= this value.
};
enum class Compare { AtLeast, AtMost, Equal, Greater, Less };
struct Target {
    bool enabled = false;
    Compare comparison = Compare::AtLeast;
    std::int64_t value = 0;
};
struct Request {
    std::wstring name;
    std::wstring notation;
    Spec spec;
    Mode mode = Mode::Normal;
    int times = 1;
    bool showTimestamp = false;
    Target target;
};
struct Group {
    std::vector<int> dice;
    std::int64_t total = 0;
    std::vector<bool> kept;
    std::vector<int> sides;
};
struct Result {
    Group first;
    std::optional<Group> second;
    std::int64_t chosen = 0;
    std::int64_t finalValue = 0;
    std::string timestamp;
};
struct Batch {
    Request request;
    std::vector<Result> results;
};
struct Cancelled : std::exception {
    const char* what() const noexcept override { return "Operation cancelled."; }
};
using NextDie = std::function<int(int sides)>;
using CancelCheck = std::function<bool()>;

std::wstring Trim(const std::wstring& value);
Spec Parse(const std::wstring& notation);
std::vector<Term> DiceTerms(const Spec& spec);
std::wstring CanonicalNotation(const Spec& spec);
int ParseTimes(const std::wstring& text);
void Validate(const Spec& spec, int times, Mode mode);
void Validate(const Request& request);
std::int64_t CheckedAdd(std::int64_t a, std::int64_t b);
Result Roll(const Spec& spec, Mode mode, const NextDie& next,
            const CancelCheck& cancelled = {});
const char* ModeName(Mode mode);
}
