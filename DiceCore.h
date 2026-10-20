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
struct Spec {
    int count = 1;
    int sides = 6;
    std::int64_t modifier = 0;
};
struct Request {
    std::wstring name;
    std::wstring notation;
    Spec spec;
    Mode mode = Mode::Normal;
    int times = 1;
    bool showTimestamp = false;
};
struct Group {
    std::vector<int> dice;
    std::int64_t total = 0;
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
int ParseTimes(const std::wstring& text);
void Validate(const Spec& spec, int times, Mode mode);
void Validate(const Request& request);
std::int64_t CheckedAdd(std::int64_t a, std::int64_t b);
Result Roll(const Spec& spec, Mode mode, const NextDie& next,
            const CancelCheck& cancelled = {});
const char* ModeName(Mode mode);
}
