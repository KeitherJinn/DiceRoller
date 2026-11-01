#pragma once
#include <windows.h>
#include "HistoryStore.h"
#include "PresetExchange.h"
#include <optional>
#include <functional>

namespace ui {
struct HistorySelection {
    enum class Action { None, Reuse, Export } action = Action::None;
    std::vector<std::size_t> indices;
    std::vector<dice::HistorySummary> selected;
    std::optional<dice::Request> request;
    // Latest metadata after a successful reload/archive; may include archived rows.
    std::vector<dice::HistorySummary> index;
    bool indexChanged = false;
};
HistorySelection ShowHistory(HWND owner, const std::vector<dice::HistoryEntry>& entries);
HistorySelection ShowHistory(HWND owner, const std::filesystem::path& root,
    std::vector<dice::HistorySummary> index, const std::vector<dice::HistoryEntry>& dirty = {},
    const std::function<void()>& exportPreflight = {},
    const std::vector<dice::HistorySummary>& pendingExportMarks = {});
void ShowProbability(HWND owner, const dice::Request& request);
std::optional<dice::ImportStrategy> ChooseImportStrategy(HWND owner, const dice::ImportPreview& preview);
// Full single-roll detail is paged by the history window, never truncated.
std::wstring FullRollDetail(const dice::HistoryEntry& entry, std::size_t index);
std::wstring BatchSummary(const dice::HistoryEntry& entry);
}
