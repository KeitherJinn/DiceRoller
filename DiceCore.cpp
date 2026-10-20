#include "DiceCore.h"
#include <algorithm>
#include <limits>

namespace dice {
std::wstring Trim(const std::wstring& value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    return value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1);
}

namespace {
std::uint64_t Digits(const std::wstring& text, std::uint64_t limit) {
    if (text.empty()) throw std::invalid_argument("A number is missing.");
    std::uint64_t result = 0;
    for (wchar_t ch : text) {
        if (ch < L'0' || ch > L'9')
            throw std::invalid_argument("Use decimal integers without internal spaces.");
        const auto digit = static_cast<unsigned>(ch - L'0');
        if (result > limit / 10 || (result == limit / 10 && digit > limit % 10))
            throw std::out_of_range("A number exceeds the supported range.");
        result = result * 10 + digit;
    }
    return result;
}
std::int64_t Modifier(const std::wstring& text) {
    const bool negative = text.front() == L'-';
    const auto max = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    const auto value = Digits(Trim(text.substr(1)), max + (negative ? 1 : 0));
    if (negative && value == max + 1) return (std::numeric_limits<std::int64_t>::min)();
    return negative ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value);
}
Group RollGroup(const Spec& spec, const NextDie& next, const CancelCheck& cancelled) {
    Group group;
    group.dice.reserve(spec.count);
    for (int i = 0; i < spec.count; ++i) {
        if (cancelled && cancelled()) throw Cancelled();
        const int value = next(spec.sides);
        if (value < 1 || value > spec.sides)
            throw std::runtime_error("Random source returned an invalid die value.");
        group.dice.push_back(value);
        group.total = CheckedAdd(group.total, value);
    }
    return group;
}
}

Spec Parse(const std::wstring& notation) {
    const auto text = Trim(notation);
    const auto d = text.find_first_of(L"dD");
    if (d == std::wstring::npos || text.find_first_of(L"dD", d + 1) != std::wstring::npos)
        throw std::invalid_argument("Use dice notation such as 2d6+3, d20-1 or 3D8.");
    Spec spec;
    const auto count = Trim(text.substr(0, d));
    spec.count = count.empty() ? 1 : static_cast<int>(Digits(count, MaxDice));
    const auto right = Trim(text.substr(d + 1));
    const auto sign = right.find_first_of(L"+-");
    spec.sides = static_cast<int>(Digits(Trim(right.substr(0, sign)),
                                        (std::numeric_limits<int>::max)()));
    if (sign != std::wstring::npos) spec.modifier = Modifier(right.substr(sign));
    Validate(spec, 1, Mode::Normal);
    return spec;
}

int ParseTimes(const std::wstring& text) {
    const auto count = static_cast<int>(Digits(Trim(text), MaxRolls));
    if (count == 0) throw std::invalid_argument("Times must be between 1 and 10,000.");
    return count;
}

std::int64_t CheckedAdd(std::int64_t a, std::int64_t b) {
    if ((b > 0 && a > (std::numeric_limits<std::int64_t>::max)() - b) ||
        (b < 0 && a < (std::numeric_limits<std::int64_t>::min)() - b))
        throw std::overflow_error("The result exceeds the 64-bit integer range.");
    return a + b;
}

void Validate(const Spec& spec, int times, Mode mode) {
    if (spec.count < 1 || spec.count > MaxDice || spec.sides < 1)
        throw std::invalid_argument("Use 1-1,000 dice with 1-2,147,483,647 sides.");
    if (times < 1 || times > MaxRolls)
        throw std::invalid_argument("Times must be between 1 and 10,000.");
    if (mode != Mode::Normal && mode != Mode::Advantage && mode != Mode::Disadvantage)
        throw std::invalid_argument("Unknown roll mode.");
    const auto work = static_cast<std::int64_t>(spec.count) * times * (mode == Mode::Normal ? 1 : 2);
    if (work > MaxWork)
        throw std::invalid_argument("A batch may generate at most 1,000,000 dice (including both groups).");
    CheckedAdd(static_cast<std::int64_t>(spec.count) * spec.sides, spec.modifier);
    CheckedAdd(spec.count, spec.modifier);
}

void Validate(const Request& request) {
    if (Trim(request.name).empty() || request.name.size() > 128 ||
        request.name.find_first_of(L"\r\n\t") != std::wstring::npos ||
        request.name.find(L'\0') != std::wstring::npos)
        throw std::invalid_argument("Name must contain 1-128 characters on one line.");
    Validate(request.spec, request.times, request.mode);
}

Result Roll(const Spec& spec, Mode mode, const NextDie& next, const CancelCheck& cancelled) {
    Validate(spec, 1, mode);
    Result result;
    result.first = RollGroup(spec, next, cancelled);
    result.chosen = result.first.total;
    if (mode != Mode::Normal) {
        result.second = RollGroup(spec, next, cancelled);
        result.chosen = mode == Mode::Advantage
            ? (std::max)(result.first.total, result.second->total)
            : (std::min)(result.first.total, result.second->total);
    }
    result.finalValue = CheckedAdd(result.chosen, spec.modifier);
    return result;
}

const char* ModeName(Mode mode) {
    switch (mode) {
    case Mode::Normal: return "Normal";
    case Mode::Advantage: return "Advantage";
    case Mode::Disadvantage: return "Disadvantage";
    }
    throw std::invalid_argument("Unknown roll mode.");
}
}
