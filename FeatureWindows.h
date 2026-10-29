#pragma once
#include <windows.h>
#include "HistoryStore.h"
#include "PresetExchange.h"
#include <optional>

namespace ui {
struct HistorySelection {
    enum class Action { None, Reuse, Export } action = Action::None;
    std::vector<std::size_t> indices;
};
HistorySelection ShowHistory(HWND owner, const std::vector<dice::HistoryEntry>& entries);
void ShowProbability(HWND owner, const dice::Request& request);
std::optional<dice::ImportStrategy> ChooseImportStrategy(HWND owner, const dice::ImportPreview& preview);
// Full single-roll detail is paged by the history window, never truncated.
std::wstring FullRollDetail(const dice::HistoryEntry& entry, std::size_t index);
std::wstring BatchSummary(const dice::HistoryEntry& entry);
}
