#include "DiceCore.h"
#include <algorithm>
#include <limits>
#include <numeric>

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
std::int64_t Modifier(const std::wstring& text, bool negative) {
    const auto max = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    const auto value = Digits(text, max + (negative ? 1 : 0));
    if (negative && value == max + 1) return (std::numeric_limits<std::int64_t>::min)();
    return negative ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value);
}
Term ParseTerm(const std::wstring& text) {
    const auto d = text.find_first_of(L"dD");
    if (d == std::wstring::npos || text.find_first_of(L"dD", d + 1) != std::wstring::npos)
        throw std::invalid_argument("Use expressions such as 2d6+d4+3 or 4d6kh3.");
    Term term;
    const auto count = Trim(text.substr(0, d));
    term.count = count.empty() ? 1 : static_cast<int>(Digits(count, MaxDice));
    const auto right = Trim(text.substr(d + 1));
    const auto keep = right.find_first_of(L"kK");
    term.sides = static_cast<int>(Digits(Trim(right.substr(0, keep)), (std::numeric_limits<int>::max)()));
    if (keep != std::wstring::npos) {
        if (keep + 1 >= right.size() || (right[keep + 1] != L'h' && right[keep + 1] != L'H' &&
            right[keep + 1] != L'l' && right[keep + 1] != L'L'))
            throw std::invalid_argument("Use khN to keep the highest or klN to keep the lowest dice.");
        term.keepHighest = right[keep + 1] == L'h' || right[keep + 1] == L'H';
        term.keep = static_cast<int>(Digits(Trim(right.substr(keep + 2)), MaxDice));
        if (!term.keep) throw std::invalid_argument("The number of dice to keep must be positive.");
    }
    return term;
}
Group RollGroup(const Spec& spec, const NextDie& next, const CancelCheck& cancelled) {
    Group group;
    group.dice.reserve(spec.count);
    group.sides.reserve(spec.count);
    group.kept.reserve(spec.count);
    for (const auto& term : DiceTerms(spec)) {
        const auto begin = group.dice.size();
        for (int i = 0; i < term.count; ++i) {
            if (cancelled && cancelled()) throw Cancelled();
            const int value = next(term.sides);
            if (value < 1 || value > term.sides)
                throw std::runtime_error("Random source returned an invalid die value.");
            group.dice.push_back(value);
            group.sides.push_back(term.sides);
            group.kept.push_back(!term.keep);
        }
        if (term.keep) {
            std::vector<std::size_t> order(static_cast<std::size_t>(term.count));
            std::iota(order.begin(), order.end(), begin);
            // Stable ties preserve generation order, making stored audit data replayable.
            std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                return term.keepHighest ? group.dice[a] > group.dice[b] : group.dice[a] < group.dice[b];
            });
            for (int i = 0; i < term.keep; ++i) group.kept[order[static_cast<std::size_t>(i)]] = true;
        }
        for (std::size_t i = begin; i < group.dice.size(); ++i) {
            if (!group.kept[i]) continue;
            const auto value = spec.successThreshold > 0 ? (group.dice[i] >= spec.successThreshold ? 1 : 0) : group.dice[i];
            group.total = CheckedAdd(group.total, value);
        }
    }
    return group;
}
}

std::vector<Term> DiceTerms(const Spec& spec) {
    return spec.terms.empty() ? std::vector<Term>{{spec.count, spec.sides, 0, true}} : spec.terms;
}

Spec Parse(const std::wstring& notation) {
    const auto text = Trim(notation);
    if (text.empty() || text.size() > 4096)
        throw std::invalid_argument("Enter a dice expression of at most 4,096 characters.");
    Spec spec;
    std::vector<Term> terms;
    std::size_t start = 0;
    bool negative = false;
    int count = 0;
    while (start < text.size()) {
        const auto separator = text.find_first_of(L"+-", start);
        const auto part = Trim(text.substr(start, separator == std::wstring::npos ? separator : separator - start));
        if (part.empty()) throw std::invalid_argument("A dice term or modifier is missing.");
        if (part.find_first_of(L"dD") != std::wstring::npos) {
            if (negative) throw std::invalid_argument("Dice terms must be added; subtraction is supported for integer modifiers only.");
            const auto term = ParseTerm(part);
            if (term.count > MaxDice - count) throw std::invalid_argument("An expression may generate at most 1,000 dice per group.");
            count += term.count;
            terms.push_back(term);
        } else {
            if (terms.empty()) throw std::invalid_argument("Start the expression with a dice term such as d20.");
            spec.modifier = CheckedAdd(spec.modifier, Modifier(part, negative));
        }
        if (separator == std::wstring::npos) break;
        negative = text[separator] == L'-';
        start = separator + 1;
        if (start == text.size()) throw std::invalid_argument("A dice term or modifier is missing.");
    }
    spec.count = count;
    spec.sides = terms.front().sides;
    if (terms.size() > 1 || terms.front().keep) spec.terms = std::move(terms);
    Validate(spec, 1, Mode::Normal);
    return spec;
}

std::wstring CanonicalNotation(const Spec& spec) {
    Validate(spec, 1, Mode::Normal);
    std::wstring result;
    for (const auto& term : DiceTerms(spec)) {
        if (!result.empty()) result += L'+';
        result += std::to_wstring(term.count) + L'd' + std::to_wstring(term.sides);
        if (term.keep) result += (term.keepHighest ? L"kh" : L"kl") + std::to_wstring(term.keep);
    }
    if (spec.modifier > 0) result += L'+';
    if (spec.modifier) result += std::to_wstring(spec.modifier);
    return result;
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
    if (spec.count < 1 || spec.count > MaxDice || spec.sides < 1 || spec.terms.size() > MaxDice)
        throw std::invalid_argument("Use 1-1,000 dice with 1-2,147,483,647 sides.");
    if (times < 1 || times > MaxRolls)
        throw std::invalid_argument("Times must be between 1 and 10,000.");
    if (mode != Mode::Normal && mode != Mode::Advantage && mode != Mode::Disadvantage)
        throw std::invalid_argument("Unknown roll mode.");
    if (spec.successThreshold < 0 || (spec.successThreshold > 0 && spec.modifier != 0))
        throw std::invalid_argument("Dice pools require a positive success threshold and cannot use a modifier.");
    int generated = 0;
    std::int64_t minimum = 0, maximum = 0;
    for (const auto& term : DiceTerms(spec)) {
        if (term.count < 1 || term.count > MaxDice || term.sides < 1 || term.keep < 0 || term.keep > term.count ||
            term.count > MaxDice - generated)
            throw std::invalid_argument("Each dice term must have valid sides and keep 1 to its dice count; total dice cannot exceed 1,000.");
        generated += term.count;
        const int retained = term.keep ? term.keep : term.count;
        if (spec.successThreshold > 0) {
            minimum += spec.successThreshold == 1 ? retained : 0;
            maximum += spec.successThreshold <= term.sides ? retained : 0;
        } else {
            minimum += retained;
            maximum += static_cast<std::int64_t>(retained) * term.sides;
        }
    }
    if (generated != spec.count || (!spec.terms.empty() && spec.sides != spec.terms.front().sides))
        throw std::invalid_argument("The expression summary does not match its dice terms.");
    const auto work = static_cast<std::int64_t>(generated) * times * (mode == Mode::Normal ? 1 : 2);
    if (work > MaxWork)
        throw std::invalid_argument("A batch may generate at most 1,000,000 dice (including both groups).");
    CheckedAdd(maximum, spec.modifier);
    CheckedAdd(minimum, spec.modifier);
}

void Validate(const Request& request) {
    const auto controls = [](const std::wstring& text) {
        return std::any_of(text.begin(), text.end(), [](wchar_t ch) { return ch < 32 || ch == 127; });
    };
    if (Trim(request.name).empty() || request.name.size() > 128 || controls(request.name))
        throw std::invalid_argument("Name must contain 1-128 characters on one line.");
    if (Trim(request.notation).empty() || request.notation.size() > 1024 || controls(request.notation))
        throw std::invalid_argument("The dice expression must contain 1-1,024 characters on one line.");
    Validate(request.spec, request.times, request.mode);
    if (CanonicalNotation(Parse(request.notation)) != CanonicalNotation(request.spec))
        throw std::invalid_argument("The displayed expression does not match the dice rules.");
    if (request.target.comparison != Compare::AtLeast && request.target.comparison != Compare::AtMost &&
        request.target.comparison != Compare::Equal && request.target.comparison != Compare::Greater &&
        request.target.comparison != Compare::Less)
        throw std::invalid_argument("Unknown target comparison.");
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
